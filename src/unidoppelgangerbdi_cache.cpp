#include "unidoppelgangerbdi_cache.h"
#include "pin.H"

#include <cstdlib>
#include <time.h>

uniDoppelgangerBDICache::uniDoppelgangerBDICache(uint32_t _numTagLines, uint32_t _numDataLines, CC* _cc, uniDoppelgangerBDITagArray* _tagArray,
uniDoppelgangerBDIDataArray* _dataArray, ReplPolicy* tagRP, ReplPolicy* dataRP, uint32_t _accLat, uint32_t _invLat, uint32_t mshrs, uint32_t ways,
uint32_t cands, uint32_t _domain, const g_string& _name, RunningStats* _crStats, RunningStats* _evStats, RunningStats* _tutStats, RunningStats* _dutStats, Counter* _tag_hits, Counter* _tag_misses, Counter* _tag_all)
: TimingCache(_numTagLines, _cc, NULL, tagRP, _accLat, _invLat, mshrs, tagLat, ways, cands, _domain, _name, _evStats, _tag_hits, _tag_misses, _tag_all), numTagLines(_numTagLines), numDataLines(_numDataLines),
tagArray(_tagArray), dataArray(_dataArray), tagRP(tagRP), dataRP(dataRP), crStats(_crStats), evStats(_evStats), tutStats(_tutStats), dutStats(_dutStats) {
    srand (time(NULL));
}

void uniDoppelgangerBDICache::initStats(AggregateStat* parentStat) {
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

void uniDoppelgangerBDICache::initCacheStats(AggregateStat* cacheStat) {
    cc->initStats(cacheStat);
    tagArray->initStats(cacheStat);
    dataArray->initStats(cacheStat);
    tagRP->initStats(cacheStat);
    dataRP->initStats(cacheStat);
}

uint64_t uniDoppelgangerBDICache::access(MemReq& req) {
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
    // // // info("\tData type: %s, Data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);

    EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
    assert_msg(evRec, "DoppelgangerBDI is not connected to TimingCore");

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

    // g_vector<uint32_t> keptFromEvictions;
    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        // info("%lu: REQ %s to address %lu in %s region", req.cycle, AccessTypeName(req.type), req.lineAddr << lineBits, approximate? "approximate":"exact");
        // info("Req data type: %s, data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t tagId = tagArray->lookup(req.lineAddr, &req, updateReplacement);
        respCycle += accLat;
        evictCycle += accLat;

        MissStartEvent* mse;
        MissResponseEvent* mre;
        MissWritebackEvent* mwe;
        if (tagId == -1) {
            if (tag_misses) tag_misses->inc();
            // info("\tTag Miss");
            assert(cc->shouldAllocate(req));
            // Get the eviction candidate
            Address wbLineAddr;
            int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
            // info("\t\tEvicting tagId: %i", victimTagId);
            // keptFromEvictions.push_back(victimTagId);
            trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);
            // Need to evict the tag.
            tagEvDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evictCycle);
            // // // // info("\t\t\tEviction finished at %lu", tagEvDoneCycle);
            int32_t newLLHead;
            bool dummy;
            bool evictDataLine = tagArray->evictAssociatedData(victimTagId, &newLLHead, &dummy);
            int32_t victimDataId = tagArray->readDataId(victimTagId);
            int32_t victimSegmentId = tagArray->readSegmentId(victimTagId);
            if (evictDataLine) {
                // info("\t\tAlong with dataId,segmenId of size %i segments: %i, %i", BDICompressionToSize(dataArray->readCompressionEncoding(victimDataId, victimSegmentId), zinfo->lineSize)/8, victimDataId, victimSegmentId);
                // Clear (Evict, Tags already evicted) data line
                dataArray->postinsert(-1, &req, victimDataId, victimSegmentId, -1, 0, NONE, false, updateReplacement);
                // // // info("SHOULD DOWN");
                tagArray->postinsert(0, &req, victimTagId, -1, -1, -1, false, false);
            } else if (newLLHead != -1) {
                // Change Tag
                // info("\t\tAnd decremented tag counter and decremented LL Head for dataId, SegmentId %i, %i", victimDataId, victimSegmentId);
                uint32_t victimCounter = dataArray->readCounter(victimDataId, victimSegmentId);
                uint32_t victimMap = dataArray->readMap(victimDataId, victimSegmentId);
                bool victimApproximate = dataArray->readApproximate(victimDataId, victimSegmentId);
                BDICompressionEncoding victimCompression= dataArray->readCompressionEncoding(victimDataId, victimSegmentId);
                dataArray->changeInPlace(victimMap, &req, victimDataId, victimSegmentId, newLLHead, victimCounter-1, victimCompression, victimApproximate, updateReplacement);
                // // // info("SHOULDN'T1");
                tagArray->postinsert(0, &req, victimTagId, -1, -1, -1, false, false);
            } else if (victimDataId != -1 && victimSegmentId != -1) {
                // info("\t\tAnd decremented dedup counter for dataId, segmentId %i, %i.", victimDataId, victimSegmentId);
                // // // info("SHOULDN'T2");
                uint32_t victimCounter = dataArray->readCounter(victimDataId, victimSegmentId);
                int32_t LLHead = dataArray->readListHead(victimDataId, victimSegmentId);
                uint32_t victimMap = dataArray->readMap(victimDataId, victimSegmentId);
                bool victimApproximate = dataArray->readApproximate(victimDataId, victimSegmentId);
                BDICompressionEncoding victimCompression= dataArray->readCompressionEncoding(victimDataId, victimSegmentId);
                dataArray->changeInPlace(victimMap, &req, victimDataId, victimSegmentId, LLHead, victimCounter-1, victimCompression, victimApproximate, updateReplacement);
                tagArray->postinsert(0, &req, victimTagId, -1, -1, -1, false, false);
            }
            if (evRec->hasRecord()) {
                // // info("\t\tEvicting tagId: %i", victimTagId);
                Evictions++;
                tagWritebackRecord.clear();
                tagWritebackRecord = evRec->popRecord();
            }

            // Need to get the line we want
            uint64_t getDoneCycle = respCycle;
            respCycle = cc->processAccess(req, victimTagId, respCycle, &getDoneCycle);
            tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
            if (evRec->hasRecord()) accessRecord = evRec->popRecord();
            
            uint16_t lineSize = 0;
            BDICompressionEncoding encoding = dataArray->compress(data, &lineSize);

            uint32_t map;
            if (approximate) {
                map = dataArray->calculateMap(data, type, min, max);
            } else {
                map = rand() % (uint32_t)std::pow(2, zinfo->mapSize-1);
            }
            // // // info("\tMiss Req data type: %s, data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);
            // info("\tMiss data map: %u", map);
            int32_t mapId = dataArray->lookup(map);
            int32_t segmentId = dataArray->lookup(map, mapId, &req, updateReplacement);

            if (segmentId != -1 && approximate) {
                // info("\t\tSimilar map at: %i", mapId);
                // Found similar mtag, insert tag to the LL.
                int32_t oldListHead = dataArray->readListHead(mapId, segmentId);
                uint32_t oldCounter = dataArray->readCounter(mapId, segmentId);
                BDICompressionEncoding compression = dataArray->readCompressionEncoding(mapId, segmentId);
                tagArray->postinsert(req.lineAddr, &req, victimTagId, mapId, segmentId, oldListHead, true, updateReplacement);
                dataArray->postinsert(map, &req, mapId, segmentId, victimTagId, oldCounter+1, compression, true, updateReplacement);

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
                int32_t victimDataId = dataArray->preinsert(map);

                // Now we need to know the available space in this set.
                uint16_t freeSpace = 0;
                g_vector<uint32_t> keptFromEvictions;
                uint64_t lastEvDoneCycle = evictCycle;
                uint64_t evBeginCycle = evictCycle;
                do {
                    uint16_t occupiedSpace = 0;
                    for (uint32_t i = 0; i < dataArray->getAssoc(); i++)
                        if (dataArray->readListHead(victimDataId, i) != -1)
                            occupiedSpace += BDICompressionToSize(dataArray->readCompressionEncoding(victimDataId, i), zinfo->lineSize);
                    freeSpace = (dataArray->getAssoc()/dataArray->getRatio())*zinfo->lineSize - occupiedSpace;
                    // info("\t\tFree Space %i segments", freeSpace/8);
                    // // info("Free %i, lineSize %i", freeSpace, lineSize);
                    int32_t victimListHeadId, newVictimListHeadId;
                    int32_t victimSegmentId = dataArray->preinsert(victimDataId, &req, &victimListHeadId, keptFromEvictions);
                    // uint32_t size = 0;

                    // info("%i, %i", victimDataId, victimSegmentId);
                    if (dataArray->readListHead(victimDataId, victimSegmentId) != -1) {
                        freeSpace += BDICompressionToSize(dataArray->readCompressionEncoding(victimDataId, victimSegmentId), zinfo->lineSize);
                        // size = BDICompressionToSize(dataArray->readCompressionEncoding(victimDataId, victimSegmentId), zinfo->lineSize)/8;
                    }
                    // info("\t\tEvicting dataline %i,%i", victimDataId, victimSegmentId);
                    keptFromEvictions.push_back(victimDataId*dataArray->getAssoc()+victimSegmentId);
                    uint64_t evDoneCycle = evBeginCycle;
                    TimingRecord writebackRecord;
                    lastEvDoneCycle = tagEvDoneCycle;
                    if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                    while (victimListHeadId != -1) {
                        if (victimListHeadId != victimTagId) {
                            // info("\t\tEvicting TagId: %i", victimListHeadId);
                            Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                            // // info("\t\tEvicting tagId: %i, %lu", victimListHeadId, wbLineAddr);
                            evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                            // // // info("\t\t\tEviction finished at %lu", evDoneCycle);
                            newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                            // // // info("SHOULDN'T/SHOULD DOWN");
                            tagArray->postinsert(0, &req, victimListHeadId, -1, -1, -1, false, false);
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
                    // info("\t\tand freed %i segments", size);
                    dataArray->postinsert(-1, &req, victimDataId, victimSegmentId, -1, 0, NONE, false, updateReplacement);
                } while (freeSpace < lineSize);

                // // // info("SHOULD UP");
                tagArray->postinsert(req.lineAddr, &req, victimTagId, victimDataId, keptFromEvictions[0]%dataArray->getAssoc(), -1, approximate, true);
                // // info("postinsert %i", victimTagId);
                if (approximate)
                    dataArray->postinsert(map, &req, victimDataId, keptFromEvictions[0]%dataArray->getAssoc(), victimTagId, 1, encoding, true, true);
                else 
                    dataArray->postinsert(-1, &req, victimDataId, keptFromEvictions[0]%dataArray->getAssoc(), victimTagId, 1, encoding, false, true);
                // hashArray->postinsert(hash, &req, victimDataId, hashId, true);
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
            tr.startEvent = mse;
            tr.endEvent = mre;
        } else {
            if (tag_hits) tag_hits->inc();
            if (req.type == PUTX) {
                // info("\tApproximate Write Tag Hit");
                // If this is a write
                uint32_t map;
                if (approximate) {
                    map = dataArray->calculateMap(data, type, min, max);
                } else {
                    map = rand() % (uint32_t)std::pow(2, zinfo->mapSize-1);
                }
                uint16_t lineSize = 0;
                BDICompressionEncoding encoding = dataArray->compress(data, &lineSize);
                // info("\tHit data map: %u", map);
                respCycle += accLat;
                // // // info("\tHit Req data type: %s, data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);
                // // info("\tHit PUTX Req map: %u, tagId = %i", map, tagId);
                uint32_t previousMap = dataArray->readMap(tagArray->readMapId(tagId), tagArray->readSegmentId(tagId));
                BDICompressionEncoding oldEncoding = dataArray->readCompressionEncoding(tagArray->readMapId(tagId), tagArray->readSegmentId(tagId));
                if ((approximate && map == previousMap) || (!approximate && oldEncoding == encoding)) {
                    // info("\tMap not different from before, nothing to do.");
                    // and its map is the same as before. Do nothing.
                    uint64_t getDoneCycle = respCycle;
                    respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                    if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                    tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                    dataArray->lookup(map, dataArray->lookup(map), &req, updateReplacement);
                    HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                    // // // info("uCREATE: %p at %u", ev, __LINE__);
                    // // // info("\t\t\tHit Event: %lu, %lu", req.cycle, respCycle - req.cycle);
                    ev->setMinStartCycle(req.cycle);
                    tr.startEvent = tr.endEvent = ev;
                } else {
                    int32_t targetDataId = dataArray->lookup(map);
                    int32_t targetSegmentId = dataArray->lookup(map, tagArray->readMapId(tagId), &req, updateReplacement);
                    int32_t newLLHead;
                    bool dummy;
                    bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead, &dummy);
                    int32_t victimDataId = tagArray->readDataId(tagId);
                    int32_t victimSegmentId = tagArray->readSegmentId(tagId);
                    if (evictDataLine) {
                        // info("\t\tAlong with dataId,segmenId of size %i segments: %i, %i", BDICompressionToSize(dataArray->readCompressionEncoding(victimDataId, victimSegmentId), zinfo->lineSize)/8, victimDataId, victimSegmentId);
                        // Clear (Evict, Tags already evicted) data line
                        dataArray->postinsert(-1, &req, victimDataId, victimSegmentId, -1, 0, NONE, false, updateReplacement);
                        // // // info("SHOULD DOWN");
                        tagArray->postinsert(0, &req, tagId, -1, -1, -1, false, false);
                    } else if (newLLHead != -1) {
                        // Change Tag
                        // info("\t\tAnd decremented tag counter and decremented LL Head for dataId, SegmentId %i, %i", victimDataId, victimSegmentId);
                        uint32_t victimCounter = dataArray->readCounter(victimDataId, victimSegmentId);
                        uint32_t victimMap = dataArray->readMap(victimDataId, victimSegmentId);
                        bool victimApproximate = dataArray->readApproximate(victimDataId, victimSegmentId);
                        BDICompressionEncoding victimCompression = dataArray->readCompressionEncoding(victimDataId, victimSegmentId);
                        dataArray->changeInPlace(victimMap, &req, victimDataId, victimSegmentId, newLLHead, victimCounter-1, victimCompression, victimApproximate, updateReplacement);
                        // // // info("SHOULDN'T1");
                        tagArray->postinsert(0, &req, tagId, -1, -1, -1, false, false);
                    } else if (victimDataId != -1 && victimSegmentId != -1) {
                        // info("\t\tAnd decremented dedup counter for dataId, segmentId %i, %i.", victimDataId, victimSegmentId);
                        // // // info("SHOULDN'T2");
                        uint32_t victimCounter = dataArray->readCounter(victimDataId, victimSegmentId);
                        int32_t LLHead = dataArray->readListHead(victimDataId, victimSegmentId);
                        uint32_t victimMap = dataArray->readMap(victimDataId, victimSegmentId);
                        bool victimApproximate = dataArray->readApproximate(victimDataId, victimSegmentId);
                        BDICompressionEncoding victimCompression = dataArray->readCompressionEncoding(victimDataId, victimSegmentId);
                        dataArray->changeInPlace(victimMap, &req, victimDataId, victimSegmentId, LLHead, victimCounter-1, victimCompression, victimApproximate, updateReplacement);
                        tagArray->postinsert(0, &req, tagId, -1, -1, -1, false, false);
                    }
                    if (approximate && targetSegmentId != -1) {
                        // // info("Data is also similar.");
                        int32_t oldListHead = dataArray->readListHead(targetDataId, targetSegmentId);
                        uint32_t oldCounter = dataArray->readCounter(targetDataId, targetSegmentId);
                        BDICompressionEncoding compression = dataArray->readCompressionEncoding(targetDataId, targetSegmentId);
                        tagArray->changeInPlace(req.lineAddr, &req, tagId, targetDataId, targetSegmentId, oldListHead, approximate, updateReplacement);
                        dataArray->postinsert(map, &req, targetDataId, targetSegmentId, tagId, oldCounter+1, compression, approximate, updateReplacement);

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
                        // info("\t\tNo similar map");
                        // allocate new data/mtag and evict another if necessary,
                        // evict the tags associated with it too.
                        evictCycle = respCycle + accLat;
                        int32_t victimDataId = dataArray->preinsert(map);

                        // Now we need to know the available space in this set.
                        uint16_t freeSpace = 0;
                        g_vector<uint32_t> keptFromEvictions;
                        uint64_t lastEvDoneCycle = evictCycle;
                        uint64_t evBeginCycle = evictCycle;
                        do {
                            uint16_t occupiedSpace = 0;
                            for (uint32_t i = 0; i < dataArray->getAssoc(); i++)
                                if (dataArray->readListHead(victimDataId, i) != -1)
                                    occupiedSpace += BDICompressionToSize(dataArray->readCompressionEncoding(victimDataId, i), zinfo->lineSize);
                            freeSpace = (dataArray->getAssoc()/dataArray->getRatio())*zinfo->lineSize - occupiedSpace;
                            // info("\t\tFree Space %i segments", freeSpace/8);
                            // // info("Free %i, lineSize %i", freeSpace, lineSize);
                            int32_t victimListHeadId, newVictimListHeadId;
                            int32_t victimSegmentId = dataArray->preinsert(victimDataId, &req, &victimListHeadId, keptFromEvictions);
                            // uint32_t size = 0;
                            if (dataArray->readListHead(victimDataId, victimSegmentId) != -1) {
                                freeSpace += BDICompressionToSize(dataArray->readCompressionEncoding(victimDataId, victimSegmentId), zinfo->lineSize);
                                // size = BDICompressionToSize(dataArray->readCompressionEncoding(victimDataId, victimSegmentId), zinfo->lineSize)/8;
                            }
                            // info("\t\tEvicting dataline %i,%i", victimDataId, victimSegmentId);

                            keptFromEvictions.push_back(victimDataId*dataArray->getAssoc()+victimSegmentId);
                            uint64_t evDoneCycle = evBeginCycle;
                            TimingRecord writebackRecord;
                            lastEvDoneCycle = tagEvDoneCycle;
                            if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                            while (victimListHeadId != -1) {
                                if (victimListHeadId != tagId) {
                                    // info("\t\tEvicting TagId: %i", victimListHeadId);
                                    Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                                    // // info("\t\tEvicting tagId: %i, %lu", victimListHeadId, wbLineAddr);
                                    evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                                    // // // info("\t\t\tEviction finished at %lu", evDoneCycle);
                                    newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                                    // // // info("SHOULDN'T/SHOULD DOWN");
                                    tagArray->postinsert(0, &req, victimListHeadId, -1, -1, -1, false, false);
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
                            // info("\t\tand freed %i segments", size);
                            dataArray->postinsert(-1, &req, victimDataId, victimSegmentId, -1, 0, NONE, false, updateReplacement);
                        } while (freeSpace < lineSize);

                        // // // info("SHOULD UP");
                        tagArray->postinsert(req.lineAddr, &req, tagId, victimDataId, keptFromEvictions[0]%dataArray->getAssoc(), -1, approximate, true);
                        // // info("postinsert %i", tagId);
                        dataArray->postinsert(map, &req, victimDataId, keptFromEvictions[0]%dataArray->getAssoc(), tagId, 1, encoding, approximate, true);
                                                uint64_t getDoneCycle = respCycle;
                        respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                        if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                        tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};

                        HitEvent* he = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                        // // // info("uCREATE: %p at %u", he, __LINE__);
                        udbHitWritebackEvent* hwe = new (evRec) udbHitWritebackEvent(this, he, respCycle - req.cycle, domain);
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
                // // info("\tHit Req");
                // dataArray->lookup(tagArray->readDataId(tagId), &req, updateReplacement);
                respCycle += accLat;
                uint64_t getDoneCycle = respCycle;
                respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                // // // info("uCREATE: %p at %u", ev, __LINE__);
                // // // // info("\t\t\tHit Event: %lu, %lu", req.cycle, respCycle - req.cycle);
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

    uint32_t dataValidSegments = 0;
    for (uint32_t i = 0; i < numDataLines/dataArray->getAssoc(); i++)
    {
        for (uint32_t j = 0; j < dataArray->getAssoc(); j++)
        {
            if (dataArray->readListHead(i, j) != -1)
                dataValidSegments += BDICompressionToSize(dataArray->readCompressionEncoding(i, j), zinfo->lineSize)/8;
        }
    }
    // info("Valid Tags: %u", tagArray->getValidLines());
    // info("Valid Segments: %u", dataArray->getValidSegments());
    // assert(tagArray->getValidLines() == tagArray->countValidLines());
    // assert(tagArray->getDataValidSegments() == dataValidSegments);
    // assert(tagArray->getValidLines() >= dataArray->getValidSegments()/8);
    // assert(tagArray->getValidLines() <= numTagLines);
    // assert(dataArray->getValidSegments() <= numDataLines*8);

    double sample = ((double)dataArray->getValidSegments()/8)/(double)tagArray->getValidLines();
    crStats->add(sample,1);

    if (req.type != PUTS) {
        sample = Evictions;
        evStats->add(sample,1);
    }

    sample = ((double)dataArray->getValidSegments()/8)/numDataLines;
    dutStats->add(sample, 1);

    sample = (double)tagArray->getValidLines()/numTagLines;
    tutStats->add(sample, 1);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}

void uniDoppelgangerBDICache::simulateHitWriteback(udbHitWritebackEvent* ev, uint64_t cycle, HitEvent* he) {
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
