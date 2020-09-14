#include "unidoppelganger_cache.h"
#include "pin.H"

#include <cstdlib>
#include <time.h>

uniDoppelgangerCache::uniDoppelgangerCache(uint32_t _numTagLines, uint32_t _numDataLines, CC* _cc, uniDoppelgangerTagArray* _tagArray,
uniDoppelgangerDataArray* _dataArray, ReplPolicy* tagRP, ReplPolicy* dataRP, uint32_t _accLat, uint32_t _invLat, uint32_t mshrs, uint32_t ways,
uint32_t cands, uint32_t _domain, const g_string& _name, RunningStats* _crStats, RunningStats* _evStats, RunningStats* _tutStats, RunningStats* _dutStats, Counter* _tag_hits, Counter* _tag_misses, Counter* _tag_all)
: TimingCache(_numTagLines, _cc, NULL, tagRP, _accLat, _invLat, mshrs, tagLat, ways, cands, _domain, _name, _evStats, _tag_hits, _tag_misses, _tag_all), numTagLines(_numTagLines), numDataLines(_numDataLines),
tagArray(_tagArray), dataArray(_dataArray), tagRP(tagRP), dataRP(dataRP), crStats(_crStats), evStats(_evStats), tutStats(_tutStats), dutStats(_dutStats) {
    srand (time(NULL));
}

void uniDoppelgangerCache::initStats(AggregateStat* parentStat) {
    AggregateStat* cacheStat = new AggregateStat();
    cacheStat->init(name.c_str(), "uniDoppelganger cache stats");
    initCacheStats(cacheStat);

    //Stats specific to timing cache
    profOccHist.init("occHist", "Occupancy MSHR cycle histogram", numMSHRs+1);
    cacheStat->append(&profOccHist);

    profHitLat.init("latHit", "Cumulative latency accesses that hit (demand and non-demand)");
    profMissRespLat.init("latMissResp", "Cumulative latency for miss start to response");
    profMissLat.init("latMiss", "Cumulative latency for miss start to finish (free MSHR)");

    cacheStat->append(&profHitLat);
    cacheStat->append(&profMissRespLat);
    cacheStat->append(&profMissLat);

    parentStat->append(cacheStat);
}

void uniDoppelgangerCache::initCacheStats(AggregateStat* cacheStat) {
    cc->initStats(cacheStat);
    tagArray->initStats(cacheStat);
    dataArray->initStats(cacheStat);
    tagRP->initStats(cacheStat);
    dataRP->initStats(cacheStat);
}

uint64_t uniDoppelgangerCache::access(MemReq& req) {
    if (tag_all) tag_all->inc();
    DataLine data = gm_calloc<uint8_t>(zinfo->lineSize);
    DataType type = ZSIM_FLOAT;
    DataValue min, max;
    bool approximate = false;
    uint64_t Evictions = 0;
    uint64_t readAddress = req.lineAddr;
    for(uint32_t i = 0; i < zinfo->approximateRegions->size(); i++) {
        if ((readAddress << lineBits) >= std::get<0>((*zinfo->approximateRegions)[i]) && (readAddress << lineBits) <= std::get<1>((*zinfo->approximateRegions)[i])
        && (readAddress << lineBits)+zinfo->lineSize-1 >= std::get<0>((*zinfo->approximateRegions)[i]) && (readAddress << lineBits)+zinfo->lineSize-1 <= std::get<1>((*zinfo->approximateRegions)[i])) {
            type = std::get<2>((*zinfo->approximateRegions)[i]);
            min = std::get<3>((*zinfo->approximateRegions)[i]);
            max = std::get<4>((*zinfo->approximateRegions)[i]);
            approximate = true;
            break;
        }
    }
    if (approximate)
        PIN_SafeCopy(data, (void*)(readAddress << lineBits), zinfo->lineSize);

    debug("%s: received %s %s req of data type %s on address %lu on cycle %lu", name.c_str(), (approximate? "approximate":""), AccessTypeName(req.type), DataTypeName(type), req.lineAddr, req.cycle);
    timing("%s: received %s req on address %lu on cycle %lu", name.c_str(), AccessTypeName(req.type), req.lineAddr, req.cycle);

    EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
    assert_msg(evRec, "uniDoppelgangerCache is not connected to TimingCore");

    // Tie two events to an optional timing record
    // TODO: Promote to evRec if this is more generally useful
    auto connect = [evRec](const TimingRecord* r, TimingEvent* startEv, TimingEvent* endEv, uint64_t startCycle, uint64_t endCycle) {
        assert_msg(startCycle <= endCycle, "start > end? %ld %ld", startCycle, endCycle);
        if (r) {
            assert_msg(startCycle <= r->reqCycle, "%ld / %ld", startCycle, r->reqCycle);
            assert_msg(r->respCycle <= endCycle, "%ld %ld %ld %ld", startCycle, r->reqCycle, r->respCycle, endCycle);
            uint64_t upLat = r->reqCycle - startCycle;
            uint64_t downLat = endCycle - r->respCycle;

            if (upLat) {
                DelayEvent* dUp = new (evRec) DelayEvent(upLat);
                dUp->setMinStartCycle(startCycle);
                startEv->addChild(dUp, evRec)->addChild(r->startEvent, evRec);
            } else {
                startEv->addChild(r->startEvent, evRec);
            }

            if (downLat) {
                DelayEvent* dDown = new (evRec) DelayEvent(downLat);
                dDown->setMinStartCycle(r->respCycle);
                r->endEvent->addChild(dDown, evRec)->addChild(endEv, evRec);
            } else {
                r->endEvent->addChild(endEv, evRec);
            }
        } else {
            if (startCycle == endCycle) {
                startEv->addChild(endEv, evRec);
            } else {
                DelayEvent* dEv = new (evRec) DelayEvent(endCycle - startCycle);
                dEv->setMinStartCycle(startCycle);
                startEv->addChild(dEv, evRec)->addChild(endEv, evRec);
            }
        }
    };

    TimingRecord tagWritebackRecord, accessRecord, tr;
    tagWritebackRecord.clear();
    accessRecord.clear();
    g_vector<TimingRecord> writebackRecords;
    g_vector<uint64_t> wbStartCycles;
    g_vector<uint64_t> wbEndCycles;
    uint64_t tagEvDoneCycle = 0;
    uint64_t respCycle = req.cycle;
    uint64_t evictCycle = req.cycle;

    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t tagId = tagArray->lookup(req.lineAddr, &req, updateReplacement);
        respCycle += accLat;
        evictCycle += accLat;
        timing("%s: tag accessed on cycle %lu", name.c_str(), respCycle);
        MissStartEvent* mse;
        MissResponseEvent* mre;
        MissWritebackEvent* mwe;
        if (tagId == -1) {
            if (tag_misses) tag_misses->inc();
            if (approximate) {
                debug("%s: approximate tag miss.", name.c_str());
                assert(cc->shouldAllocate(req));
                // Get the eviction candidate
                Address wbLineAddr;
                int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
                // Need to evict the tag.
                // Timing: to evict, need to read the data array too.
                evictCycle += accLat;
                timing("%s: tag access missed, evicting address %lu on cycle %lu", name.c_str(), wbLineAddr, evictCycle);
                tagEvDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evictCycle);
                timing("%s: finished eviction on cycle %lu", name.c_str(), tagEvDoneCycle);
                int32_t newLLHead;
                bool approximateVictim;
                bool evictDataLine = tagArray->evictAssociatedData(victimTagId, &newLLHead, &approximateVictim);
                int32_t victimDataId = tagArray->readMapId(victimTagId);
                if (evictDataLine) {
                    debug("%s: tag miss caused eviction of data line %i", name.c_str(), victimDataId);
                    // Clear (Evict, Tags already evicted) data line
                    dataArray->postinsert(-1, &req, victimDataId, -1, false, false);
                } else if (newLLHead != -1) {
                    debug("%s: tag miss caused dedup of data line %i to decrease", name.c_str(), victimDataId);
                    // Change Tag
                    uint32_t victimMap = dataArray->readMap(victimDataId);
                    dataArray->changeInPlace(victimMap, &req, victimDataId, newLLHead, approximateVictim, false);
                }
                if (evRec->hasRecord()) {
                    debug("%s: tag miss caused eviction of address %lu", name.c_str(), wbLineAddr);
                    Evictions++;
                    tagWritebackRecord.clear();
                    tagWritebackRecord = evRec->popRecord();
                }
                // Need to get the line we want
                uint64_t getDoneCycle = respCycle;
                timing("%s: doing processAccess on cycle %lu", name.c_str(), respCycle);
                respCycle = cc->processAccess(req, victimTagId, respCycle, &getDoneCycle);
                timing("%s: finished processAccess on cycle %lu", name.c_str(), respCycle);
                tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                if (evRec->hasRecord()) accessRecord = evRec->popRecord();

                uint32_t map = dataArray->calculateMap(data, type, min, max);
                debug("%s: data hashed to %u", name.c_str(), map);
                int32_t mapId = dataArray->lookup(map, &req, updateReplacement);

                if (mapId != -1) {
                    debug("%s: Found similar hash on line %i", name.c_str(), mapId);
                    int32_t oldListHead = dataArray->readListHead(mapId);
                    tagArray->postinsert(req.lineAddr, &req, victimTagId, mapId, oldListHead, true, updateReplacement);
                    dataArray->postinsert(map, &req, mapId, victimTagId, true, updateReplacement);
                    assert_msg(getDoneCycle == respCycle, "gdc %ld rc %ld", getDoneCycle, respCycle);
                    mse = new (evRec) MissStartEvent(this, accLat, domain);
                    mre = new (evRec) MissResponseEvent(this, mse, domain);
                    mwe = new (evRec) MissWritebackEvent(this, mse, accLat, domain);
                    mse->setMinStartCycle(req.cycle);
                    mre->setMinStartCycle(respCycle);
                    mwe->setMinStartCycle(MAX(respCycle, tagEvDoneCycle));
                    timing("%s: missStartEvent Min Start: %lu, duration: %u", name.c_str(), req.cycle, accLat);
                    timing("%s: missResponseEvent Min Start: %lu", name.c_str(), respCycle);
                    timing("%s: missWritebackEvent Min Start: %lu, duration: %u", name.c_str(), MAX(respCycle, tagEvDoneCycle), accLat);

                    connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + accLat, respCycle);
                    mre->addChild(mwe, evRec);
                    if (tagEvDoneCycle) {
                        connect(tagWritebackRecord.isValid()? &tagWritebackRecord : nullptr, mse, mwe, req.cycle + accLat, tagEvDoneCycle);
                    }
                } else {
                    debug("%s: Found no matching hash.", name.c_str());
                    // Timing: because no similar line was found, we need to read
                    // another victim data line, one more accLat for the data
                    // and another for the tag, all after recieving the response.
                    evictCycle = respCycle + 2*accLat;
                    timing("%s: Read victim line for eviction on cycle %lu", name.c_str(), evictCycle);
                    int32_t victimListHeadId, newVictimListHeadId;
                    int32_t victimDataId = dataArray->preinsert(map, &req, &victimListHeadId);
                    debug("%s: Picked victim data line %i", name.c_str(), victimDataId);
                    uint64_t evBeginCycle = evictCycle;
                    uint64_t evDoneCycle = evictCycle;
                    TimingRecord writebackRecord;
                    uint64_t lastEvDoneCycle = tagEvDoneCycle;
                    while (victimListHeadId != -1) {
                        if (victimListHeadId != victimTagId) {
                            Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                            timing("%s: dedup caused eviction, evicting address %lu on cycle %lu", name.c_str(), wbLineAddr, evBeginCycle);
                            evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                            timing("%s: finished eviction on cycle %lu", name.c_str(), evDoneCycle);
                            newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                            tagArray->postinsert(0, &req, victimListHeadId, -1, -1, false, false);
                        } else {
                            newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                        }
                        if (evRec->hasRecord()) {
                            debug("%s: dedup caused eviction from tagId %i for address %lu", name.c_str(), victimListHeadId, wbLineAddr);
                            Evictions++;
                            writebackRecord.clear();
                            writebackRecord = evRec->popRecord();
                            writebackRecords.push_back(writebackRecord);
                            wbStartCycles.push_back(evBeginCycle);
                            wbEndCycles.push_back(evDoneCycle);
                            lastEvDoneCycle = evDoneCycle;
                            evBeginCycle += accLat;
                        }
                        victimListHeadId = newVictimListHeadId;
                    }
                    tagArray->postinsert(req.lineAddr, &req, victimTagId, victimDataId, -1, true, updateReplacement);
                    dataArray->postinsert(map, &req, victimDataId, victimTagId, true, updateReplacement);
                    assert_msg(getDoneCycle == respCycle, "gdc %ld rc %ld", getDoneCycle, respCycle);

                    mse = new (evRec) MissStartEvent(this, accLat, domain);
                    mre = new (evRec) MissResponseEvent(this, mse, domain);
                    mwe = new (evRec) MissWritebackEvent(this, mse, accLat, domain);
                    mse->setMinStartCycle(req.cycle);
                    mre->setMinStartCycle(respCycle);
                    mwe->setMinStartCycle(MAX(lastEvDoneCycle, tagEvDoneCycle));
                    timing("%s: missStartEvent Min Start: %lu, duration: %u", name.c_str(), req.cycle, accLat);
                    timing("%s: missResponseEvent Min Start: %lu", name.c_str(), respCycle);
                    timing("%s: missWritebackEvent Min Start: %lu, duration: %u", name.c_str(), MAX(lastEvDoneCycle, tagEvDoneCycle), accLat);

                    connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + accLat, respCycle);
                    if(wbStartCycles.size()) {
                        for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                            DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - respCycle);
                            del->setMinStartCycle(respCycle);
                            mre->addChild(del, evRec);
                            connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, del, mwe, wbStartCycles[i], wbEndCycles[i]);
                        }
                    }
                    mre->addChild(mwe, evRec);
                    if (tagEvDoneCycle) {
                        connect(tagWritebackRecord.isValid()? &tagWritebackRecord : nullptr, mse, mwe, req.cycle + accLat, tagEvDoneCycle);
                    }
                }
            } else {
                debug("%s: exact tag miss.", name.c_str());

                // Miss, no tags found.
                assert(cc->shouldAllocate(req));

                // Get the eviction candidate
                Address wbLineAddr;
                int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
                trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

                // Need to evict the tag.
                timing("%s: tag access missed, evicting address %lu on cycle %lu", name.c_str(), wbLineAddr, evictCycle);
                tagEvDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evictCycle);
                timing("%s: finished eviction on cycle %lu", name.c_str(), tagEvDoneCycle);
                int32_t newLLHead;
                bool approximateVictim;
                bool evictDataLine = tagArray->evictAssociatedData(victimTagId, &newLLHead, &approximateVictim);
                int32_t victimDataId = tagArray->readMapId(victimTagId);
                if (evictDataLine) {
                    debug("%s: tag miss caused eviction of data line %i", name.c_str(), victimDataId);
                    // Clear (Evict, Tags already evicted) data line
                    dataArray->postinsert(-1, &req, victimDataId, -1, false, false);
                } else if (newLLHead != -1) {
                    debug("%s: tag miss caused dedup of data line %i to decrease", name.c_str(), victimDataId);
                    // Change Tag
                    uint32_t victimMap = dataArray->readMap(victimDataId);
                    dataArray->changeInPlace(victimMap, &req, victimDataId, newLLHead, approximateVictim, false);
                }
                if (evRec->hasRecord()) {
                    debug("%s: tag miss caused eviction of address %lu", name.c_str(), wbLineAddr);
                    Evictions++;
                    tagWritebackRecord.clear();
                    tagWritebackRecord = evRec->popRecord();
                }
                // Need to get the line we want
                uint64_t getDoneCycle = respCycle;
                timing("%s: doing processAccess on cycle %lu", name.c_str(), respCycle);
                respCycle = cc->processAccess(req, victimTagId, respCycle, &getDoneCycle);
                timing("%s: finished processAccess on cycle %lu", name.c_str(), respCycle);
                tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                if (evRec->hasRecord()) accessRecord = evRec->popRecord();

                // Need to free a data line
                uint64_t evBeginCycle = 0;
                uint64_t lastEvDoneCycle = evictCycle;
                int32_t victimListHeadId, newVictimListHeadId;
                uint32_t map = rand() % (uint32_t)std::pow(2, zinfo->mapSize-1);
                victimDataId = dataArray->preinsert(map, &req, &victimListHeadId);
                // Timing: because no similar line was found, we need to read
                // another victim data line, one more accLat for the data
                // and another for the tag, all after recieving the response.
                evictCycle = respCycle + 2*accLat;
                timing("%s: Read victim line for eviction on cycle %lu", name.c_str(), evictCycle);
                evBeginCycle = evictCycle;
                uint64_t evDoneCycle = evictCycle;
                TimingRecord writebackRecord;

                while (victimListHeadId != -1) {
                    if (victimListHeadId != victimTagId) {
                        Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                        timing("%s: dedup caused eviction, evicting address %lu on cycle %lu", name.c_str(), wbLineAddr, evBeginCycle);
                        evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                        timing("%s: finished eviction on cycle %lu", name.c_str(), evDoneCycle);
                        newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                        tagArray->postinsert(0, &req, victimListHeadId, -1, -1, false, false);
                    } else {
                        newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                    }
                    if (evRec->hasRecord()) {
                        debug("%s: dedup caused eviction from tagId %i for address %lu", name.c_str(), victimListHeadId, wbLineAddr);
                        Evictions++;
                        writebackRecord.clear();
                        writebackRecord = evRec->popRecord();
                        writebackRecords.push_back(writebackRecord);
                        wbStartCycles.push_back(evBeginCycle);
                        wbEndCycles.push_back(evDoneCycle);
                        lastEvDoneCycle = evDoneCycle;
                        evBeginCycle += accLat;
                    }
                    victimListHeadId = newVictimListHeadId;
                }
                tagArray->postinsert(req.lineAddr, &req, victimTagId, victimDataId, -1, false, updateReplacement);
                dataArray->postinsert(-1, &req, victimDataId, victimTagId, false, updateReplacement);

                assert_msg(getDoneCycle == respCycle, "gdc %ld rc %ld", getDoneCycle, respCycle);
                mse = new (evRec) MissStartEvent(this, accLat, domain);
                mre = new (evRec) MissResponseEvent(this, mse, domain);
                mwe = new (evRec) MissWritebackEvent(this, mse, accLat, domain);

                mse->setMinStartCycle(req.cycle);
                mre->setMinStartCycle(respCycle);
                mwe->setMinStartCycle(MAX(lastEvDoneCycle, tagEvDoneCycle));
                timing("%s: missStartEvent Min Start: %lu, duration: %u", name.c_str(), req.cycle, accLat);
                timing("%s: missResponseEvent Min Start: %lu", name.c_str(), respCycle);
                timing("%s: missWritebackEvent Min Start: %lu, duration: %u", name.c_str(), MAX(lastEvDoneCycle, tagEvDoneCycle), accLat);


                connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + accLat, respCycle);
                if(wbStartCycles.size()) {
                    for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                        DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - (req.cycle + accLat));
                        del->setMinStartCycle(req.cycle + accLat);
                        mse->addChild(del, evRec);
                        connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, del, mwe, wbStartCycles[i], wbEndCycles[i]);
                    }
                }
                mre->addChild(mwe, evRec);
                if (tagEvDoneCycle) {
                    connect(tagWritebackRecord.isValid()? &tagWritebackRecord : nullptr, mse, mwe, req.cycle + accLat, tagEvDoneCycle);
                }
            }
            tr.startEvent = mse;
            tr.endEvent = mre;
        } else {
            if (tag_hits) tag_hits->inc();
            if (approximate && req.type == PUTX) {
                debug("%s: Approximate Write Tag Hit", name.c_str());
                // If this is a write
                uint32_t map = dataArray->calculateMap(data, type, min, max);
                uint32_t previousMap = dataArray->readMap(tagArray->readMapId(tagId));
                debug("%s: hashed data to %u", name.c_str(), map);

                if (map == previousMap) {
                    debug("%s: hash is the same as before.", name.c_str());
                    respCycle += accLat;
                    uint64_t getDoneCycle = respCycle;
                    timing("%s: doing processAccess on cycle %lu", name.c_str(), respCycle);
                    respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                    timing("%s: finished processAccess on cycle %lu", name.c_str(), respCycle);
                    if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                    tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                    dataArray->lookup(map, &req, updateReplacement);
                    HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                    ev->setMinStartCycle(req.cycle);
                    timing("%s: hitEvent Min Start: %lu, duration: %lu", name.c_str(), req.cycle, respCycle - req.cycle);
                    tr.startEvent = tr.endEvent = ev;
                } else {
                    debug("%s: hash is different from before.", name.c_str());
                    // and its map is not the same as before.
                    int32_t mapId = dataArray->lookup(map, &req, updateReplacement);
                    if (mapId != -1) {
                        debug("%s: Found matching hash at %i", name.c_str(), mapId);
                        // but is similar to something else that exists.
                        // we only need to add the tag to the existing linked
                        // list.
                        int32_t newLLHead;
                        bool approximateVictim;
                        bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead, &approximateVictim);
                        int32_t victimDataId = tagArray->readMapId(tagId);
                        if (evictDataLine) {
                            debug("%s: old data line %i evicted", name.c_str(), mapId);
                            // Clear (Evict, Tags already evicted) data line
                            dataArray->postinsert(-1, &req, victimDataId, -1, false, false);
                        } else if (newLLHead != -1) {
                            // Change Tag
                            debug("%s: dedup of old data line %i decreased", name.c_str(), mapId);
                            uint32_t victimMap = dataArray->readMap(victimDataId);
                            dataArray->changeInPlace(victimMap, &req, victimDataId, newLLHead, approximateVictim, false);
                        }
                        int32_t oldListHead = dataArray->readListHead(mapId);
                        tagArray->changeInPlace(req.lineAddr, &req, tagId, mapId, oldListHead, true, false);
                        dataArray->postinsert(map, &req, mapId, tagId, true, false);
                        respCycle += accLat;
                        uint64_t getDoneCycle = respCycle;
                        respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                        if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                        tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                        HitEvent* he = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                        he->setMinStartCycle(req.cycle);
                        timing("%s: hitEvent Min Start: %lu, duration: %lu", name.c_str(), req.cycle, respCycle - req.cycle);
                        // Timing: even though this is a hit, we need to figure out if the
                        // line has changed from before. requires two more accLats to find that a
                        // line is similar and to actually update its dedup.
                        uHitWritebackEvent* hwe = new (evRec) uHitWritebackEvent(this, he, 2*accLat, domain);
                        hwe->setMinStartCycle(respCycle);
                        timing("%s: hitWritebackEvent Min Start: %lu, duration: %u", name.c_str(), respCycle, 2*accLat);

                        for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                            DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - (req.cycle + accLat));
                            del->setMinStartCycle(req.cycle + accLat);
                            he->addChild(del, evRec);
                            connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, del, hwe, wbStartCycles[i], wbEndCycles[i]);
                        }
                        he->addChild(hwe, evRec);
                        tr.startEvent = tr.endEvent = he;
                    } else {
                        debug("%s: Found no matching hash", name.c_str());
                        // and is also not similar to anything we have, we
                        // need to allocate new data, and evict another if we
                        // have to.
                        int32_t newLLHead;
                        bool approximateVictim;
                        bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead, &approximateVictim);
                        int32_t victimDataId2 = tagArray->readMapId(tagId);
                        if (evictDataLine) {
                            debug("%s: old data line %i evicted", name.c_str(), victimDataId2);
                            // Clear (Evict, Tags already evicted) data line
                            dataArray->postinsert(-1, &req, victimDataId2, -1, false, false);
                        } else if (newLLHead != -1) {
                            debug("%s: dedup of old data line %i decreased", name.c_str(), victimDataId2);
                            // Change Tag
                            uint32_t victimMap = dataArray->readMap(victimDataId2);
                            dataArray->changeInPlace(victimMap, &req, victimDataId2, newLLHead, approximateVictim, false);
                        }

                        // Timing: need to evict a victim dataLine, that
                        // means we need to read it's data, then tag
                        // first.
                        evictCycle = respCycle + 2*accLat;
                        timing("%s: Read victim line for eviction on cycle %lu", name.c_str(), evictCycle);

                        int32_t victimListHeadId, newVictimListHeadId;
                        int32_t victimDataId = dataArray->preinsert(map, &req, &victimListHeadId);
                        debug("%s: Picked victim data line %i", name.c_str(), victimDataId);
                        uint64_t wbStartCycle = respCycle;
                        uint64_t evDoneCycle = respCycle;
                        TimingRecord writebackRecord;
                        Address wbLineAddr;
                        while (victimListHeadId != -1) {
                            if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                            if (victimListHeadId != tagId) {
                                wbLineAddr = tagArray->readAddress(victimListHeadId);
                                timing("%s: dedup caused eviction, evicting address %lu on cycle %lu", name.c_str(), wbLineAddr, wbStartCycle);
                                evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, wbStartCycle);
                                timing("%s: finished eviction on cycle %lu", name.c_str(), wbStartCycle);
                                newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                                tagArray->postinsert(0, &req, victimListHeadId, -1, -1, false, false);
                            } else {
                                wbLineAddr = 0;
                                newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                            }
                            if (evRec->hasRecord()) {
                                debug("%s: dedup caused eviction from tagId %i for address %lu", name.c_str(), victimListHeadId, wbLineAddr);
                                Evictions++;
                                writebackRecord.clear();
                                writebackRecord = evRec->popRecord();
                                writebackRecords.push_back(writebackRecord);
                                wbStartCycles.push_back(wbStartCycle);
                                wbStartCycle += accLat;
                                wbEndCycles.push_back(evDoneCycle);
                            }
                            victimListHeadId = newVictimListHeadId;
                        }

                        tagArray->changeInPlace(req.lineAddr, &req, tagId, victimDataId, -1, true, false);
                        dataArray->postinsert(map, &req, victimDataId, tagId, true, false);
                        uint64_t getDoneCycle = respCycle;
                        timing("%s: doing processAccess on cycle %lu", name.c_str(), respCycle);
                        respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                        timing("%s: finished processAccess on cycle %lu", name.c_str(), respCycle);
                        if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                        tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};

                        HitEvent* he = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                        he->setMinStartCycle(req.cycle);
                        timing("%s: hitEvent Min Start: %lu, duration: %lu", name.c_str(), req.cycle, respCycle - req.cycle);
                        // Timing: even though this is a hit, we need to figure out if the
                        // line has changed from before. requires two more accLats to find that a
                        // line is similar and to actually update its dedup.
                        uHitWritebackEvent* hwe = new (evRec) uHitWritebackEvent(this, he, 2*accLat, domain);
                        hwe->setMinStartCycle(respCycle);
                        timing("%s: hitWritebackEvent Min Start: %lu, duration: %u", name.c_str(), respCycle, 2*accLat);

                        for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                            DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - (req.cycle + accLat));
                            del->setMinStartCycle(req.cycle + accLat);
                            he->addChild(del, evRec);
                            connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, del, hwe, wbStartCycles[i], wbEndCycles[i]);
                        }
                        he->addChild(hwe, evRec);
                        tr.startEvent = tr.endEvent = he;
                    }
                }
            } else {
                debug("%s: read hit, or exact write hit.", name.c_str());
                dataArray->lookup(tagArray->readMapId(tagId), &req, updateReplacement);
                respCycle += accLat;
                timing("%s: reading data on cycle %lu", name.c_str(), respCycle);
                uint64_t getDoneCycle = respCycle;
                timing("%s: doing processAccess on cycle %lu", name.c_str(), respCycle);
                respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                timing("%s: finished processAccess on cycle %lu", name.c_str(), respCycle);
                if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                timing("%s: hitEvent Min Start: %lu, duration: %lu", name.c_str(), req.cycle, respCycle - req.cycle);
                ev->setMinStartCycle(req.cycle);
                tr.startEvent = tr.endEvent = ev;
            }
        }
        gm_free(data);
        evRec->pushRecord(tr);

        // tagArray->print();
        // dataArray->print();
    }

    cc->endAccess(req);

    // info("Valid Tags: %u", tagArray->getValidLines());
    // info("Valid Lines: %u", dataArray->getValidLines());
    // assert(tagArray->getValidLines() == tagArray->countValidLines());
    // assert(dataArray->getValidLines() == dataArray->countValidLines());
    assert(tagArray->getValidLines() >= dataArray->getValidLines());
    assert(tagArray->getValidLines() <= numTagLines);
    assert(dataArray->getValidLines() <= numDataLines);
    double sample = (double)dataArray->getValidLines()/(double)tagArray->getValidLines();
    crStats->add(sample,1);

    if (req.type != PUTS) {
        sample = Evictions;
        evStats->add(sample,1);
    }

    sample = (double)dataArray->getValidLines()/numDataLines;
    dutStats->add(sample, 1);

    sample = (double)tagArray->getValidLines()/numTagLines;
    tutStats->add(sample, 1);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}

void uniDoppelgangerCache::simulateHitWriteback(uHitWritebackEvent* ev, uint64_t cycle, HitEvent* he) {
    uint64_t lookupCycle = tryLowPrioAccess(cycle);
    if (lookupCycle) { //success, release MSHR
        if (!pendingQueue.empty()) {
            //// // info("XXX %ld elems in pending queue", pendingQueue.size());
            for (TimingEvent* qev : pendingQueue) {
                qev->requeue(cycle+1);
            }
            pendingQueue.clear();
        }
        ev->done(cycle);
    } else {
        ev->requeue(cycle+1);
    }
}
