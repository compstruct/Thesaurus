#include "approximatebdi_cache.h"
#include "pin.H"

ApproximateBDICache::ApproximateBDICache(uint32_t _numTagLines, uint32_t _numDataLines, CC* _cc, ApproximateBDITagArray* _tagArray, ApproximateBDIDataArray* _dataArray,
ReplPolicy* tagRP, ReplPolicy* dataRP, uint32_t _accLat, uint32_t _invLat, uint32_t mshrs, uint32_t ways, uint32_t cands, uint32_t _domain, const g_string& _name,
RunningStats* _crStats, RunningStats* _evStats, RunningStats* _tutStats, RunningStats* _dutStats, Counter* _tag_hits, Counter* _tag_misses, Counter* _tag_all) : TimingCache(_numTagLines, _cc, NULL, tagRP,
_accLat, _invLat, mshrs, tagLat, ways, cands, _domain, _name, _evStats, _tag_hits, _tag_misses, _tag_all), numTagLines(_numTagLines), numDataLines(_numDataLines), tagArray(_tagArray), tagRP(tagRP), crStats(_crStats),
evStats(_evStats), tutStats(_tutStats), dutStats(_dutStats) {
    g_string statName = name + g_string(" Data Size Average");
    bdiStats = new RunningStats(statName);
    statName = name + g_string(" Maximum Util Average");
    mutStats = new RunningStats(statName);
    tagCausedEv = 0;
    TM_bdiCausedEv = 0;
    WD_TH_bdiCausedEv = 0;
}

void ApproximateBDICache::initStats(AggregateStat* parentStat) {
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

void ApproximateBDICache::initCacheStats(AggregateStat* cacheStat) {
    cc->initStats(cacheStat);
    tagArray->initStats(cacheStat);
    tagRP->initStats(cacheStat);
}

uint64_t ApproximateBDICache::access(MemReq& req) {
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

    g_vector<uint32_t> keptFromEvictions;

    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t tagId = tagArray->lookup(req.lineAddr, &req, updateReplacement);
        // Timing: Tag array access latency.
        respCycle += accLat;
        evictCycle += accLat;
        timing("%s: tag accessed on cycle %lu", name.c_str(), respCycle);

        MissStartEvent* mse;
        MissResponseEvent* mre;
        MissWritebackEvent* mwe;
        if (tagId == -1) {
            if(tag_misses) tag_misses->inc();
            assert(cc->shouldAllocate(req));
            // Get the eviction candidate
            Address wbLineAddr;
            int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
            debug("%s: tag miss, inserting into line %i", name.c_str(), tagId);
            keptFromEvictions.push_back(victimTagId);
            // Need to evict the tag.
            // Timing: to evict, need to read the data array too.
            evictCycle += accLat;
            timing("%s: tag access missed, evicting address %lu on cycle %lu", name.c_str(), wbLineAddr, evictCycle);
            tagEvDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evictCycle);
            timing("%s: finished eviction on cycle %lu", name.c_str(), tagEvDoneCycle);
            if (evRec->hasRecord()) {
                debug("%s: tag miss caused eviction of %i segments from address %lu", name.c_str(), BDICompressionToSize(tagArray->readCompressionEncoding(victimTagId), zinfo->lineSize)/8, wbLineAddr);
                tagCausedEv++;
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

            // Now compress (and approximate) the new line
            if (approximate)
                dataArray->approximate(data, type);
            uint16_t lineSize = 0;
            BDICompressionEncoding encoding = dataArray->compress(data, &lineSize);
            debug("%s: compressed data to %i segments", name.c_str(), lineSize/8);

            // If the size of evicted line is not enough for the the compressed line
            // evict more
            // Timing: to evict more, no extra delay is needed, we already
            // read that line before. we just add 1
            uint64_t evBeginCycle = respCycle + 1;
            int32_t victimTagId2 = tagArray->needEviction(req.lineAddr, &req, lineSize, keptFromEvictions, &wbLineAddr);
            TimingRecord writebackRecord;
            uint64_t lastEvDoneCycle = tagEvDoneCycle;
            while(victimTagId2 != -1) {
                keptFromEvictions.push_back(victimTagId2);
                timing("%s: doing size eviction for address %lu on cycle %lu", name.c_str(), wbLineAddr, evBeginCycle);
                uint64_t evDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId2, evBeginCycle);
                timing("%s: size eviction finished on cycle %lu", name.c_str(), evDoneCycle);
                if (evRec->hasRecord()) {
                    debug("%s: size eviction of %i segments from tagId %i for address %lu", name.c_str(), BDICompressionToSize(tagArray->readCompressionEncoding(victimTagId2), zinfo->lineSize)/8, victimTagId2, wbLineAddr);
                    TM_bdiCausedEv++;
                    Evictions++;
                    writebackRecord.clear();
                    writebackRecord = evRec->popRecord();
                    writebackRecords.push_back(writebackRecord);
                    wbStartCycles.push_back(evBeginCycle);
                    wbEndCycles.push_back(evDoneCycle);
                    lastEvDoneCycle = evDoneCycle;
                    evBeginCycle += 1;
                }
                tagArray->postinsert(0, &req, victimTagId2, -1, NONE, false, false);
                victimTagId2 = tagArray->needEviction(req.lineAddr, &req, lineSize, keptFromEvictions, &wbLineAddr);
            }
            tagArray->postinsert(req.lineAddr, &req, victimTagId, 0, encoding, approximate, true);
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
                DelayEvent* del = new (evRec) DelayEvent(accLat);
                del->setMinStartCycle(req.cycle + accLat);
                mse->addChild(del, evRec);
                connect(tagWritebackRecord.isValid()? &tagWritebackRecord : nullptr, del, mwe, req.cycle + 2*accLat, tagEvDoneCycle);
            }
            tr.startEvent = mse;
            tr.endEvent = mre;
        } else {
            if(tag_hits) tag_hits->inc();
            debug("%s: tag hit on line %i", name.c_str(), tagId);
            if (req.type == PUTX) {
                // Now compress (and approximate) the new line
                if (approximate)
                    dataArray->approximate(data, type);
                uint16_t lineSize = 0;
                BDICompressionEncoding encoding = dataArray->compress(data, &lineSize);
                debug("%s: compressed write data to %i segments", name.c_str(), lineSize/8);
                // If size is the same
                if (lineSize == BDICompressionToSize(tagArray->readCompressionEncoding(tagId), zinfo->lineSize)) {
                    debug("%s: data is the same size as before, overwrite.", name.c_str());
                    // Timing: Data Array access Latency
                    respCycle += accLat;
                    timing("%s: writing data on cycle %lu", name.c_str(), respCycle);
                    uint64_t getDoneCycle = respCycle;
                    timing("%s: doing processAccess on cycle %lu", name.c_str(), respCycle);
                    respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                    timing("%s: finished processAccess on cycle %lu", name.c_str(), respCycle);
                    if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                    tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                    HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                    ev->setMinStartCycle(req.cycle);
                    timing("%s: hitEvent Min Start: %lu, duration: %lu", name.c_str(), req.cycle, respCycle - req.cycle);
                    tr.startEvent = tr.endEvent = ev;
                } else if (lineSize < BDICompressionToSize(tagArray->readCompressionEncoding(tagId), zinfo->lineSize)) {
                    debug("%s: data is smaller than before, overwrite.", name.c_str());
                    // Timing: Data Array access Latency
                    respCycle += accLat;
                    tagArray->writeCompressionEncoding(tagId, encoding);
                    timing("%s: writing data on cycle %lu", name.c_str(), respCycle);
                    uint64_t getDoneCycle = respCycle;
                    timing("%s: doing processAccess on cycle %lu", name.c_str(), respCycle);
                    respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                    timing("%s: finished processAccess on cycle %lu", name.c_str(), respCycle);
                    if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                    tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                    HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                    ev->setMinStartCycle(req.cycle);
                    timing("%s: hitEvent Min Start: %lu, duration: %lu", name.c_str(), req.cycle, respCycle - req.cycle);
                    tr.startEvent = tr.endEvent = ev;
                } else {
                    // If the size of evicted line is not enough for the the compressed line
                    // evict more
                    debug("%s: data is bigger than before.", name.c_str());
                    Address wbLineAddr;
                    // Timing: evictions cannot start until a read data
                    // occurs, requiring one more accLat.
                    respCycle += accLat;
                    uint64_t evBeginCycle = respCycle;
                    keptFromEvictions.push_back(tagId);
                    int32_t victimTagId = tagArray->needEviction(req.lineAddr, &req, lineSize, keptFromEvictions, &wbLineAddr);
                    TimingRecord writebackRecord;
                    if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                    uint64_t lastEvDoneCycle = tagEvDoneCycle;
                    while(victimTagId != -1) {
                        keptFromEvictions.push_back(victimTagId);
                        timing("%s: doing size eviction for address %lu on cycle %lu", name.c_str(), wbLineAddr, evBeginCycle);
                        uint64_t evDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evBeginCycle);
                        timing("%s: size eviction finished on cycle %lu", name.c_str(), evDoneCycle);
                        if (evRec->hasRecord()) {
                            debug("%s: size eviction of %i segments from tagId %i for address %lu", name.c_str(), BDICompressionToSize(tagArray->readCompressionEncoding(victimTagId), zinfo->lineSize)/8, victimTagId, wbLineAddr);
                            WD_TH_bdiCausedEv++;
                            Evictions++;
                            writebackRecord.clear();
                            writebackRecord = evRec->popRecord();
                            writebackRecords.push_back(writebackRecord);
                            wbStartCycles.push_back(evBeginCycle);
                            wbEndCycles.push_back(evDoneCycle);
                            lastEvDoneCycle = evDoneCycle;
                            evBeginCycle += 1;
                        }
                        tagArray->postinsert(0, &req, victimTagId, -1, NONE, false, false);
                        victimTagId = tagArray->needEviction(req.lineAddr, &req, lineSize, keptFromEvictions, &wbLineAddr);
                    }
                    timing("%s: writing data on cycle %lu", name.c_str(), respCycle);
                    uint64_t getDoneCycle = respCycle;
                    timing("%s: doing processAccess on cycle %lu", name.c_str(), respCycle);
                    respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                    timing("%s: finished processAccess on cycle %lu", name.c_str(), respCycle);
                    tagArray->writeCompressionEncoding(tagId, encoding);
                    if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                    tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};

                    HitEvent* he = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                    he->setMinStartCycle(req.cycle);
                    timing("%s: hitEvent Min Start: %lu, duration: %lu", name.c_str(), req.cycle, respCycle - req.cycle);
                    if(wbStartCycles.size()) {
                        // Timing: Writing the value requires reading for
                        // evictions first, then actually writing the new data.
                        aHitWritebackEvent* hwe = new (evRec) aHitWritebackEvent(this, he, accLat, domain);
                        hwe->setMinStartCycle(lastEvDoneCycle);
                        timing("%s: hitWritebackEvent Min Start: %lu, duration: %u", name.c_str(), lastEvDoneCycle, accLat);
                        for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                            DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - (req.cycle + 2*accLat));
                            del->setMinStartCycle(req.cycle + 2*accLat);
                            he->addChild(del, evRec);
                            connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, del, hwe, wbStartCycles[i], wbEndCycles[i]);
                        }
                        he->addChild(hwe, evRec);
                    }
                    tr.startEvent = tr.endEvent = he;
                }
            } else {
                debug("%s: reading data.", name.c_str());
                // Timing: Data Array access Latency
                respCycle += accLat;
                timing("%s: reading data on cycle %lu", name.c_str(), respCycle);
                uint64_t getDoneCycle = respCycle;
                timing("%s: doing processAccess on cycle %lu", name.c_str(), respCycle);
                respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                timing("%s: finished processAccess on cycle %lu", name.c_str(), respCycle);
                if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                ev->setMinStartCycle(req.cycle);
                timing("%s: hitEvent Min Start: %lu, duration: %lu", name.c_str(), req.cycle, respCycle - req.cycle);
                tr.startEvent = tr.endEvent = ev;
            }
        }
        gm_free(data);
        evRec->pushRecord(tr);

        // tagArray->print();
    }
    cc->endAccess(req);

    // info("Valid Tags: %u", tagArray->getValidLines());
    // info("Valid Lines: %u", tagArray->getDataValidSegments()/8);
    // assert(tagArray->getValidLines() == tagArray->countValidLines());
    // assert(tagArray->getDataValidSegments() == tagArray->countDataValidSegments());
    assert(tagArray->getValidLines() <= numTagLines);
    assert(tagArray->getDataValidSegments() <= numDataLines*8);
    assert(tagArray->getValidLines() >= tagArray->getDataValidSegments()/8);
    double sample = ((double)tagArray->getDataValidSegments()/8)/(double)tagArray->getValidLines();
    crStats->add(sample,1);

    if (req.type != PUTS) {
        sample = Evictions;
        evStats->add(sample,1);
    }

    sample = ((double)tagArray->getDataValidSegments()/8)/numDataLines;
    double Num1 = sample;
    dutStats->add(sample, 1);

    sample = (double)tagArray->getValidLines()/numTagLines;
    double Num2 = sample;
    tutStats->add(sample, 1);

    sample = std::max(Num1, Num2);
    mutStats->add(sample, 1);

    sample = (double)tagArray->getDataValidSegments()/tagArray->getValidLines();
    bdiStats->add(sample, 1);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}

void ApproximateBDICache::simulateHitWriteback(aHitWritebackEvent* ev, uint64_t cycle, HitEvent* he) {
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

void ApproximateBDICache::dumpStats() {
    bdiStats->dump();
    mutStats->dump();
    info("tagCausedEv: %lu", tagCausedEv);
    info("TM_bdiCausedEv: %lu", TM_bdiCausedEv);
    info("WD_TH_bdiCausedEv: %lu", WD_TH_bdiCausedEv);
}
