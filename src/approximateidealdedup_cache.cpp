#include "approximateidealdedup_cache.h"
#include "pin.H"

ApproximateIdealDedupCache::ApproximateIdealDedupCache(uint32_t _numTagLines, uint32_t _numDataLines, CC* _cc, ApproximateDedupTagArray* _tagArray, ApproximateDedupDataArray* _dataArray, ApproximateDedupHashArray* _hashArray, ReplPolicy* tagRP,
ReplPolicy* dataRP, ReplPolicy* hashRP, uint32_t _accLat, uint32_t _invLat, uint32_t mshrs, uint32_t ways, uint32_t cands, uint32_t _domain, const g_string& _name, RunningStats* _crStats,
RunningStats* _evStats, RunningStats* _tutStats, RunningStats* _dutStats, Counter* _tag_hits, Counter* _tag_misses, Counter* _tag_all) : TimingCache(_numTagLines, _cc, NULL, tagRP, _accLat, _invLat, mshrs, tagLat, ways, cands, _domain, _name, _evStats, _tag_hits, _tag_misses, _tag_all), numTagLines(_numTagLines),
numDataLines(_numDataLines), tagArray(_tagArray), dataArray(_dataArray), hashArray(_hashArray), tagRP(tagRP), dataRP(dataRP), hashRP(hashRP), crStats(_crStats), evStats(_evStats), tutStats(_tutStats), dutStats(_dutStats) {
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
}

void ApproximateIdealDedupCache::initStats(AggregateStat* parentStat) {
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

void ApproximateIdealDedupCache::initCacheStats(AggregateStat* cacheStat) {
    cc->initStats(cacheStat);
    tagArray->initStats(cacheStat);
    tagRP->initStats(cacheStat);
    dataArray->initStats(cacheStat);
    dataRP->initStats(cacheStat);
    hashRP->initStats(cacheStat);
}

uint64_t ApproximateIdealDedupCache::access(MemReq& req) {
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

    EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
    assert_msg(evRec, "ApproximateBDI is not connected to TimingCore");

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

    g_vector<uint32_t> keptFromEvictions;

    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        // info("%lu: REQ %s to address %lu in %s region", req.cycle, AccessTypeName(req.type), req.lineAddr << lineBits, approximate? "approximate":"exact");
        // info("Req data type: %s, data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t tagId = tagArray->lookup(req.lineAddr, &req, updateReplacement);
        zinfo->tagAll++;
        respCycle += accLat;
        evictCycle += accLat;

        MissStartEvent* mse;
        MissResponseEvent* mre;
        MissWritebackEvent* mwe;
        if (tagId == -1) {
            if (tag_misses) tag_misses->inc();
            zinfo->tagMisses++;
            // info("\tTag Miss");
            assert(cc->shouldAllocate(req));
            // Get the eviction candidate
            Address wbLineAddr;
            int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
            // info("\t\tEvicting tagId: %i", victimTagId);
            keptFromEvictions.push_back(victimTagId);
            trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);
            // Need to evict the tag.
            tagEvDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evictCycle);
            // // // info("\t\t\tEviction finished at %lu", tagEvDoneCycle);
            int32_t newLLHead = -1;
            bool approximateVictim;
            bool evictDataLine = tagArray->evictAssociatedData(victimTagId, &newLLHead, &approximateVictim);
            int32_t victimDataId = tagArray->readDataId(victimTagId);
            if (evictDataLine) {
                // info("\t\tAlong with dataId: %i", victimDataId);
                // Clear (Evict, Tags already evicted) data line
                dataArray->postinsert(-1, &req, 0, victimDataId, false, NULL, false);
            } else if (newLLHead != -1) {
                // Change Tag
                // info("\t\tAnd decremented tag counter and decremented LL Head for dataline %i", victimDataId);
                uint32_t victimCounter = dataArray->readCounter(victimDataId);
                dataArray->changeInPlace(newLLHead, &req, victimCounter-1, victimDataId, approximateVictim, NULL, false);
            } else if (victimDataId != -1) {
                // info("\t\tAnd decremented dedup counter for dataline %i.", victimDataId);
                uint32_t victimCounter = dataArray->readCounter(victimDataId);
                int32_t LLHead = dataArray->readListHead(victimDataId);
                dataArray->changeInPlace(LLHead, &req, victimCounter-1, victimDataId, approximateVictim, NULL, false);
            }
            tagArray->postinsert(0, &req, victimTagId, -1, -1, false, false);
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

            if(approximate)
                hashArray->approximate(data, type);
            int32_t dataId = -1;
            for (uint32_t i = 0; i < numDataLines; i++) {
                if (dataArray->readCounter(i) && dataArray->isSame(i, data)) {
                    dataId = i;
                    break;
                }
            }
            uint64_t hash = hashArray->hash(data);
            int32_t hashId = hashArray->lookup(hash, &req, false);
            if (dataId != -1) {
                if (hashId == -1) {
                    DS_HI++;
                    hashId = hashArray->preinsert(hash, &req);
                    if(hashId != -1)
                        hashArray->postinsert(hash, &req, dataId, hashId, true);
                } else if (hashArray->readDataPointer(hashId) == dataId) {
                    DS_HS++;
                } else {
                    DS_HD++;
                    if(dataArray->readCounter(hashArray->readDataPointer(hashId)) == 1)
                        hashArray->postinsert(hash, &req, dataId, hashId, true);
                }
                TM_DS++;
                // info("\t\tfound matching data at %i.", dataId);
                int32_t oldListHead = dataArray->readListHead(dataId);
                // // info("With a list head at %i", oldListHead);
                uint32_t dataCounter = dataArray->readCounter(dataId);
                tagArray->postinsert(req.lineAddr, &req, victimTagId, dataId, oldListHead, true, updateReplacement);
                // // info("postinsert %i with oldListHead %i", victimTagId, oldListHead);
                dataArray->postinsert(victimTagId, &req, dataCounter+1, dataId, true, NULL, updateReplacement);

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
                TM_DD++;
                // info("\t\tCouldn't find matching hash.");
                // Select data to evict
                evictCycle = respCycle + accLat;
                int32_t victimListHeadId, newVictimListHeadId;
                int32_t victimDataId = dataArray->preinsert(&victimListHeadId);
                if (hashId == -1) {
                    DD_HI++;
                    hashId = hashArray->preinsert(hash, &req);
                    if(hashId != -1)
                        hashArray->postinsert(hash, &req, victimDataId, hashId, true);
                } else {
                    DD_HD++;
                    if(dataArray->readCounter(hashArray->readDataPointer(hashId)) == 1)
                        hashArray->postinsert(hash, &req, victimDataId, hashId, true);
                }
                // info("\t\tEvicting dataline %i", victimDataId);
                uint64_t evBeginCycle = evictCycle;
                TimingRecord writebackRecord;
                uint64_t lastEvDoneCycle = tagEvDoneCycle;
                uint64_t evDoneCycle = evBeginCycle;
                if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                while (victimListHeadId != -1) {
                    if (victimListHeadId != victimTagId) {
                        Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                        // info("\t\tAlong with tagId: %i", victimListHeadId);
                        // // info("\t\tEvicting tagId: %i", victimListHeadId);
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
                // // info("postinsert %i", victimTagId);
                dataArray->postinsert(victimTagId, &req, 1, victimDataId, true, data, updateReplacement);
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
            zinfo->tagHits++;
            if(approximate)
                hashArray->approximate(data, type);
            int32_t dataId = tagArray->readDataId(tagId);
            if (req.type == PUTX && !dataArray->isSame(dataId, data)) {
                // int32_t dataId = hashArray->readDataPointer(hashId);
                // info("\tWrite Tag Hit, Data different");
                uint64_t hash = hashArray->hash(data);
                int32_t hashId = hashArray->lookup(hash, &req, false);
                int32_t targetDataId = -1;
                for (uint32_t i = 0; i < numDataLines; i++) {
                    if (dataArray->readCounter(i) && dataArray->isSame(i, data)) {
                        targetDataId = i;
                        break;
                    }
                }
                if (targetDataId != -1) {
                    if (hashId == -1) {
                        DS_HI++;
                        hashId = hashArray->preinsert(hash, &req);
                        if(hashId != -1)
                            hashArray->postinsert(hash, &req, targetDataId, hashId, true);
                    } else if (hashArray->readDataPointer(hashId) == targetDataId) {
                        DS_HS++;
                    } else {
                        DS_HD++;
                        if(dataArray->readCounter(hashArray->readDataPointer(hashId)) == 1)
                            hashArray->postinsert(hash, &req, targetDataId, hashId, true);
                    }
                    WD_TH_DS++;
                    // info("\t\tFound matching data at %i.", targetDataId);
                    // // info("Data is also similar to %i.", targetDataId);
                    bool approximateVictim;
                    int32_t newLLHead;
                    bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead, &approximateVictim);
                    if (evictDataLine) {
                        // info("\t\tDeleting old dataId at %i", dataId);
                        // // info("\t\tAlong with dataId: %i", dataId);
                        // Clear (Evict, Tags already evicted) data line
                        dataArray->postinsert(-1, &req, 0, dataId, false, NULL, false);
                    } else if (newLLHead != -1) {
                        // info("\t\tchanging LL pointer for old dataId at %i and decremented it's counter", dataId);
                        // Change Tag
                        uint32_t victimCounter = dataArray->readCounter(dataId);
                        dataArray->changeInPlace(newLLHead, &req, victimCounter-1, dataId, approximateVictim, NULL, false);
                    } else {
                        // info("\t\tdecremented the counter at dataId %i", dataId);
                        // // info("\t\tAlso decremented dedup counter.");
                        uint32_t victimCounter = dataArray->readCounter(dataId);
                        int32_t LLHead = dataArray->readListHead(dataId);
                        dataArray->changeInPlace(LLHead, &req, victimCounter-1, dataId, approximateVictim, NULL, false);
                    }
                    respCycle += 2*accLat;
                    int32_t oldListHead = dataArray->readListHead(targetDataId);
                    // // info("With a list head at %i", oldListHead);
                    uint32_t dataCounter = dataArray->readCounter(targetDataId);
                    tagArray->changeInPlace(req.lineAddr, &req, tagId, targetDataId, oldListHead, true, updateReplacement);
                    // // info("postinsert %i with oldListHead %i", tagId, oldListHead);
                    dataArray->postinsert(tagId, &req, dataCounter+1, targetDataId, true, NULL, updateReplacement);

                    uint64_t getDoneCycle = respCycle;
                    respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                    if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                    tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};

                    HitEvent* he = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                    // // // info("uCREATE: %p at %u", he, __LINE__);
                    idHitWritebackEvent* hwe = new (evRec) idHitWritebackEvent(this, he, respCycle - req.cycle, domain);
                    // // // info("uCREATE: %p at %u", hwe, __LINE__);

                    he->setMinStartCycle(req.cycle);
                    hwe->setMinStartCycle(respCycle);
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
                } else {
                    // info("\t\tCouldn't find a matching hash.");
                    if (dataArray->readCounter(dataId) == 1) {
                        if (hashId == -1) {
                            DD_HI++;
                            hashId = hashArray->preinsert(hash, &req);
                            if(hashId != -1)
                                hashArray->postinsert(hash, &req, dataId, hashId, true);
                        } else {
                            DD_HD++;
                            if(dataArray->readCounter(hashArray->readDataPointer(hashId)) == 1)
                                hashArray->postinsert(hash, &req, dataId, hashId, true);
                        }
                        WD_TH_DD_1++;
                        // info("\t\tOnly had one tag. Overwriting self instead of picking random data victim.");
                        // Data only exists once, just update.
                        // // info("PUTX only once.");
                        dataArray->writeData(dataId, data, &req, true);
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
                        WD_TH_DD_M++;
                        // Data exists more than once, evict from LL.
                        // // info("PUTX more than once");
                        bool approximateVictim;
                        int32_t newLLHead;
                        bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead, &approximateVictim);
                        if (evictDataLine) {
                            // info("\t\tDeleting old dataId at %i", dataId);
                            // // info("\t\tAlong with dataId: %i", dataId);
                            // Clear (Evict, Tags already evicted) data line
                            dataArray->postinsert(-1, &req, 0, dataId, false, NULL, false);
                        } else if (newLLHead != -1) {
                            // info("\t\tchanging LL pointer for old dataId at %i and decremented it's counter", dataId);
                            // Change Tag
                            uint32_t victimCounter = dataArray->readCounter(dataId);
                            dataArray->changeInPlace(newLLHead, &req, victimCounter-1, dataId, approximateVictim, NULL, false);
                        } else {
                            // info("\t\tdecremented the counter at dataId %i", dataId);
                            // // info("\t\tAlso decremented dedup counter.");
                            uint32_t victimCounter = dataArray->readCounter(dataId);
                            int32_t LLHead = dataArray->readListHead(dataId);
                            dataArray->changeInPlace(LLHead, &req, victimCounter-1, dataId, approximateVictim, NULL, false);
                        }
                        evictCycle = respCycle + accLat;
                        respCycle += 2*accLat;
                        int32_t victimListHeadId, newVictimListHeadId;
                        int32_t victimDataId = dataArray->preinsert(&victimListHeadId);
                        while (victimDataId == dataId)
                            victimDataId = dataArray->preinsert(&victimListHeadId);
                        if (hashId == -1) {
                            DD_HI++;
                            hashId = hashArray->preinsert(hash, &req);
                            if(hashId != -1)
                                hashArray->postinsert(hash, &req, victimDataId, hashId, true);
                        } else {
                            DD_HD++;
                            if(dataArray->readCounter(hashArray->readDataPointer(hashId)) == 1)
                                hashArray->postinsert(hash, &req, victimDataId, hashId, true);
                        }
                        // info("\t\tEvicting dataline %i", victimDataId);
                        uint64_t evBeginCycle = evictCycle;
                        uint64_t evDoneCycle = evBeginCycle;
                        TimingRecord writebackRecord;
                        uint64_t lastEvDoneCycle = tagEvDoneCycle;
                        if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                        while (victimListHeadId != -1) {
                            if (victimListHeadId != tagId) {
                                // info("\t\tAlong with tagId: %i", victimListHeadId);
                                Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                                // // info("\t\tEvicting tagId: %i", victimListHeadId);
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
                        tagArray->changeInPlace(req.lineAddr, &req, tagId, victimDataId, -1, true, false);
                        // // info("changeInPlace %i", tagId);
                        dataArray->postinsert(tagId, &req, 1, victimDataId, true, data, updateReplacement);
                        respCycle += accLat;

                        uint64_t getDoneCycle = respCycle;
                        respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                        if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                        tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};

                        HitEvent* he = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                        // // // info("uCREATE: %p at %u", he, __LINE__);
                        idHitWritebackEvent* hwe = new (evRec) idHitWritebackEvent(this, he, respCycle - req.cycle, domain);
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
                WSR_TH++;
                // info("\tTag Hit");
                dataArray->lookup(tagArray->readDataId(tagId), &req, updateReplacement);
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
        // hashArray->print();
    }
    cc->endAccess(req);

    // uint32_t count = 0;
    // for (int32_t i = 0; i < (signed)numDataLines; i++) {
    //     if (dataArray->readListHead(i) == -1)
    //         continue;
    //     count += dataArray->readCounter(i);
    //     int32_t tagId = dataArray->readListHead(i);
    //     assert(tagArray->readDataId(tagId) == i);
    // }
    // assert(count == tagArray->getValidLines());

    // info("Valid Tags: %u", tagArray->getValidLines());
    // info("Valid Lines: %u", dataArray->getValidLines());
    assert(tagArray->getValidLines() == tagArray->countValidLines());
    assert(dataArray->getValidLines() == dataArray->countValidLines());
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

    sample = (double)tagArray->getValidLines()/dataArray->getValidLines();
    dupStats->add(sample, 1);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}

void ApproximateIdealDedupCache::simulateHitWriteback(idHitWritebackEvent* ev, uint64_t cycle, HitEvent* he) {
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

void ApproximateIdealDedupCache::dumpStats() {
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
}
