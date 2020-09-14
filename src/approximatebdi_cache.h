#ifndef APPROXIMATEBDI_CACHE_H_
#define APPROXIMATEBDI_CACHE_H_

#include "timing_cache.h"
#include "stats.h"

class aHitWritebackEvent;

class ApproximateBDICache : public TimingCache {
    protected:
        // Cache stuff
        uint32_t numTagLines;
        uint32_t numDataLines;

        ApproximateBDITagArray* tagArray;
        ApproximateBDIDataArray* dataArray;

        ReplPolicy* tagRP;
        ReplPolicy* dataRP;

        RunningStats* crStats;
        RunningStats* evStats;
        RunningStats* tutStats;
        RunningStats* dutStats;
        RunningStats* bdiStats;
        RunningStats* mutStats;

        uint64_t tagCausedEv;
        uint64_t TM_bdiCausedEv;
        uint64_t WD_TH_bdiCausedEv;

    public:
        ApproximateBDICache(uint32_t _numTagLines, uint32_t _numDataLines, CC* _cc, ApproximateBDITagArray* _tagArray, ApproximateBDIDataArray* _dataArray, ReplPolicy* tagRP, ReplPolicy* dataRP, uint32_t _accLat, uint32_t _invLat,
                        uint32_t mshrs, uint32_t ways, uint32_t cands, uint32_t _domain, const g_string& _name, RunningStats* _crStats, RunningStats* _evStats, RunningStats* _tutStats, RunningStats* _dutStats, Counter* _tag_hits, Counter* _tag_misses, Counter* _tag_all);

        uint64_t access(MemReq& req);
        void dumpStats();

        void initStats(AggregateStat* parentStat);
        void simulateHitWriteback(aHitWritebackEvent* ev, uint64_t cycle, HitEvent* he);

    protected:
        void initCacheStats(AggregateStat* cacheStat);
};

class aHitWritebackEvent : public TimingEvent {
    private:
        ApproximateBDICache* cache;
        HitEvent* he;
    public:
        aHitWritebackEvent(ApproximateBDICache* _cache,  HitEvent* _he, uint32_t postDelay, int32_t domain) : TimingEvent(0, postDelay, domain), cache(_cache), he(_he) {}
        void simulate(uint64_t startCycle) {cache->simulateHitWriteback(this, startCycle, he);}
};

#endif // APPROXIMATEBDI_CACHE_H_
