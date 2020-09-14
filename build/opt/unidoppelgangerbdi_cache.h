#ifndef UNIDOPPELGANGERBDI_CACHE_H_
#define UNIDOPPELGANGERBDI_CACHE_H_

#include "timing_cache.h"
#include "stats.h"

class udbHitWritebackEvent;

class uniDoppelgangerBDICache : public TimingCache {
    protected:
        // Cache stuff
        uint32_t numTagLines;
        uint32_t numDataLines;

        uniDoppelgangerBDITagArray* tagArray;
        uniDoppelgangerBDIDataArray* dataArray;

        ReplPolicy* tagRP;
        ReplPolicy* dataRP;

        RunningStats* crStats;
        RunningStats* evStats;
        RunningStats* tutStats;
        RunningStats* dutStats;

    public:
        uniDoppelgangerBDICache(uint32_t _numTagLines, uint32_t _numDataLines, CC* _cc, uniDoppelgangerBDITagArray* _tagArray, uniDoppelgangerBDIDataArray* _dataArray,
                        ReplPolicy* tagRP, ReplPolicy* dataRP, uint32_t _accLat, uint32_t _invLat, uint32_t mshrs, uint32_t ways, uint32_t cands, uint32_t _domain, 
                        const g_string& _name, RunningStats* _crStats, RunningStats* _evStats, RunningStats* _tutStats, RunningStats* _dutStats, Counter* _tag_hits, Counter* _tag_misses, Counter* _tag_all);

        uint64_t access(MemReq& req);

        void initStats(AggregateStat* parentStat);
        void simulateHitWriteback(udbHitWritebackEvent* ev, uint64_t cycle, HitEvent* he);

    protected:
        void initCacheStats(AggregateStat* cacheStat);
};

class udbHitWritebackEvent : public TimingEvent {
    private:
        uniDoppelgangerBDICache* cache;
        HitEvent* he;
    public:
        udbHitWritebackEvent(uniDoppelgangerBDICache* _cache,  HitEvent* _he, uint32_t postDelay, int32_t domain) : TimingEvent(0, postDelay, domain), cache(_cache), he(_he) {}
        void simulate(uint64_t startCycle) {cache->simulateHitWriteback(this, startCycle, he);}
};

#endif // UNIDOPPELGANGERBDI_CACHE_H_
