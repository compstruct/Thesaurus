#include "approximateidealdedupbdi_cache.h"
#include "pin.H"

ApproximateIdealDedupBDICache::ApproximateIdealDedupBDICache(uint32_t _numTagLines, uint32_t _numDataLines, CC* _cc, ApproximateDedupBDITagArray* _tagArray, ApproximateDedupBDIDataArray* _dataArray, ApproximateDedupBDIHashArray* _hashArray, ReplPolicy* tagRP,
ReplPolicy* dataRP, ReplPolicy* hashRP, uint32_t _accLat, uint32_t _invLat, uint32_t mshrs, uint32_t ways, uint32_t cands, uint32_t _domain, const g_string& _name, RunningStats* _crStats,
RunningStats* _evStats, RunningStats* _tutStats, RunningStats* _dutStats, Counter* _tag_hits, Counter* _tag_misses, Counter* _all_misses) : TimingCache(_numTagLines, _cc, NULL, tagRP, _accLat, _invLat, mshrs, tagLat, ways, cands, _domain, _name, _evStats, _tag_hits, _tag_misses, _all_misses), numTagLines(_numTagLines),
numDataLines(_numDataLines), dataAssoc(ways), tagArray(_tagArray), dataArray(_dataArray), hashArray(_hashArray), tagRP(tagRP), dataRP(dataRP), hashRP(hashRP), crStats(_crStats), evStats(_evStats), tutStats(_tutStats), dutStats(_dutStats) {
    dataArray->assignTagArray(tagArray);
    hashArray->registerDataArray(dataArray);
    TM_DS = 0;
    TM_DD = 0;
    WD_TH_DS = 0;
    WD_TH_DD_1 = 0;
    WD_TH_DD_M = 0;
    WSR_TH = 0;
    DS_HI = 0;
    DS_HS = 0;
    DS_HD = 0;
    DD_HI = 0;
    DD_HD = 0;
    g_string statName = name + g_string(" Deduplication Average");
    dupStats = new RunningStats(statName);
    statName = name + g_string(" Data Size Average");
    bdiStats = new RunningStats(statName);
}

void ApproximateIdealDedupBDICache::initStats(AggregateStat* parentStat) {
    AggregateStat* cacheStat = new AggregateStat();
    cacheStat->init(name.c_str(), "Approximate BDI cache stats");
    initCacheStats(cacheStat);

    //Stats specific to timing cacheStat
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

void ApproximateIdealDedupBDICache::initCacheStats(AggregateStat* cacheStat) {
    cc->initStats(cacheStat);
    tagArray->initStats(cacheStat);
    tagRP->initStats(cacheStat);
    dataArray->initStats(cacheStat);
    // dataRP->initStats(cacheStat);
    hashRP->initStats(cacheStat);
}

uint64_t ApproximateIdealDedupBDICache::access(MemReq& req) {
    if (tag_all) tag_all->inc();
    DataLine data = gm_calloc<uint8_t>(zinfo->lineSize);
    DataType type = ZSIM_FLOAT;
    bool approximate = false;
    uint64_t Evictions = 0;
    uint64_t readAddress = req.lineAddr;
    for(uint32_t i = 0; i < zinfo->approximateRegions->size(); i++) {
        if ((readAddress << lineBits) >= std::get<0>((*zinfo->approximateRegions)[i]) && (readAddress << lineBits) <= std::get<1>((*zinfo->approximateRegions)[i])
        && (readAddress << lineBits)+zinfo->lineSize-1 >= std::get<0>((*zinfo->approximateRegions)[i]) && (readAddress << lineBits)+zinfo->lineSize-1 <= std::get<1>((*zinfo->approximateRegions)[i])) {
            type = std::get<2>((*zinfo->approximateRegions)[i]);
            approximate = true;
            break;
        }
    }
    PIN_SafeCopy(data, (void*)(readAddress << lineBits), zinfo->lineSize);
    // // // info("\tData type: %s, Data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);

    debug("%s: received %s %s req of data type %s on address %lu on cycle %lu", name.c_str(), (approximate? "approximate":""), AccessTypeName(req.type), DataTypeName(type), req.lineAddr, req.cycle);
    timing("%s: received %s req on address %lu on cycle %lu", name.c_str(), AccessTypeName(req.type), req.lineAddr, req.cycle);

    EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
    assert_msg(evRec, "ApproximateDedupBDI is not connected to TimingCore");

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
        zinfo->tagAll++;
        respCycle += accLat;
        evictCycle += accLat;
        timing("%s: tag accessed on cycle %lu", name.c_str(), respCycle);

        MissStartEvent* mse;
        MissResponseEvent* mre;
        MissWritebackEvent* mwe;
        if (tagId == -1) {
            if(tag_misses) tag_misses->inc();
            zinfo->tagMisses++;
            assert(cc->shouldAllocate(req));
            // Get the eviction candidate
            Address wbLineAddr;
            int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
            debug("%s: tag miss, inserting into line %i", name.c_str(), tagId);
            // Need to evict the tag.
            // Timing: to evict, need to read the data array too.
            evictCycle += accLat;
            timing("%s: tag access missed, evicting address %lu on cycle %lu", name.c_str(), wbLineAddr, evictCycle);
            tagEvDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evictCycle);
            timing("%s: finished eviction on cycle %lu", name.c_str(), tagEvDoneCycle);
            int32_t newLLHead;
            bool evictDataLine = tagArray->evictAssociatedData(victimTagId, &newLLHead);
            int32_t victimDataId = tagArray->readDataId(victimTagId);
            int32_t victimSegmentId = tagArray->readSegmentPointer(victimTagId);
            // Timing: in any of the following cases, an extra data access is
            // required to zero or change the counters or update the freeList.
            // this was not needed in conventional and BDI because tags and
            // data are 1 to 1 (at least sets). which is not the case here.
            // FIXME: I'm ignoring this delay for now. it looks like it needs
            // an extra event?
            if (evictDataLine) {
                debug("%s: tag miss caused eviction of data line %i, segment %i", name.c_str(), victimDataId, victimSegmentId);
                // Clear (Evict, Tags already evicted) data line
                dataArray->postinsert(-1, &req, 0, victimDataId, victimSegmentId, NULL, false);
            } else if (newLLHead != -1) {
                debug("%s: tag miss caused dedup of data line %i, segment %i to decrease", name.c_str(), victimDataId, victimSegmentId);
                // Change Tag
                uint32_t victimCounter = dataArray->readCounter(victimDataId, victimSegmentId);
                dataArray->changeInPlace(newLLHead, &req, victimCounter-1, victimDataId, victimSegmentId, NULL, false);
            } else if (victimDataId != -1 && victimSegmentId != -1) {
                uint32_t victimCounter = dataArray->readCounter(victimDataId, victimSegmentId);
                int32_t LLHead = dataArray->readListHead(victimDataId, victimSegmentId);
                debug("%s: tag miss caused dedup of data line %i, segment %i to decrease and LL to change to %i", name.c_str(), victimDataId, victimSegmentId, LLHead);
                dataArray->changeInPlace(LLHead, &req, victimCounter-1, victimDataId, victimSegmentId, NULL, false);
            }
            tagArray->postinsert(0, &req, victimTagId, -1, -1, NONE, -1, false);
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

            if(approximate)
                hashArray->approximate(data, type);
            int32_t dataId = -1;
            int32_t segmentId = -1;
            for (uint32_t i = 0; i < numDataLines/dataAssoc; i++) {
                for (uint32_t j = 0; j < dataAssoc*8; j++) {
                    if (dataArray->readCounter(i, j) && dataArray->isSame(i, j, data)) {
                        dataId = i;
                        segmentId = j;
                        break;
                    }
                }
            }
            uint16_t lineSize = 0;
            BDICompressionEncoding encoding = dataArray->compress(data, &lineSize);
            debug("%s: compressed data to %i segments", name.c_str(), lineSize/8);
            uint64_t hash = hashArray->hash(data);
            int32_t hashId = hashArray->lookup(hash, &req, false);

            if (dataId != -1) {
                if (hashId == -1) {
                    DS_HI++;
                    hashId = hashArray->preinsert(hash, &req);
                    if(hashId != -1)
                        hashArray->postinsert(hash, &req, dataId, segmentId, hashId, true);
                } else if (hashArray->readDataPointer(hashId) == dataId && hashArray->readSegmentPointer(hashId) == segmentId) {
                    DS_HS++;
                } else {
                    DS_HD++;
                    if(dataArray->readCounter(hashArray->readDataPointer(hashId), hashArray->readSegmentPointer(hashId)) == 1)
                        hashArray->postinsert(hash, &req, dataId, segmentId, hashId, true);
                }
                debug("%s: Found matching data line %i, segment %i.", name.c_str(), dataId, segmentId);
                TM_DS++;
                int32_t oldListHead = dataArray->readListHead(dataId, segmentId);
                uint32_t dataCounter = dataArray->readCounter(dataId, segmentId);
                tagArray->postinsert(req.lineAddr, &req, victimTagId, dataId, segmentId, encoding, oldListHead, true);
                dataArray->changeInPlace(victimTagId, &req, dataCounter+1, dataId, segmentId, NULL, updateReplacement);

                assert_msg(getDoneCycle == respCycle, "gdc %ld rc %ld", getDoneCycle, respCycle);

                mse = new (evRec) MissStartEvent(this, accLat, domain);
                mre = new (evRec) MissResponseEvent(this, mse, domain);
                // Timing: Writeback is 2 accLat, one to find out lines
                // are similar and the other to update dedup info.
                mwe = new (evRec) MissWritebackEvent(this, mse, 2*accLat, domain);

                mse->setMinStartCycle(req.cycle);
                mre->setMinStartCycle(respCycle);
                mwe->setMinStartCycle(MAX(respCycle, tagEvDoneCycle));
                timing("%s: missStartEvent Min Start: %lu, duration: %u", name.c_str(), req.cycle, accLat);
                timing("%s: missResponseEvent Min Start: %lu", name.c_str(), respCycle);
                timing("%s: missWritebackEvent Min Start: %lu, duration: %u", name.c_str(), MAX(respCycle, tagEvDoneCycle), 2*accLat);

                connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + accLat, respCycle);
                mre->addChild(mwe, evRec);
                if (tagEvDoneCycle) {
                    DelayEvent* del = new (evRec) DelayEvent(accLat);
                    del->setMinStartCycle(req.cycle + accLat);
                    mse->addChild(del, evRec);
                    connect(tagWritebackRecord.isValid()? &tagWritebackRecord : nullptr, del, mwe, req.cycle + 2*accLat, tagEvDoneCycle);
                }
            } else {
                TM_DD++;
                debug("%s: Found no matching line.", name.c_str());
                // Select data to evict
                evictCycle = respCycle + 2*accLat;
                int32_t victimDataId = dataArray->preinsert(lineSize);
                timing("%s: Read victim line for eviction on cycle %lu", name.c_str(), evictCycle);
                debug("%s: Picked victim data line %i", name.c_str(), victimDataId);

                // Now we need to know the available space in this set.
                uint16_t freeSpace = 0;
                g_vector<uint32_t> keptFromEvictions;
                uint64_t lastEvDoneCycle = evictCycle;
                uint64_t evBeginCycle = evictCycle;
                do {
                    uint16_t occupiedSpace = 0;
                    for (uint32_t i = 0; i < dataArray->getAssoc()*zinfo->lineSize/8; i++)
                        if (dataArray->readListHead(victimDataId, i) != -1)
                            occupiedSpace += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, i)), zinfo->lineSize);
                    freeSpace = dataArray->getAssoc()*zinfo->lineSize - occupiedSpace;
                    debug("%s: line now has %i segments free.", name.c_str(), freeSpace/8);
                    int32_t victimListHeadId, newVictimListHeadId;
                    int32_t victimSegmentId = dataArray->preinsert(victimDataId, &victimListHeadId, keptFromEvictions);
                    if (dataArray->readListHead(victimDataId, victimSegmentId) != -1) {
                        freeSpace += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, victimSegmentId)), zinfo->lineSize);
                    }
                    debug("%s: Picked victim segment %i", name.c_str(), victimSegmentId);
                    keptFromEvictions.push_back(victimSegmentId);
                    uint64_t evDoneCycle = evBeginCycle;
                    TimingRecord writebackRecord;
                    lastEvDoneCycle = tagEvDoneCycle;
                    if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                    while (victimListHeadId != -1) {
                        if (victimListHeadId != victimTagId) {
                            Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                            timing("%s: doing size/dedup eviction for address %lu on cycle %lu", name.c_str(), wbLineAddr, evBeginCycle);
                            evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                            timing("%s: size/dedup eviction finished on cycle %lu", name.c_str(), evDoneCycle);
                            newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                            tagArray->postinsert(0, &req, victimListHeadId, -1, -1, NONE, -1, false);
                        } else {
                            newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                        }
                        if (evRec->hasRecord()) {
                            debug("%s: size/dedup eviction of %i segments from tagId %i for address %lu", name.c_str(), BDICompressionToSize(tagArray->readCompressionEncoding(victimListHeadId), zinfo->lineSize)/8, victimListHeadId, wbLineAddr);
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
                    dataArray->postinsert(-1, &req, 0, victimDataId, victimSegmentId, NULL, false);
                } while (freeSpace < lineSize);
                tagArray->postinsert(req.lineAddr, &req, victimTagId, victimDataId, keptFromEvictions[0], encoding, -1, true);
                dataArray->postinsert(victimTagId, &req, 1, victimDataId, keptFromEvictions[0], data, updateReplacement);
                if (hashId == -1) {
                    DD_HI++;
                    hashId = hashArray->preinsert(hash, &req);
                    if(hashId != -1)
                        hashArray->postinsert(hash, &req, victimDataId, keptFromEvictions[0], hashId, true);
                } else {
                    DD_HD++;
                    if(dataArray->readCounter(hashArray->readDataPointer(hashId), hashArray->readSegmentPointer(hashId)) == 1)
                        hashArray->postinsert(hash, &req, victimDataId, keptFromEvictions[0], hashId, true);
                }

                assert_msg(getDoneCycle == respCycle, "gdc %ld rc %ld", getDoneCycle, respCycle);
                mse = new (evRec) MissStartEvent(this, accLat, domain);
                mre = new (evRec) MissResponseEvent(this, mse, domain);
                // Timing: Writeback is 2 accLat, one to read the line and
                // find out it's different, and the other to write to the
                // victim.
                mwe = new (evRec) MissWritebackEvent(this, mse, 2*accLat, domain);
                mse->setMinStartCycle(req.cycle);
                mre->setMinStartCycle(respCycle);
                mwe->setMinStartCycle(MAX(lastEvDoneCycle, tagEvDoneCycle));
                timing("%s: missStartEvent Min Start: %lu, duration: %u", name.c_str(), req.cycle, accLat);
                timing("%s: missResponseEvent Min Start: %lu", name.c_str(), respCycle);
                timing("%s: missWritebackEvent Min Start: %lu, duration: %u", name.c_str(), MAX(lastEvDoneCycle, tagEvDoneCycle), 2*accLat);

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
                    DelayEvent* del = new (evRec) DelayEvent(accLat);
                    del->setMinStartCycle(req.cycle + accLat);
                    mse->addChild(del, evRec);
                    connect(tagWritebackRecord.isValid()? &tagWritebackRecord : nullptr, del, mwe, req.cycle + 2*accLat, tagEvDoneCycle);
                }
            }
            tr.startEvent = mse;
            tr.endEvent = mre;
        } else {
            if (tag_hits) tag_hits->inc();
            debug("%s: tag hit on line %i", name.c_str(), tagId);
            zinfo->tagHits++;
            if(approximate)
                hashArray->approximate(data, type);
            uint16_t lineSize = 0;
            BDICompressionEncoding encoding = dataArray->compress(data, &lineSize);
            int32_t dataId = tagArray->readDataId(tagId);
            int32_t segmentId = tagArray->readSegmentPointer(tagId);
            debug("%s: compressed data to %i segments", name.c_str(), lineSize/8);
            if (req.type == PUTX && !dataArray->isSame(dataId, segmentId, data)) {
                debug("%s: write data is found different from before on cycle %lu.", name.c_str(), respCycle);
                int32_t targetDataId = -1;
                int32_t targetSegmentId = -1;
                uint64_t hash = hashArray->hash(data);
                int32_t hashId = hashArray->lookup(hash, &req, false);
                for (uint32_t i = 0; i < numDataLines/dataAssoc; i++) {
                    for (uint32_t j = 0; j < dataAssoc*8; j++) {
                        if (dataArray->readCounter(i, j) && dataArray->isSame(i, j, data)) {
                            targetDataId = i;
                            targetSegmentId = j;
                            break;
                        }
                    }
                }
                if (targetDataId != -1) {
                    if (hashId == -1) {
                        DS_HI++;
                        hashId = hashArray->preinsert(hash, &req);
                        if(hashId != -1)
                            hashArray->postinsert(hash, &req, targetDataId, targetSegmentId, hashId, true);
                    } else if (hashArray->readDataPointer(hashId) == targetDataId && hashArray->readSegmentPointer(hashId) == targetSegmentId) {
                        DS_HS++;
                    } else {
                        DS_HD++;
                        if(dataArray->readCounter(hashArray->readDataPointer(hashId), hashArray->readSegmentPointer(hashId)) == 1)
                            hashArray->postinsert(hash, &req, targetDataId, targetSegmentId, hashId, true);
                    }
                    WD_TH_DS++;
                    debug("%s: Found matching hash at %i pointing to similar data line %i", name.c_str(), hashId, targetDataId);
                    int32_t newLLHead;
                    bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead);
                    if (evictDataLine) {
                        debug("%s: old data line %i, segment %i evicted", name.c_str(), dataId, segmentId);
                        // Clear (Evict, Tags already evicted) data line
                        dataArray->postinsert(-1, &req, 0, dataId, segmentId, NULL, false);
                        tagArray->postinsert(0, &req, tagId, -1, -1, NONE, -1, false, false);
                    } else if (newLLHead != -1) {
                        debug("%s: dedup of old data line %i, segment %i decreased", name.c_str(), dataId, segmentId);
                        // Change Tag
                        uint32_t victimCounter = dataArray->readCounter(dataId, segmentId);
                        dataArray->changeInPlace(newLLHead, &req, victimCounter-1, dataId, segmentId, NULL, false);
                    } else {
                        uint32_t victimCounter = dataArray->readCounter(dataId, segmentId);
                        int32_t LLHead = dataArray->readListHead(dataId, segmentId);
                        debug("%s: dedup of old data line %i, segment %i decreased, and LL changed to %i", name.c_str(), dataId, segmentId, LLHead);
                        dataArray->changeInPlace(LLHead, &req, victimCounter-1, dataId, segmentId, NULL, false);
                    }
                    int32_t oldListHead = dataArray->readListHead(targetDataId, targetSegmentId);
                    uint32_t dataCounter = dataArray->readCounter(targetDataId, targetSegmentId);
                    tagArray->changeInPlace(req.lineAddr, &req, tagId, targetDataId, targetSegmentId, encoding, oldListHead, true);
                    dataArray->changeInPlace(tagId, &req, dataCounter+1, targetDataId, targetSegmentId, NULL, true);
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
                    // line has changed from before. requires extra accLat to read
                    // data line. then two more accLats to find that a
                    // line is similar and to actually update its dedup.
                    idbHitWritebackEvent* hwe = new (evRec) idbHitWritebackEvent(this, he, 3*accLat, domain);
                    hwe->setMinStartCycle(respCycle);
                    timing("%s: hitWritebackEvent Min Start: %lu, duration: %u", name.c_str(), respCycle, 3*accLat);
                    he->addChild(hwe, evRec);
                    tr.startEvent = tr.endEvent = he;
                } else {
                    debug("%s: Found no matching line.", name.c_str());
                    if (dataArray->readCounter(dataId, segmentId) == 1) {
                        WD_TH_DD_1++;
                        debug("%s: line was not deduplicated", name.c_str());
                        int32_t newLLHead;
                        bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead);
                        if (evictDataLine) {
                            debug("%s: old data line %i, segment %i evicted", name.c_str(), dataId, segmentId);
                            // Clear (Evict, Tags already evicted) data line
                            dataArray->postinsert(-1, &req, 0, dataId, segmentId, NULL, false);
                            tagArray->postinsert(0, &req, tagId, -1, -1, NONE, -1, false, false);
                        } else if (newLLHead != -1) {
                            debug("%s: dedup of old data line %i, segment %i decreased", name.c_str(), dataId, segmentId);
                            // Change Tag
                            uint32_t victimCounter = dataArray->readCounter(dataId, segmentId);
                            dataArray->changeInPlace(newLLHead, &req, victimCounter-1, dataId, segmentId, NULL, false);
                        } else {
                            uint32_t victimCounter = dataArray->readCounter(dataId, segmentId);
                            int32_t LLHead = dataArray->readListHead(dataId, segmentId);
                            debug("%s: dedup of old data line %i, segment %i decreased, and LL changed to %i", name.c_str(), dataId, segmentId, LLHead);
                            dataArray->changeInPlace(LLHead, &req, victimCounter-1, dataId, segmentId, NULL, false);
                        }
                        // Timing: need to evict a victim dataLine, that
                        // means we need to read it's data, then tag
                        // first.
                        evictCycle = respCycle + 2*accLat;
                        timing("%s: Read victim line for eviction on cycle %lu", name.c_str(), evictCycle);
                        int32_t victimDataId = dataArray->preinsert(lineSize);
                        debug("%s: Picked victim data line %i", name.c_str(), victimDataId);
                        uint16_t freeSpace = 0;
                        g_vector<uint32_t> keptFromEvictions;
                        uint64_t lastEvDoneCycle = tagEvDoneCycle;
                        uint64_t evBeginCycle = evictCycle;
                        do {
                            uint16_t occupiedSpace = 0;
                            for (uint32_t i = 0; i < dataArray->getAssoc()*zinfo->lineSize/8; i++)
                                if (dataArray->readListHead(victimDataId, i) != -1)
                                    occupiedSpace += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, i)), zinfo->lineSize);
                            freeSpace = dataArray->getAssoc()*zinfo->lineSize - occupiedSpace;
                            debug("%s: line now has %i segments free.", name.c_str(), freeSpace/8);
                            int32_t victimListHeadId, newVictimListHeadId;
                            int32_t victimSegmentId = dataArray->preinsert(victimDataId, &victimListHeadId, keptFromEvictions);
                            if (dataArray->readListHead(victimDataId, victimSegmentId) != -1) {
                                freeSpace += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, victimSegmentId)), zinfo->lineSize);
                            }
                            debug("%s: Picked victim segment %i", name.c_str(), victimSegmentId);
                            keptFromEvictions.push_back(victimSegmentId);
                            uint64_t evDoneCycle = evBeginCycle;
                            TimingRecord writebackRecord;
                            if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                            while (victimListHeadId != -1) {
                                Address wbLineAddr;
                                if (victimListHeadId != tagId) {
                                    wbLineAddr = tagArray->readAddress(victimListHeadId);
                                    timing("%s: doing size/dedup eviction for address %lu on cycle %lu", name.c_str(), wbLineAddr, evBeginCycle);
                                    evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                                    timing("%s: size/dedup eviction finished on cycle %lu", name.c_str(), evDoneCycle);
                                    newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                                    tagArray->postinsert(0, &req, victimListHeadId, -1, -1, NONE, -1, false);
                                } else {
                                    newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                                    wbLineAddr = 0;
                                }
                                if (evRec->hasRecord()) {
                                    debug("%s: size/dedup eviction of %i segments from tagId %i for address %lu", name.c_str(), BDICompressionToSize(tagArray->readCompressionEncoding(victimListHeadId), zinfo->lineSize)/8, victimListHeadId, wbLineAddr);
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
                            dataArray->postinsert(-1, &req, 0, victimDataId, victimSegmentId, NULL, false);
                        } while (freeSpace < lineSize);
                        tagArray->postinsert(req.lineAddr, &req, tagId, victimDataId, keptFromEvictions[0], encoding, -1, updateReplacement, false);
                        dataArray->postinsert(tagId, &req, 1, victimDataId, keptFromEvictions[0], data, true);
                        if (hashId == -1) {
                            DD_HI++;
                            hashId = hashArray->preinsert(hash, &req);
                            if(hashId != -1)
                                hashArray->postinsert(hash, &req, victimDataId, keptFromEvictions[0], hashId, true);
                        } else {
                            DD_HD++;
                            if(dataArray->readCounter(hashArray->readDataPointer(hashId), hashArray->readSegmentPointer(hashId)) == 1)
                                hashArray->postinsert(hash, &req, victimDataId, keptFromEvictions[0], hashId, true);
                        }
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
                        // line has changed from before. requires extra accLat to read
                        // data line. then two more accLats to find that a
                        // line is colliding and to actually overwrite another.
                        idbHitWritebackEvent* hwe = new (evRec) idbHitWritebackEvent(this, he, 3*accLat, domain);
                        hwe->setMinStartCycle(lastEvDoneCycle);
                        timing("%s: hitWritebackEvent Min Start: %lu, duration: %u", name.c_str(), lastEvDoneCycle, 3*accLat);

                        if(wbStartCycles.size()) {
                            for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                                DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - (req.cycle + accLat));
                                del->setMinStartCycle(req.cycle + accLat);
                                he->addChild(del, evRec);
                                connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, del, hwe, wbStartCycles[i], wbEndCycles[i]);
                            }
                        }
                        he->addChild(hwe, evRec);
                        tr.startEvent = tr.endEvent = he;
                    } else {
                        WD_TH_DD_M++;
                        debug("%s: line was deduplicated", name.c_str());
                        // Data exists more than once, evict from LL.
                        int32_t newLLHead;
                        bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead);
                        if (evictDataLine) {
                            panic("Shouldn't happen %i, %i, %i", tagId, dataId, segmentId);
                        } else if (newLLHead != -1) {
                            debug("%s: dedup of old data line %i, segment %i decreased", name.c_str(), dataId, segmentId);
                            // Change Tag
                            uint32_t victimCounter = dataArray->readCounter(dataId, segmentId);
                            dataArray->changeInPlace(newLLHead, &req, victimCounter-1, dataId, segmentId, NULL, false);
                        } else {
                            uint32_t victimCounter = dataArray->readCounter(dataId, segmentId);
                            int32_t LLHead = dataArray->readListHead(dataId, segmentId);
                            debug("%s: dedup of old data line %i, segment %i decreased, and LL changed to %i", name.c_str(), dataId, segmentId, LLHead);
                            dataArray->changeInPlace(LLHead, &req, victimCounter-1, dataId, segmentId, NULL, false);
                        }
                        // Timing: need to evict a victim dataLine, that
                        // means we need to read it's data, then tag
                        // first.
                        evictCycle = respCycle + 2*accLat;
                        timing("%s: Read victim line for eviction on cycle %lu", name.c_str(), evictCycle);
                        int32_t victimDataId = dataArray->preinsert(lineSize);
                        debug("%s: Picked victim data line %i", name.c_str(), victimDataId);
                        uint16_t freeSpace = 0;
                        g_vector<uint32_t> keptFromEvictions;
                        evictCycle += accLat;
                        uint64_t lastEvDoneCycle = tagEvDoneCycle;
                        uint64_t evBeginCycle = evictCycle;
                        do {
                            uint16_t occupiedSpace = 0;
                            for (uint32_t i = 0; i < dataArray->getAssoc()*zinfo->lineSize/8; i++)
                                if (dataArray->readListHead(victimDataId, i) != -1)
                                    occupiedSpace += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, i)), zinfo->lineSize);
                            freeSpace = dataArray->getAssoc()*zinfo->lineSize - occupiedSpace;
                            debug("%s: line now has %i segments free.", name.c_str(), freeSpace/8);
                            int32_t victimListHeadId, newVictimListHeadId;
                            int32_t victimSegmentId = dataArray->preinsert(victimDataId, &victimListHeadId, keptFromEvictions);
                            if (dataArray->readListHead(victimDataId, victimSegmentId) != -1) {
                                freeSpace += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, victimSegmentId)), zinfo->lineSize);
                            }
                            debug("%s: Picked victim segment %i", name.c_str(), victimSegmentId);
                            keptFromEvictions.push_back(victimSegmentId);
                            uint64_t evDoneCycle = evBeginCycle;
                            TimingRecord writebackRecord;
                            if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                            while (victimListHeadId != -1) {
                                Address wbLineAddr;
                                if (victimListHeadId != tagId) {
                                    wbLineAddr = tagArray->readAddress(victimListHeadId);
                                    timing("%s: doing size/dedup eviction for address %lu on cycle %lu", name.c_str(), wbLineAddr, evBeginCycle);
                                    evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                                    timing("%s: size/dedup eviction finished on cycle %lu", name.c_str(), evDoneCycle);
                                    newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                                    tagArray->postinsert(0, &req, victimListHeadId, -1, -1, NONE, -1, false);
                                } else {
                                    newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                                    wbLineAddr = 0;
                                }
                                if (evRec->hasRecord()) {
                                    debug("%s: size/dedup eviction of %i segments from tagId %i for address %lu", name.c_str(), BDICompressionToSize(tagArray->readCompressionEncoding(victimListHeadId), zinfo->lineSize)/8, victimListHeadId, wbLineAddr);
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
                            dataArray->postinsert(-1, &req, 0, victimDataId, victimSegmentId, NULL, false);
                        } while (freeSpace < lineSize);
                        tagArray->postinsert(req.lineAddr, &req, tagId, victimDataId, keptFromEvictions[0], encoding, -1, updateReplacement, false);
                        dataArray->postinsert(tagId, &req, 1, victimDataId, keptFromEvictions[0], data, true);
                        if (hashId == -1) {
                            DD_HI++;
                            hashId = hashArray->preinsert(hash, &req);
                            if(hashId != -1)
                                hashArray->postinsert(hash, &req, victimDataId, keptFromEvictions[0], hashId, true);
                        } else {
                            DD_HD++;
                            if(dataArray->readCounter(hashArray->readDataPointer(hashId), hashArray->readSegmentPointer(hashId)) == 1)
                                hashArray->postinsert(hash, &req, victimDataId, keptFromEvictions[0], hashId, true);
                        }
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
                        // line has changed from before. requires extra accLat to read
                        // data line. then two more accLats to find that a
                        // line is colliding and to actually overwrite another.
                        idbHitWritebackEvent* hwe = new (evRec) idbHitWritebackEvent(this, he, 3*accLat, domain);
                        hwe->setMinStartCycle(lastEvDoneCycle);
                        timing("%s: hitWritebackEvent Min Start: %lu, duration: %u", name.c_str(), lastEvDoneCycle, 3*accLat);

                        if(wbStartCycles.size()) {
                            for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                                DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - (req.cycle + accLat));
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
                WSR_TH++;
                debug("%s: read hit, or write same data.", name.c_str());
                respCycle += accLat;
                timing("%s: reading data on cycle %lu", name.c_str(), respCycle);
                dataArray->lookup(tagArray->readDataId(tagId), tagArray->readSegmentPointer(tagId), &req, updateReplacement);
                uint64_t getDoneCycle = respCycle;
                timing("%s: doing processAccess on cycle %lu", name.c_str(), respCycle);
                respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                timing("%s: finished processAccess on cycle %lu", name.c_str(), respCycle);
                if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                ev->setMinStartCycle(req.cycle);
                tr.startEvent = tr.endEvent = ev;
            }
        }
        gm_free(data);
        evRec->pushRecord(tr);

        // tagArray->print();
        // dataArray->print();
        // hashArray->print();
    }
    cc->endAccess(req);

    // uint32_t dataValidSegments = 0;
    // for (uint32_t i = 0; i < numDataLines/dataAssoc; i++)
    // {
    //     uint32_t singleSetCount = 0;
    //     for (uint32_t j = 0; j < dataAssoc*8; j++)
    //     {
    //         if (dataArray->readListHead(i, j) != -1) {
    //             dataValidSegments += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(i, j)), zinfo->lineSize)/8;
    //             singleSetCount += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(i, j)), zinfo->lineSize)/8;
    //         }
    //         assert(singleSetCount <= dataAssoc*8);
    //     }
    // }

    // uint32_t count = 0;
    // for (int32_t i = 0; i < (signed)(numDataLines/dataAssoc); i++) {
    //     for (int32_t j = 0; j < (signed)dataAssoc*8; j++) {
    //         if (dataArray->readListHead(i, j) == -1)
    //             continue;
    //         count += dataArray->readCounter(i, j);
    //         int32_t tagId = dataArray->readListHead(i, j);
    //         assert(tagArray->readDataId(tagId) == i && tagArray->readSegmentPointer(tagId) == j);
    //     }
    // }
    // assert(count == tagArray->getValidLines());

    // info("Valid Tags: %u", tagArray->getValidLines());
    // info("Valid Segments: %u", tagArray->getDataValidSegments());
    // assert(tagArray->getValidLines() == tagArray->countValidLines());
    // assert(tagArray->getDataValidSegments() == dataValidSegments);
    assert(tagArray->getValidLines() >= tagArray->getDataValidSegments()/8);
    assert(tagArray->getValidLines() <= numTagLines);
    assert(tagArray->getDataValidSegments() <= numDataLines*8);

    double sample = ((double)tagArray->getDataValidSegments()/8)/(double)tagArray->getValidLines();
    crStats->add(sample,1);

    if (req.type != PUTS) {
        sample = Evictions;
        evStats->add(sample,1);
    }

    sample = ((double)tagArray->getDataValidSegments()/8)/numDataLines;
    dutStats->add(sample, 1);

    sample = (double)tagArray->getValidLines()/numTagLines;
    tutStats->add(sample, 1);

    uint32_t compressedLineCount = 0;
    for (uint32_t i = 0; i < numDataLines/dataAssoc; i++) {
        for (uint32_t j = 0; j < dataAssoc*8; j++) {
            if(dataArray->readListHead(i, j) != -1) {
                compressedLineCount++;
            }
        }
    }

    sample = (double)tagArray->getValidLines()/compressedLineCount;
    dupStats->add(sample, 1);

    sample = (double)tagArray->getDataValidSegments()/compressedLineCount;
    bdiStats->add(sample, 1);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}

void ApproximateIdealDedupBDICache::simulateHitWriteback(idbHitWritebackEvent* ev, uint64_t cycle, HitEvent* he) {
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

void ApproximateIdealDedupBDICache::dumpStats() {
    info("TM_DS: %lu", TM_DS);
    info("TM_DD: %lu", TM_DD);
    info("WD_TH_DS: %lu", WD_TH_DS);
    info("WD_TH_DD_1: %lu", WD_TH_DD_1);
    info("WD_TH_DD_M: %lu", WD_TH_DD_M);
    info("WSR_TH: %lu", WSR_TH);
    info("DS_HI: %lu", DS_HI);
    info("DS_HS: %lu", DS_HS);
    info("DS_HD: %lu", DS_HD);
    info("DD_HI: %lu", DD_HI);
    info("DD_HD: %lu", DD_HD);
    dupStats->dump();
    bdiStats->dump();
}
