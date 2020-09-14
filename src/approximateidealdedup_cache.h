#ifndef APPROXIMATEIDEALDEDUP_CACHE_H_
#define APPROXIMATEIDEALDEDUP_CACHE_H_

#include "timing_cache.h"
#include "stats.h"

class idHitWritebackEvent;

class ApproximateIdealDedupCache : public TimingCache {
    protected:
        // Cache stuff
        uint32_t numTagLines;
        uint32_t numDataLines;

        ApproximateDedupTagArray* tagArray;
        ApproximateDedupDataArray* dataArray;
        ApproximateDedupHashArray* hashArray;

        ReplPolicy* tagRP;
        ReplPolicy* dataRP;
        ReplPolicy* hashRP;

        RunningStats* crStats;
        RunningStats* evStats;
        RunningStats* tutStats;
        RunningStats* dutStats;
        RunningStats* dupStats;

        uint64_t TM_DS;
        uint64_t TM_DD;
        uint64_t WD_TH_DS;
        uint64_t WD_TH_DD_1;
        uint64_t WD_TH_DD_M;
        uint64_t WSR_TH;

        uint64_t DS_HI;
        uint64_t DS_HS;
        uint64_t DS_HD;
        uint64_t DD_HI;
        uint64_t DD_HD;

    public:
        ApproximateIdealDedupCache(uint32_t _numTagLines, uint32_t _numDataLines, CC* _cc, ApproximateDedupTagArray* _tagArray, ApproximateDedupDataArray* _dataArray, ApproximateDedupHashArray* _hashArray, ReplPolicy* tagRP,
                        ReplPolicy* dataRP, ReplPolicy* hashRP, uint32_t _accLat, uint32_t _invLat, uint32_t mshrs, uint32_t ways, uint32_t cands, uint32_t _domain, const g_string& _name, RunningStats* _crStats,
                        RunningStats* _evStats, RunningStats* _tutStats, RunningStats* _dutStats, Counter* _tag_hits, Counter* _tag_misses, Counter* _tag_all);
        uint64_t access(MemReq& req);
        void dumpStats();

        void initStats(AggregateStat* parentStat);
        void simulateHitWriteback(idHitWritebackEvent* ev, uint64_t cycle, HitEvent* he);

    protected:
        void initCacheStats(AggregateStat* cacheStat);
};

class idHitWritebackEvent : public TimingEvent {
    private:
        ApproximateIdealDedupCache* cache;
        HitEvent* he;
    public:
        idHitWritebackEvent(ApproximateIdealDedupCache* _cache,  HitEvent* _he, uint32_t postDelay, int32_t domain) : TimingEvent(0, postDelay, domain), cache(_cache), he(_he) {}
        void simulate(uint64_t startCycle) {cache->simulateHitWriteback(this, startCycle, he);}
};

#endif // APPROXIMATEIDEALDEDUP_CACHE_H_
