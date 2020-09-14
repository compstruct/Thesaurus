/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef CACHE_ARRAYS_H_
#define CACHE_ARRAYS_H_

#include "memory_hierarchy.h"
#include "stats.h"
#include <random>
#include "g_std/g_unordered_map.h"

/* General interface of a cache array. The array is a fixed-size associative container that
 * translates addresses to line IDs. A line ID represents the position of the tag. The other
 * cache components store tag data in non-associative arrays indexed by line ID.
 */
class CacheArray : public GlobAlloc {
    public:
        /* Returns tag's ID if present, -1 otherwise. If updateReplacement is set, call the replacement policy's update() on the line accessed*/
        virtual int32_t lookup(const Address lineAddr, const MemReq* req, bool updateReplacement) = 0;

        /* Runs replacement scheme, returns tag ID of new pos and address of line to write back*/
        virtual uint32_t preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr) = 0;

        /* Actually do the replacement, writing the new address in lineId.
         * NOTE: This method is guaranteed to be called after preinsert, although
         * there may be some intervening calls to lookup. The implementation is
         * allowed to keep internal state in preinsert() and use it in postinsert()
         */
        virtual void postinsert(const Address lineAddr, const MemReq* req, uint32_t lineId) = 0;

        virtual void initStats(AggregateStat* parent) {}
};

class ReplPolicy;
class DataLRUReplPolicy;
class HashFamily;
class H3HashFamily;

/* Set-associative cache array */
class SetAssocArray : public CacheArray {
    protected:
        Address* array;
        ReplPolicy* rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t setMask;

    public:
        SetAssocArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf);
        SetAssocArray() {};

        int32_t lookup(const Address lineAddr, const MemReq* req, bool updateReplacement);
        uint32_t preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr);
        void postinsert(const Address lineAddr, const MemReq* req, uint32_t candidate);
};

// uniDoppelganger Start
class uniDoppelgangerTagArray {
    protected:
        bool* approximateArray;
        Address* tagArray;
        int32_t* prevPointerArray;
        int32_t* nextPointerArray;
        int32_t* mapPointerArray; // Or directly data array.
        ReplPolicy* rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t setMask;
        uint32_t validLines;

    public:
        uniDoppelgangerTagArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf);
        ~uniDoppelgangerTagArray();
        // Returns the Index of the matching tag, or -1 if none found.
        int32_t lookup(Address lineAddr, const MemReq* req, bool updateReplacement);
        // Returns candidate Index for insertion, wbLineAddr will point to its address for eviction.
        int32_t preinsert(Address lineAddr, const MemReq* req, Address* wbLineAddr);
        // returns a true if we should evict the associated map/data line. newLLHead is the Index of the new
        // LinkedList Head to pointed to from the data array.
        bool evictAssociatedData(int32_t lineId, int32_t* newLLHead, bool* approximate);
        // Actually inserts
        void postinsert(Address lineAddr, const MemReq* req, int32_t tagId, int32_t mapId, int32_t listHead, bool approximate, bool updateReplacement);
        void changeInPlace(Address lineAddr, const MemReq* req, int32_t tagId, int32_t mapId, int32_t listHead, bool approximate, bool updateReplacement);
        // returns mapId
        int32_t readMapId(int32_t tagId);
        // returns dataId
        int32_t readDataId(int32_t tagId);
        // returns address
        Address readAddress(int32_t tagId);
        // returns next tagID in LL
        int32_t readNextLL(int32_t tagId);
        uint32_t getValidLines();
        uint32_t countValidLines();
        void initStats(AggregateStat* parent) {}
        void print();
};

class uniDoppelgangerDataArray {
    protected:
        bool* approximateArray;
        int32_t* mtagArray;
        int32_t* tagPointerArray;
        ReplPolicy* rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t setMask;
        uint32_t validLines;

    public:
        uniDoppelgangerDataArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf);
        ~uniDoppelgangerDataArray();
        // Returns the Index of the matching map, Must find.
        int32_t lookup(uint32_t map, const MemReq* req, bool updateReplacement);
        // Return the map of this data line
        uint32_t calculateMap(const DataLine data, DataType type, DataValue minValue, DataValue maxValue);
        // Returns candidate ID for insertion, tagID will point to a tag list head that need to be evicted.
        int32_t preinsert(uint32_t map, const MemReq* req, int32_t* tagId);
        // Actually inserts
        void postinsert(int32_t map, const MemReq* req, int32_t mapId, int32_t tagId, bool approximate, bool updateReplacement);
        void changeInPlace(int32_t map, const MemReq* req, int32_t mapId, int32_t tagId, bool approximate, bool updateReplacement);
        // returns tagId
        int32_t readListHead(int32_t mapId);
        // returns map
        int32_t readMap(int32_t mapId);
        uint32_t getValidLines();
        uint32_t countValidLines();
        void initStats(AggregateStat* parent) {}
        void print();
};
// uniDoppelganger End

// BDI Begin
class ApproximateBDITagArray {
    protected:
        bool* approximateArray;
        Address* tagArray;
        int32_t* segmentPointerArray;    // NOTE: doesn't actually reflect segmentPointer. It's just valid or invalid.
        BDICompressionEncoding* compressionEncodingArray;
        ReplPolicy* rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t dataAssoc;
        uint32_t setMask;
        uint32_t validLines;
        uint32_t dataValidSegments;
    public:
        ApproximateBDITagArray(uint32_t _numLines, uint32_t _assoc, uint32_t _dataAssoc, ReplPolicy* _rp, HashFamily* _hf);
        ~ApproximateBDITagArray();
        // Returns the Index of the matching tag, or -1 if none found.
        int32_t lookup(Address lineAddr, const MemReq* req, bool updateReplacement);
        // Returns candidate Index for insertion, wbLineAddr will point to its address for eviction.
        int32_t preinsert(Address lineAddr, const MemReq* req, Address* wbLineAddr);
        // Returns candidate Index for insertion, wbLineAddr will point to its address for eviction, or -1 if none needed.
        int32_t needEviction(Address lineAddr, const MemReq* req, uint16_t size, g_vector<uint32_t>& alreadyEvicted, Address* wbLineAddr);
        // Actually inserts
        void postinsert(Address lineAddr, const MemReq* req, int32_t tagId, int8_t segmentId, BDICompressionEncoding compression, bool approximate, bool updateReplacement);
        // returns compressionEncoding
        BDICompressionEncoding readCompressionEncoding(int32_t tagId);
        void writeCompressionEncoding(int32_t tagId, BDICompressionEncoding encoding);
        // returns segmentPointer
        int8_t readSegmentPointer(int32_t tagId);
        uint32_t getValidLines();
        uint32_t countValidLines();
        uint32_t getDataValidSegments();
        uint32_t countDataValidSegments();
        void initStats(AggregateStat* parent) {}
        void print();
};

class ApproximateBDIDataArray {
    protected:
        // uint64_t my_llabs(int64_t x);
        // uint8_t multiBaseCompression(uint64_t* values, uint8_t size, uint8_t blimit, uint8_t bsize);
    public:
        // We can also generate bit masks here, but it will not affect the timing.
        BDICompressionEncoding compress(const DataLine data, uint16_t* size);
        void approximate(const DataLine data, DataType type);
};
// BDI Begin

// Dedup Begin
// This in fact is exactly the same as uniDoppelgangerTagArray
class ApproximateDedupTagArray {
    protected:
        bool* approximateArray;
        Address* tagArray;
        int32_t* prevPointerArray;
        int32_t* nextPointerArray;
        int32_t* dataPointerArray;
        ReplPolicy* rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t setMask;
        uint32_t validLines;

    public:
        ApproximateDedupTagArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf);
        ~ApproximateDedupTagArray();
        // Returns the Index of the matching tag, or -1 if none found.
        int32_t lookup(Address lineAddr, const MemReq* req, bool updateReplacement);
        // Returns candidate Index for insertion, wbLineAddr will point to its address for eviction.
        int32_t preinsert(Address lineAddr, const MemReq* req, Address* wbLineAddr);
        // returns a true if we should evict the associated map/data line. newLLHead is the Index of the new
        // LinkedList Head to pointed to from the data array.
        bool evictAssociatedData(int32_t lineId, int32_t* newLLHead, bool* approximate);
        // Actually inserts
        void postinsert(Address lineAddr, const MemReq* req, int32_t tagId, int32_t dataId, int32_t listHead, bool approximate, bool updateReplacement);
        void changeInPlace(Address lineAddr, const MemReq* req, int32_t tagId, int32_t dataId, int32_t listHead, bool approximate, bool updateReplacement);
        // returns dataId
        int32_t readDataId(int32_t tagId);
        // returns address
        Address readAddress(int32_t tagId);
        // returns next tagID in LL
        int32_t readNextLL(int32_t tagId);
        // returns next tagID in LL
        int32_t readPrevLL(int32_t tagId);
        uint32_t getValidLines();
        uint32_t countValidLines();
        void initStats(AggregateStat* parent) {}
        void print();
};

class ApproximateDedupDataArray {
    protected:
        bool* approximateArray;
        int32_t* tagCounterArray;
        int32_t* tagPointerArray;
        DataLine* dataArray;
        ReplPolicy* rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t setMask;
        uint32_t validLines;
        std::mt19937* RNG;
        std::uniform_int_distribution<>* DIST;
        g_vector<int32_t> freeList;
    public:
        ApproximateDedupDataArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf);
        ~ApproximateDedupDataArray();
        void lookup(int32_t dataId, const MemReq* req, bool updateReplacement);
        int32_t preinsert(int32_t* tagPointer);
        // Actually inserts
        void postinsert(int32_t tagId, const MemReq* req, int32_t counter, int32_t dataId, bool approximate, DataLine data, bool updateReplacement);
        void changeInPlace(int32_t tagId, const MemReq* req, int32_t counter, int32_t dataId, bool approximate, DataLine data, bool updateReplacement);
        void writeData(int32_t dataId, DataLine data, const MemReq* req, bool updateReplacement);
        bool isSame(int32_t dataId, DataLine data);
        // returns tagId
        int32_t readListHead(int32_t dataId);
        // returns counter
        int32_t readCounter(int32_t dataId);
        DataLine readData(int32_t dataId);
        uint32_t getValidLines();
        uint32_t countValidLines();
        void initStats(AggregateStat* parent) {}
        void print();
};

class ApproximateDedupHashArray {
    protected:
        uint64_t* hashArray;
        int32_t* dataPointerArray;
        ReplPolicy* rp;
        HashFamily* hf;
        H3HashFamily* dataHash;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t setMask;
        ApproximateDedupDataArray* dataArray;
    public:
        ApproximateDedupHashArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf, H3HashFamily* _dataHash);
        ~ApproximateDedupHashArray();
        void registerDataArray(ApproximateDedupDataArray* dataArray);
        int32_t lookup(uint64_t hash, const MemReq* req, bool updateReplacement);
        int32_t preinsert(uint64_t hash, const MemReq* req);
        void postinsert(uint64_t hash, const MemReq* req, int32_t dataPointer, int32_t hashId, bool updateReplacement);
        int32_t readDataPointer(int32_t hashId);
        void approximate(const DataLine data, DataType type);
        uint64_t hash(const DataLine data);
        uint32_t countValidLines();
        void print();
};
// Dedup End

// Dedup BDI Begin
class ApproximateDedupBDITagArray {
    protected:
        Address* tagArray;
        int32_t* segmentPointerArray;
        int32_t* prevPointerArray;
        int32_t* nextPointerArray;
        int32_t* dataPointerArray;
        BDICompressionEncoding* compressionEncodingArray;
        ReplPolicy* rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t setMask;
        uint32_t validLines;
        uint32_t dataValidSegments;
    public:
        ApproximateDedupBDITagArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf);
        ~ApproximateDedupBDITagArray();
        // Returns the Index of the matching tag, or -1 if none found.
        int32_t lookup(Address lineAddr, const MemReq* req, bool updateReplacement);
        // Returns candidate Index for insertion, wbLineAddr will point to its address for eviction.
        int32_t preinsert(Address lineAddr, const MemReq* req, Address* wbLineAddr);
        // returns a true if we should evict the associated map/data line. newLLHead is the Index of the new
        // LinkedList Head to pointed to from the data array.
        bool evictAssociatedData(int32_t lineId, int32_t* newLLHead);
        // Actually inserts
        void postinsert(Address lineAddr, const MemReq* req, int32_t tagId, int32_t dataId, int32_t segmentId, BDICompressionEncoding encoding, int32_t listHead, bool updateReplacement, bool replace = true);
        void changeInPlace(Address lineAddr, const MemReq* req, int32_t tagId, int32_t dataId, int32_t segmentId, BDICompressionEncoding encoding, int32_t listHead, bool updateReplacement);
        // returns compressionEncoding
        BDICompressionEncoding readCompressionEncoding(int32_t tagId);
        void writeCompressionEncoding(int32_t tagId, BDICompressionEncoding encoding);
        // returns segmentPointer
        int32_t readSegmentPointer(int32_t tagId);
        // returns mapId
        int32_t readMapId(int32_t tagId);
        // returns dataId
        int32_t readDataId(int32_t tagId);
        // returns address
        Address readAddress(int32_t tagId);
        // returns next tagID in LL
        int32_t readNextLL(int32_t tagId);
        int32_t readPrevLL(int32_t tagId);
        uint32_t getValidLines();
        uint32_t countValidLines();
        uint32_t getDataValidSegments();
        void initStats(AggregateStat* parent) {}
        void print();
};

class ApproximateDedupBDIDataArray : public ApproximateBDIDataArray {
    protected:
        int32_t** tagCounterArray;
        int32_t** tagPointerArray;
        DataLine** compressedDataArray;
        DataLRUReplPolicy** rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t setMask;
        uint32_t validLines;
        std::mt19937* RNG;
        std::uniform_int_distribution<>* DIST;
        g_vector<g_vector<int32_t>> freeList;
        ApproximateDedupBDITagArray* tagArray;
        bool popped;

    public:
        ApproximateDedupBDIDataArray(uint32_t _numLines, uint32_t _assoc, HashFamily* _hf);
        ~ApproximateDedupBDIDataArray();
        void assignTagArray(ApproximateDedupBDITagArray* _tagArray);
        void lookup(int32_t dataId, int32_t segmentId, const MemReq* req, bool updateReplacement);
        int32_t preinsert(uint16_t lineSize);
        int32_t preinsert(int32_t dataId, int32_t* tagId, g_vector<uint32_t>& exceptions);
        // Actually inserts
        void postinsert(int32_t tagId, const MemReq* req, int32_t counter, int32_t dataId, int32_t segmentId, DataLine data, bool updateReplacement);
        void changeInPlace(int32_t tagId, const MemReq* req, int32_t counter, int32_t dataId, int32_t segmentId, DataLine data, bool updateReplacement);
        void writeData(int32_t dataId, int32_t segmentId, DataLine data, const MemReq* req, bool updateReplacement);
        bool isSame(int32_t dataId, int32_t segmentId, DataLine data);
        // returns tagId
        int32_t readListHead(int32_t dataId, int32_t segmentId);
        // returns counter
        int32_t readCounter(int32_t dataId, int32_t segmentId);
        DataLine readData(int32_t dataId, int32_t segmentId);
        uint32_t getValidLines();
        void initStats(AggregateStat* parent) {}
        uint32_t getAssoc() {return assoc;}
        void print();
};

class ApproximateDedupBDIHashArray {
    protected:
        uint64_t* hashArray;
        int32_t* dataPointerArray;
        int32_t* segmentPointerArray;
        ReplPolicy* rp;
        HashFamily* hf;
        H3HashFamily* dataHash;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t setMask;
        ApproximateDedupBDIDataArray* dataArray;
    public:
        ApproximateDedupBDIHashArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf, H3HashFamily* _dataHash);
        ~ApproximateDedupBDIHashArray();
        void registerDataArray(ApproximateDedupBDIDataArray* dataArray);
        int32_t lookup(uint64_t hash, const MemReq* req, bool updateReplacement);
        int32_t preinsert(uint64_t hash, const MemReq* req);
        void postinsert(uint64_t hash, const MemReq* req, int32_t dataPointer, int32_t segmentPointer, int32_t hashId, bool updateReplacement);
        void changeInPlace(uint64_t hash, const MemReq* req, int32_t dataPointer, int32_t segmentPointer, int32_t hashId, bool updateReplacement);
        int32_t readDataPointer(int32_t hashId);
        int32_t readSegmentPointer(int32_t hashId);
        void approximate(const DataLine data, DataType type);
        uint64_t hash(const DataLine data);
        uint32_t countValidLines();
        void print();
};
// Dedup BDI End

class ApproximateNaiiveDedupBDIDataArray : public ApproximateDedupBDIDataArray {
    protected:
        g_vector<int32_t> freeList;

    public:
        ApproximateNaiiveDedupBDIDataArray(uint32_t _numLines, uint32_t _assoc, HashFamily* _hf);
        ~ApproximateNaiiveDedupBDIDataArray();
        int32_t preinsert(uint16_t lineSize);
        int32_t preinsert(int32_t dataId, int32_t* tagId, g_vector<uint32_t>& exceptions);
        // Actually inserts
        void postinsert(int32_t tagId, const MemReq* req, int32_t counter, int32_t dataId, int32_t segmentId, DataLine data, bool updateReplacement);
};

// Doppelganger BDI Begin
class uniDoppelgangerBDITagArray {
    protected:
        bool* approximateArray;
        Address* tagArray;
        int32_t* prevPointerArray;
        int32_t* nextPointerArray;
        int32_t* mapPointerArray; // Or directly data array.
        int32_t* segmentPointerArray;
        ReplPolicy* rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t setMask;
        uint32_t validLines;

    public:
        uniDoppelgangerBDITagArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf);
        ~uniDoppelgangerBDITagArray();
        // Returns the Index of the matching tag, or -1 if none found.
        int32_t lookup(Address lineAddr, const MemReq* req, bool updateReplacement);
        // Returns candidate Index for insertion, wbLineAddr will point to its address for eviction.
        int32_t preinsert(Address lineAddr, const MemReq* req, Address* wbLineAddr);
        // returns a true if we should evict the associated map/data line. newLLHead is the Index of the new
        // LinkedList Head to pointed to from the data array.
        bool evictAssociatedData(int32_t lineId, int32_t* newLLHead, bool* approximate);
        // Actually inserts
        void postinsert(Address lineAddr, const MemReq* req, int32_t tagId, int32_t mapId, int32_t segmentId, int32_t listHead, bool approximate, bool updateReplacement);
        void changeInPlace(Address lineAddr, const MemReq* req, int32_t tagId, int32_t mapId, int32_t segmentId, int32_t listHead, bool approximate, bool updateReplacement);
        // returns mapId
        int32_t readMapId(int32_t tagId);
        int32_t readSegmentId(int32_t tagId);
        // returns dataId
        int32_t readDataId(int32_t tagId);
        // returns address
        Address readAddress(int32_t tagId);
        // returns next tagID in LL
        int32_t readNextLL(int32_t tagId);
        uint32_t getValidLines();
        uint32_t countValidLines();
        void initStats(AggregateStat* parent) {}
        void print();
};

class uniDoppelgangerBDIDataArray : public ApproximateBDIDataArray {
    protected:
        bool** approximateArray;
        int32_t** mtagArray;
        int32_t** tagPointerArray;
        int32_t** tagCounterArray;
        BDICompressionEncoding** compressionEncodingArray;
        ReplPolicy* rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t setMask;
        uint32_t validSegments;
        uint32_t tagRatio;
    public:
        uniDoppelgangerBDIDataArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf, uint32_t _tagRatio);
        ~uniDoppelgangerBDIDataArray();
        // Returns the Index of the matching map, Must find.
        int32_t lookup(uint32_t map);
        int32_t lookup(uint32_t map, uint32_t mapId, const MemReq* req, bool updateReplacement);
        // Return the map of this data line
        uint32_t calculateMap(const DataLine data, DataType type, DataValue minValue, DataValue maxValue);
        // Returns candidate ID for insertion, tagID will point to a tag list head that need to be evicted.
        int32_t preinsert(uint32_t map);
        int32_t preinsert(uint32_t set, const MemReq* req, int32_t* tagId, g_vector<uint32_t>& exceptions);
        // Actually inserts
        void postinsert(int32_t map, const MemReq* req, int32_t mapId, int32_t segmentId, int32_t tagId, int32_t counter, BDICompressionEncoding compression, bool approximate, bool updateReplacement);
        void changeInPlace(int32_t map, const MemReq* req, int32_t mapId, int32_t segmentId, int32_t tagId, int32_t counter, BDICompressionEncoding compression, bool approximate, bool updateReplacement);
        // returns tagId
        int32_t readListHead(int32_t mapId, int32_t segmentId);
        int32_t readCounter(int32_t mapId, int32_t segmentId);
        bool readApproximate(int32_t mapId, int32_t segmentId);
        // returns map
        int32_t readMap(int32_t mapId, int32_t segmentId);
        BDICompressionEncoding readCompressionEncoding(int32_t mapId, int32_t segmentId);
        uint32_t getValidSegments();
        // uint32_t countValidLines();
        void initStats(AggregateStat* parent) {}
        uint32_t getAssoc() {return assoc;}
        uint32_t getRatio() {return tagRatio;}
        void print();
};
// Doppelganger BDI End

/* The cache array that started this simulator :) */
class ZArray : public CacheArray {
    private:
        Address* array; //maps line id to address
        uint32_t* lookupArray; //maps physical position to lineId
        ReplPolicy* rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t ways;
        uint32_t cands;
        uint32_t setMask;

        //preinsert() stores the swaps that must be done here, postinsert() does the swaps
        uint32_t* swapArray; //contains physical positions
        uint32_t swapArrayLen; //set in preinsert()

        uint32_t lastCandIdx;

        Counter statSwaps;

    public:
        ZArray(uint32_t _numLines, uint32_t _ways, uint32_t _candidates, ReplPolicy* _rp, HashFamily* _hf);

        int32_t lookup(const Address lineAddr, const MemReq* req, bool updateReplacement);
        uint32_t preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr);
        void postinsert(const Address lineAddr, const MemReq* req, uint32_t candidate);

        //zcache-specific, since timing code needs to know the number of swaps, and these depend on idx
        //Should be called after preinsert(). Allows intervening lookups
        uint32_t getLastCandIdx() const {return lastCandIdx;}

        void initStats(AggregateStat* parentStat);
};

// Simple wrapper classes and iterators for candidates in each case; simplifies replacement policy interface without sacrificing performance
// NOTE: All must implement the same interface and be POD (we pass them by value)
struct SetAssocCands {
    struct iterator {
        uint32_t x;
        explicit inline iterator(uint32_t _x) : x(_x) {}
        inline void inc() {x++;} //overloading prefix/postfix too messy
        inline uint32_t operator*() const { return x; }
        inline bool operator==(const iterator& it) const { return it.x == x; }
        inline bool operator!=(const iterator& it) const { return it.x != x; }
    };

    uint32_t b, e;
    inline SetAssocCands(uint32_t _b, uint32_t _e) : b(_b), e(_e) {}
    inline iterator begin() const {return iterator(b);}
    inline iterator end() const {return iterator(e);}
    inline uint32_t numCands() const { return e-b; }
};


struct ZWalkInfo {
    uint32_t pos;
    uint32_t lineId;
    int32_t parentIdx;

    inline void set(uint32_t p, uint32_t i, int32_t x) {pos = p; lineId = i; parentIdx = x;}
};

struct ZCands {
    struct iterator {
        ZWalkInfo* x;
        explicit inline iterator(ZWalkInfo* _x) : x(_x) {}
        inline void inc() {x++;} //overloading prefix/postfix too messy
        inline uint32_t operator*() const { return x->lineId; }
        inline bool operator==(const iterator& it) const { return it.x == x; }
        inline bool operator!=(const iterator& it) const { return it.x != x; }
    };

    ZWalkInfo* b;
    ZWalkInfo* e;
    inline ZCands(ZWalkInfo* _b, ZWalkInfo* _e) : b(_b), e(_e) {}
    inline iterator begin() const {return iterator(b);}
    inline iterator end() const {return iterator(e);}
    inline uint32_t numCands() const { return e-b; }
};

#endif  // CACHE_ARRAYS_H_
