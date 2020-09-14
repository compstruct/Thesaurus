#ifndef APPROXIMATEDEDUP_CACHE_H_
#define APPROXIMATEDEDUP_CACHE_H_

#include "timing_cache.h"
#include "stats.h"

class dHitWritebackEvent;

class ApproximateDedupCache : public TimingCache {
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
        RunningStats* hutStats;
        RunningStats* dupStats;
        RunningStats* mutStats;

        uint64_t TM_HM;
        uint64_t TM_HH_DI;
        uint64_t TM_HH_DS;
        uint64_t TM_HH_DD;
        uint64_t WD_TH_HM_1;
        uint64_t WD_TH_HM_M;
        uint64_t WD_TH_HH_DI;
        uint64_t WD_TH_HH_DS;
        uint64_t WD_TH_HH_DD_1;
        uint64_t WD_TH_HH_DD_M;
        uint64_t WSR_TH;

        uint64_t tagCausedEv;
        uint64_t TM_HH_DD_dedupCausedEv;
        uint64_t TM_HM_dedupCausedEv;
        uint64_t WD_TH_HH_DD_M_dedupCausedEv;
        uint64_t WD_TH_HM_M_dedupCausedEv;

    public:
        ApproximateDedupCache(uint32_t _numTagLines, uint32_t _numDataLines, CC* _cc, ApproximateDedupTagArray* _tagArray, ApproximateDedupDataArray* _dataArray, ApproximateDedupHashArray* _hashArray, ReplPolicy* tagRP, 
                        ReplPolicy* dataRP, ReplPolicy* hashRP, uint32_t _accLat, uint32_t _invLat, uint32_t mshrs, uint32_t ways, uint32_t cands, uint32_t _domain, const g_string& _name, RunningStats* _crStats, 
                        RunningStats* _evStats, RunningStats* _tutStats, RunningStats* _dutStats, Counter* _tag_hits, Counter* _tag_misses, Counter* _tag_all);

        uint64_t access(MemReq& req);
        void dumpStats();

        void initStats(AggregateStat* parentStat);
        void simulateHitWriteback(dHitWritebackEvent* ev, uint64_t cycle, HitEvent* he);

    protected:
        void initCacheStats(AggregateStat* cacheStat);
};

class dHitWritebackEvent : public TimingEvent {
    private:
        ApproximateDedupCache* cache;
        HitEvent* he;
    public:
        dHitWritebackEvent(ApproximateDedupCache* _cache,  HitEvent* _he, uint32_t postDelay, int32_t domain) : TimingEvent(0, postDelay, domain), cache(_cache), he(_he) {}
        void simulate(uint64_t startCycle) {cache->simulateHitWriteback(this, startCycle, he);}
};

#endif // APPROXIMATEDEDUP_CACHE_H_
