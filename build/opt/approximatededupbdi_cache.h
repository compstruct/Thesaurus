#ifndef APPROXIMATEDEDUPBDI_CACHE_H_
#define APPROXIMATEDEDUPBDI_CACHE_H_

#include "timing_cache.h"
#include "stats.h"

class dbHitWritebackEvent;

class ApproximateDedupBDICache : public TimingCache {
    protected:
        // Cache stuff
        uint32_t numTagLines;
        uint32_t numDataLines;
        uint32_t dataAssoc;

        ApproximateDedupBDITagArray* tagArray;
        ApproximateDedupBDIDataArray* dataArray;
        ApproximateDedupBDIHashArray* hashArray;

        ReplPolicy* tagRP;
        ReplPolicy* dataRP;
        ReplPolicy* hashRP;

        RunningStats* crStats;
        RunningStats* evStats;
        RunningStats* tutStats;
        RunningStats* dutStats;

    public:
        ApproximateDedupBDICache(uint32_t _numTagLines, uint32_t _numDataLines, CC* _cc, ApproximateDedupBDITagArray* _tagArray, ApproximateDedupBDIDataArray* _dataArray, ApproximateDedupBDIHashArray* _hashArray, ReplPolicy* tagRP, 
                        ReplPolicy* dataRP, ReplPolicy* hashRP, uint32_t _accLat, uint32_t _invLat, uint32_t mshrs, uint32_t ways, uint32_t cands, uint32_t _domain, const g_string& _name, RunningStats* _crStats, 
                        RunningStats* _evStats, RunningStats* _tutStats, RunningStats* _dutStats, Counter* _tag_hits, Counter* _tag_misses, Counter* _all_misses);

        uint64_t access(MemReq& req);

        void initStats(AggregateStat* parentStat);
        void simulateHitWriteback(dbHitWritebackEvent* ev, uint64_t cycle, HitEvent* he);

    protected:
        void initCacheStats(AggregateStat* cacheStat);
};

class dbHitWritebackEvent : public TimingEvent {
    private:
        ApproximateDedupBDICache* cache;
        HitEvent* he;
    public:
        dbHitWritebackEvent(ApproximateDedupBDICache* _cache,  HitEvent* _he, uint32_t postDelay, int32_t domain) : TimingEvent(0, postDelay, domain), cache(_cache), he(_he) {}
        void simulate(uint64_t startCycle) {cache->simulateHitWriteback(this, startCycle, he);}
};

#endif // APPROXIMATEDEDUPBDI_CACHE_H_
