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
    // // // info("%lu: REQ %s to address %lu.", req.cycle, AccessTypeName(req.type), req.lineAddr);
    DataLine data = gm_calloc<uint8_t>(zinfo->lineSize);
    DataType type = ZSIM_FLOAT;
    DataValue min, max;
    bool approximate = false;
    uint64_t Evictions = 0;
    uint64_t readAddress = req.lineAddr;
    if (zinfo->realAddresses->find(req.lineAddr) != zinfo->realAddresses->end())
        readAddress = (*zinfo->realAddresses)[req.lineAddr];
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
                // // // info("uCREATE: %p at %u", dUp, __LINE__);
                dUp->setMinStartCycle(startCycle);
                startEv->addChild(dUp, evRec)->addChild(r->startEvent, evRec);
            } else {
                startEv->addChild(r->startEvent, evRec);
            }

            if (downLat) {
                DelayEvent* dDown = new (evRec) DelayEvent(downLat);
                // // // info("uCREATE: %p at %u", dDown, __LINE__);
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
                // // // info("uCREATE: %p at %u", dEv, __LINE__);
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
        // info("%lu: REQ %s to address %lu", req.cycle, AccessTypeName(req.type), req.lineAddr << lineBits);
        // info("Req data type: %s, data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t tagId = tagArray->lookup(req.lineAddr, &req, updateReplacement);

        MissStartEvent* mse;
        MissResponseEvent* mre;
        MissWritebackEvent* mwe;
        if (tagId == -1) {
            if (tag_misses) tag_misses->inc();
            if (approximate) {
                 // info("\tApproximate Tag Miss");
                // Exact line, free a data line and a tag for it.
                respCycle += accLat;
                evictCycle += accLat;
                assert(cc->shouldAllocate(req));

                // Get the eviction candidate
                Address wbLineAddr;
                int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
                trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

                // Need to evict the tag.
                // info("\t\tEvicting tagId: %i", victimTagId);
                tagEvDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evictCycle);
                // // // info("\t\t\tEviction finished at %lu", tagEvDoneCycle);
                int32_t newLLHead;
                bool approximateVictim;
                bool evictDataLine = tagArray->evictAssociatedData(victimTagId, &newLLHead, &approximateVictim);
                int32_t victimDataId = tagArray->readMapId(victimTagId);
                if (evictDataLine) {
                    // info("\t\tAlong with dataId: %i", victimDataId);
                    // Clear (Evict, Tags already evicted) data line
                    dataArray->postinsert(-1, &req, victimDataId, -1, false, false);
                } else if (newLLHead != -1) {
                    // Change Tag
                    uint32_t victimMap = dataArray->readMap(victimDataId);
                    dataArray->changeInPlace(victimMap, &req, victimDataId, newLLHead, approximateVictim, false);
                }
                if (evRec->hasRecord()) {
                    Evictions++;
                    tagWritebackRecord.clear();
                    tagWritebackRecord = evRec->popRecord();
                }
                uint32_t map = dataArray->calculateMap(data, type, min, max);
                // // // info("\tMiss Req data type: %s, data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);
                // info("\tMiss data map: %u", map);
                int32_t mapId = dataArray->lookup(map, &req, updateReplacement);

                // Need to get the line we want
                uint64_t getDoneCycle = respCycle;
                respCycle = cc->processAccess(req, victimTagId, respCycle, &getDoneCycle);
                tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                if (evRec->hasRecord()) accessRecord = evRec->popRecord();

                if (mapId != -1) {
                    // info("\t\tSimilar map at: %i", mapId);
                    // Found similar mtag, insert tag to the LL.
                    int32_t oldListHead = dataArray->readListHead(mapId);
                    tagArray->postinsert(req.lineAddr, &req, victimTagId, mapId, oldListHead, true, updateReplacement);
                    dataArray->postinsert(map, &req, mapId, victimTagId, true, updateReplacement);

                    assert_msg(getDoneCycle == respCycle, "gdc %ld rc %ld", getDoneCycle, respCycle);

                    mse = new (evRec) MissStartEvent(this, accLat, domain);
                    // // // info("uCREATE: %p at %u", mse, __LINE__);
                    mre = new (evRec) MissResponseEvent(this, mse, domain);
                    // // // info("uCREATE: %p at %u", mre, __LINE__);
                    mwe = new (evRec) MissWritebackEvent(this, mse, accLat, domain);
                    // // // info("uCREATE: %p at %u", mwe, __LINE__);

                    mse->setMinStartCycle(req.cycle);
                    // // // info("\t\t\tMiss Start Event: %lu, %u", req.cycle, accLat);
                    mre->setMinStartCycle(respCycle);
                    // // // info("\t\t\tMiss Response Event: %lu", respCycle);
                    mwe->setMinStartCycle(MAX(respCycle, tagEvDoneCycle));
                    // // // info("\t\t\tMiss writeback event: %lu, %u", MAX(respCycle, tagEvDoneCycle), accLat);

                    connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + accLat, respCycle);
                    mre->addChild(mwe, evRec);
                    if (tagEvDoneCycle) {
                        connect(tagWritebackRecord.isValid()? &tagWritebackRecord : nullptr, mse, mwe, req.cycle + accLat, tagEvDoneCycle);
                    }
                } else {
                    // info("\t\tNo similar map");
                    // allocate new data/mtag and evict another if necessary,
                    // evict the tags associated with it too.
                    evictCycle = respCycle + accLat;
                    int32_t victimListHeadId, newVictimListHeadId;
                    int32_t victimDataId = dataArray->preinsert(map, &req, &victimListHeadId);
                    // info("\t\tEvicting Data line %i", victimDataId);
                    uint64_t evBeginCycle = evictCycle;
                    uint64_t evDoneCycle = evictCycle;
                    TimingRecord writebackRecord;
                    uint64_t lastEvDoneCycle = tagEvDoneCycle;

                    while (victimListHeadId != -1) {
                        if (victimListHeadId != victimTagId) {
                            Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                            // info("\t\talong with tagId: %i", victimListHeadId);
                            evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                            // // // info("\t\t\tEviction finished at %lu", evDoneCycle);
                            newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                            tagArray->postinsert(0, &req, victimListHeadId, -1, -1, false, false);
                        } else {
                            newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                        }
                        if (evRec->hasRecord()) {
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
                    // // // info("uCREATE: %p at %u", mse, __LINE__);
                    mre = new (evRec) MissResponseEvent(this, mse, domain);
                    // // // info("uCREATE: %p at %u", mre, __LINE__);
                    mwe = new (evRec) MissWritebackEvent(this, mse, accLat, domain);
                    // // // info("uCREATE: %p at %u", mwe, __LINE__);

                    mse->setMinStartCycle(req.cycle);
                    // // // info("\t\t\tMiss Start Event: %lu, %u", req.cycle, accLat);
                    mre->setMinStartCycle(respCycle);
                    // // // info("\t\t\tMiss Response Event: %lu", respCycle);
                    mwe->setMinStartCycle(MAX(lastEvDoneCycle, tagEvDoneCycle));
                    // // // info("\t\t\tMiss writeback event: %lu, %u", MAX(lastEvDoneCycle, tagEvDoneCycle), accLat);

                    connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + accLat, respCycle);
                    if(wbStartCycles.size()) {
                        for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                            DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - respCycle);
                            // // // info("uCREATE: %p at %u", del, __LINE__);
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
                // Miss, no tags found.
                // info("\tExact Tag Miss");
                respCycle += accLat;
                evictCycle += accLat;
                assert(cc->shouldAllocate(req));

                // Get the eviction candidate
                Address wbLineAddr;
                int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
                trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

                // Need to evict the tag.
                // info("\t\tEvicting tagId: %i", victimTagId);
                tagEvDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evictCycle);
                // // // info("\t\t\tEviction finished at %lu", tagEvDoneCycle);
                int32_t newLLHead;
                bool approximateVictim;
                bool evictDataLine = tagArray->evictAssociatedData(victimTagId, &newLLHead, &approximateVictim);
                int32_t victimDataId = tagArray->readMapId(victimTagId);
                bool Free = false;
                if (evictDataLine) {
                    // info("\t\tAlong with dataId: %i", victimDataId);
                    // Clear (Evict, Tags already evicted) data line
                    dataArray->postinsert(-1, &req, victimDataId, -1, false, false);
                    Free = true;
                } else if (newLLHead != -1) {
                    // Change Tag
                    uint32_t victimMap = dataArray->readMap(victimDataId);
                    dataArray->changeInPlace(victimMap, &req, victimDataId, newLLHead, approximateVictim, false);
                }
                if (evRec->hasRecord()) {
                    Evictions++;
                    tagWritebackRecord.clear();
                    tagWritebackRecord = evRec->popRecord();
                }
                // Need to get the line we want
                uint64_t getDoneCycle = respCycle;
                respCycle = cc->processAccess(req, victimTagId, respCycle, &getDoneCycle);
                tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                if (evRec->hasRecord()) accessRecord = evRec->popRecord();

                // Need to free a data line
                uint64_t evBeginCycle = 0;
                uint64_t lastEvDoneCycle = evictCycle;
                if (!Free) {
                    // srand (time(NULL));
                    int32_t victimListHeadId, newVictimListHeadId;
                    uint32_t map = rand() % (uint32_t)std::pow(2, zinfo->mapSize-1);
                    victimDataId = dataArray->preinsert(map, &req, &victimListHeadId);
                    // info("\t\tEvicting Data line %i", victimDataId);
                    evictCycle += accLat;
                    evBeginCycle = evictCycle;
                    uint64_t evDoneCycle = evictCycle;
                    TimingRecord writebackRecord;

                    while (victimListHeadId != -1) {
                        if (victimListHeadId != victimTagId) {
                            Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                            // info("\t\tAlong with tagId: %i", victimListHeadId);
                            evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                            // // // info("\t\t\tEviction finished at %lu", evDoneCycle);
                            newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                            tagArray->postinsert(0, &req, victimListHeadId, -1, -1, false, false);
                        } else {
                            newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                        }
                        if (evRec->hasRecord()) {
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
                }
                tagArray->postinsert(req.lineAddr, &req, victimTagId, victimDataId, -1, false, updateReplacement);
                dataArray->postinsert(-1, &req, victimDataId, victimTagId, false, updateReplacement);

                assert_msg(getDoneCycle == respCycle, "gdc %ld rc %ld", getDoneCycle, respCycle);
                mse = new (evRec) MissStartEvent(this, accLat, domain);
                // // // info("uCREATE: %p at %u", mse, __LINE__);
                mre = new (evRec) MissResponseEvent(this, mse, domain);
                // // // info("uCREATE: %p at %u", mre, __LINE__);
                mwe = new (evRec) MissWritebackEvent(this, mse, accLat, domain);
                // // // info("uCREATE: %p at %u", mwe, __LINE__);

                mse->setMinStartCycle(req.cycle);
                // // // info("\t\t\tMiss Start Event: %lu, %u", req.cycle, accLat);
                mre->setMinStartCycle(respCycle);
                // // // info("\t\t\tMiss Response Event: %lu", respCycle);
                mwe->setMinStartCycle(MAX(lastEvDoneCycle, tagEvDoneCycle));
                // // // info("\t\t\tMiss writeback event: %lu, %u", MAX(lastEvDoneCycle, tagEvDoneCycle), accLat);

                connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + accLat, respCycle);
                if(wbStartCycles.size()) {
                    for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                        DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - (req.cycle + accLat));
                        // // // info("uCREATE: %p at %u", del, __LINE__);
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
                // info("\tApproximate Write Tag Hit");
                // If this is a write
                uint32_t map = dataArray->calculateMap(data, type, min, max);
                // info("\tHit data map: %u", map);
                respCycle += accLat;
                // // // info("\tHit Req data type: %s, data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);
                // // info("\tHit PUTX Req map: %u, tagId = %i", map, tagId);
                uint32_t previousMap = dataArray->readMap(tagArray->readMapId(tagId));

                if (map == previousMap) {
                    // info("\tMap not different from before, nothing to do.");
                    // and its map is the same as before. Do nothing.
                    uint64_t getDoneCycle = respCycle;
                    respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                    if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                    tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                    dataArray->lookup(map, &req, updateReplacement);
                    HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                    // // // info("uCREATE: %p at %u", ev, __LINE__);
                    // // // info("\t\t\tHit Event: %lu, %lu", req.cycle, respCycle - req.cycle);
                    ev->setMinStartCycle(req.cycle);
                    tr.startEvent = tr.endEvent = ev;
                } else {
                    // and its map is not the same as before.
                    int32_t mapId = dataArray->lookup(map, &req, updateReplacement);
                    if (mapId != -1) {
                        // info("\tSimilar map at: %i", mapId);
                        // but is similar to something else that exists.
                        // we only need to add the tag to the existing linked
                        // list.
                        int32_t newLLHead;
                        bool approximateVictim;
                        bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead, &approximateVictim);
                        int32_t victimDataId = tagArray->readMapId(tagId);
                        if (evictDataLine) {
                            // info("\t\tEvicting dataId %i previously associated with tagId %i", victimDataId, tagId);
                            // Clear (Evict, Tags already evicted) data line
                            dataArray->postinsert(-1, &req, victimDataId, -1, false, false);
                        } else if (newLLHead != -1) {
                            // Change Tag
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
                        HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                        // // // info("uCREATE: %p at %u", ev, __LINE__);
                        // // // info("\t\t\tHit Event: %lu, %lu", req.cycle, respCycle - req.cycle);
                        ev->setMinStartCycle(req.cycle);
                        tr.startEvent = tr.endEvent = ev;
                    } else {
                        // info("\tNo similar map");
                        // and is also not similar to anything we have, we
                        // need to allocate new data, and evict another if we
                        // have to.
                        int32_t victimListHeadId, newVictimListHeadId;
                        int32_t victimDataId = dataArray->preinsert(map, &req, &victimListHeadId);
                        // info("\t\tEvicting Data line %i", victimDataId);
                        respCycle += accLat;
                        uint64_t wbStartCycle = respCycle;
                        uint64_t evDoneCycle = respCycle;
                        TimingRecord writebackRecord;

                        uint64_t lastEvDoneCycle = respCycle;

                        while (victimListHeadId != -1) {
                            if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                            if (victimListHeadId != tagId) {
                                Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                                // info("\t\tAlong with tagId %i", victimListHeadId);
                                evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, wbStartCycle);
                                // // // info("\t\t\tEviction finished at %lu", evDoneCycle);
                                newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                                tagArray->postinsert(0, &req, victimListHeadId, -1, -1, false, false);
                            } else {
                                newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                            }
                            if (evRec->hasRecord()) {
                                Evictions++;
                                writebackRecord.clear();
                                writebackRecord = evRec->popRecord();
                                writebackRecords.push_back(writebackRecord);
                                wbStartCycles.push_back(wbStartCycle);
                                wbStartCycle += accLat;
                                respCycle += accLat;
                                wbEndCycles.push_back(evDoneCycle);
                                lastEvDoneCycle = evDoneCycle;
                            }
                            victimListHeadId = newVictimListHeadId;
                        }

                        int32_t newLLHead;
                        bool approximateVictim;
                        bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead, &approximateVictim);
                        int32_t victimDataId2 = tagArray->readMapId(tagId);
                        if (evictDataLine) {
                            // info("\t\tClearing dataId %i previously associated with tagId %i", victimDataId2, tagId);
                            // Clear (Evict, Tags already evicted) data line
                            dataArray->postinsert(-1, &req, victimDataId2, -1, false, false);
                        } else if (newLLHead != -1) {
                            // Change Tag
                            uint32_t victimMap = dataArray->readMap(victimDataId2);
                            dataArray->changeInPlace(victimMap, &req, victimDataId2, newLLHead, approximateVictim, false);
                        }
                        tagArray->changeInPlace(req.lineAddr, &req, tagId, victimDataId, -1, true, false);
                        dataArray->postinsert(map, &req, victimDataId, tagId, true, false);
                        respCycle += accLat;

                        uint64_t getDoneCycle = respCycle;
                        respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                        if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                        tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};

                        HitEvent* he = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                        // // // info("uCREATE: %p at %u", he, __LINE__);
                        uHitWritebackEvent* hwe = new (evRec) uHitWritebackEvent(this, he, respCycle - req.cycle, domain);
                        // // // info("uCREATE: %p at %u", hwe, __LINE__);

                        he->setMinStartCycle(req.cycle);
                        hwe->setMinStartCycle(lastEvDoneCycle);
                        // // // info("\t\t\tHit Event: %lu, %lu", req.cycle, respCycle - req.cycle);
                        // // // info("\t\t\tHit writeback Event: %lu, %lu", lastEvDoneCycle, respCycle - req.cycle);

                        if(wbStartCycles.size()) {
                            for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                                DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - (req.cycle + accLat));
                                // // // info("uCREATE: %p at %u", del, __LINE__);
                                del->setMinStartCycle(req.cycle + accLat);
                                he->addChild(del, evRec);
                                connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, del, hwe, wbStartCycles[i], wbEndCycles[i]);
                            }
                        }
                        he->addChild(hwe, evRec);
                        tr.startEvent = tr.endEvent = he;
                    }
                }
            } else {
                // info("\tHit Req");
                dataArray->lookup(tagArray->readMapId(tagId), &req, updateReplacement);
                respCycle += accLat;
                uint64_t getDoneCycle = respCycle;
                respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                // // // info("uCREATE: %p at %u", ev, __LINE__);
                // // // info("\t\t\tHit Event: %lu, %lu", req.cycle, respCycle - req.cycle);
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
