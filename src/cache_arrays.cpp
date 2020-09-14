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

#include <limits>

#include "cache_arrays.h"
#include "hash.h"
#include "repl_policies.h"
#include "zsim.h"

#include "pin.H"
#include <cstdlib>
#include <time.h>
#include <algorithm>
/* Set-associative array implementation */

SetAssocArray::SetAssocArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf) : rp(_rp), hf(_hf), numLines(_numLines), assoc(_assoc)  {
    array = gm_calloc<Address>(numLines);
    numSets = numLines/assoc;
    setMask = numSets - 1;
    info("Set Assoc Array: %i lines and %i sets", numLines, numSets);
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

int32_t SetAssocArray::lookup(const Address lineAddr, const MemReq* req, bool updateReplacement) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;
    for (uint32_t id = first; id < first + assoc; id++) {
        if (array[id] ==  lineAddr) {
            if (updateReplacement) rp->update(id, req);
            return id;
        }
    }
    return -1;
}

uint32_t SetAssocArray::preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr) { //TODO: Give out valid bit of wb cand?
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;

    uint32_t candidate = rp->rankCands(req, SetAssocCands(first, first+assoc));

    *wbLineAddr = array[candidate];
    return candidate;
}

void SetAssocArray::postinsert(const Address lineAddr, const MemReq* req, uint32_t candidate) {
    rp->replaced(candidate);
    array[candidate] = lineAddr;
    rp->update(candidate, req);
}

// uniDoppelganger Start
uniDoppelgangerTagArray::uniDoppelgangerTagArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf) : rp(_rp), hf(_hf), numLines(_numLines), assoc(_assoc)  {
    tagArray = gm_calloc<Address>(numLines);
    prevPointerArray = gm_calloc<int32_t>(numLines);
    nextPointerArray = gm_calloc<int32_t>(numLines);
    mapPointerArray = gm_calloc<int32_t>(numLines);
    approximateArray = gm_calloc<bool>(numLines);
    for (uint32_t i = 0; i < numLines; i++) {
        prevPointerArray[i] = -1;
        nextPointerArray[i] = -1;
        mapPointerArray[i] = -1;
    }
    numSets = numLines/assoc;
    setMask = numSets - 1;
    validLines = 0;
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

uniDoppelgangerTagArray::~uniDoppelgangerTagArray() {
    gm_free(tagArray);
    gm_free(prevPointerArray);
    gm_free(nextPointerArray);
    gm_free(mapPointerArray);
    gm_free(approximateArray);
}

int32_t uniDoppelgangerTagArray::lookup(Address lineAddr, const MemReq* req, bool updateReplacement) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;
    for (uint32_t id = first; id < first + assoc; id++) {
        if (tagArray[id] ==  lineAddr) {
            if (updateReplacement) rp->update(id, req);
            return id;
        }
    }
    return -1;
}

int32_t uniDoppelgangerTagArray::preinsert(Address lineAddr, const MemReq* req, Address* wbLineAddr) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;

    uint32_t candidate = rp->rankCands(req, SetAssocCands(first, first+assoc));

    *wbLineAddr = tagArray[candidate];
    return candidate;
}

bool uniDoppelgangerTagArray::evictAssociatedData(int32_t lineId, int32_t* newLLHead, bool* approximate) {
    *newLLHead = -1;
    if (mapPointerArray[lineId] == -1)
        return false;
    if (!approximateArray[lineId])
        return true;
    *approximate = true;
    if (prevPointerArray[lineId] != -1)
        return false;
    else
        *newLLHead = nextPointerArray[lineId];
    if (nextPointerArray[lineId] != -1)
        return false;
    return true;
}

void uniDoppelgangerTagArray::postinsert(Address lineAddr, const MemReq* req, int32_t tagId, int32_t mapId, int32_t listHead, bool approximate, bool updateReplacement) {
    if (!tagArray[tagId] && lineAddr) {
        validLines++;
    } else if (tagArray[tagId] && !lineAddr) {
        assert(validLines);
        validLines--;
    }
    rp->replaced(tagId);
    tagArray[tagId] = lineAddr;
    mapPointerArray[tagId] = mapId;
    approximateArray[tagId] = approximate;
    if (prevPointerArray[tagId] != -1)
        nextPointerArray[prevPointerArray[tagId]] = nextPointerArray[tagId];
    if (nextPointerArray[tagId] != -1)
        prevPointerArray[nextPointerArray[tagId]] = prevPointerArray[tagId];
    prevPointerArray[tagId] = -1;
    nextPointerArray[tagId] = listHead;
    if (listHead >= 0) {
        if(prevPointerArray[listHead] == -1) prevPointerArray[listHead] = tagId;
        else panic("List head is not actually a list head!");
    }
    if(updateReplacement) rp->update(tagId, req);
}

void uniDoppelgangerTagArray::changeInPlace(Address lineAddr, const MemReq* req, int32_t tagId, int32_t mapId, int32_t listHead, bool approximate, bool updateReplacement) {
    // if (!tagArray[tagId] && lineAddr) {
    //     validLines++;
    // } else if (tagArray[tagId] && !lineAddr) {
    //     validLines--;
    // }
    // rp->replaced(tagId);
    tagArray[tagId] = lineAddr;
    mapPointerArray[tagId] = mapId;
    approximateArray[tagId] = approximate;
    if (prevPointerArray[tagId] != -1)
        nextPointerArray[prevPointerArray[tagId]] = nextPointerArray[tagId];
    if (nextPointerArray[tagId] != -1)
        prevPointerArray[nextPointerArray[tagId]] = prevPointerArray[tagId];
    prevPointerArray[tagId] = -1;
    nextPointerArray[tagId] = listHead;
    if (listHead >= 0) {
        if(prevPointerArray[listHead] == -1) prevPointerArray[listHead] = tagId;
        else panic("List head is not actually a list head!");
    }
    if(updateReplacement) rp->update(tagId, req);
}

int32_t uniDoppelgangerTagArray::readMapId(const int32_t tagId) {
    // assert_msg(approximate[tagId], "must be approximate to read mapId");
    return mapPointerArray[tagId];
}

int32_t uniDoppelgangerTagArray::readDataId(const int32_t tagId) {
    // assert_msg(!approximate[tagId], "must be exact to read dataId");
    return mapPointerArray[tagId];
}

Address uniDoppelgangerTagArray::readAddress(int32_t tagId) {
    return tagArray[tagId];
}

int32_t uniDoppelgangerTagArray::readNextLL(int32_t tagId) {
    return nextPointerArray[tagId];
}

uint32_t uniDoppelgangerTagArray::getValidLines() {
    return validLines;
}

uint32_t uniDoppelgangerTagArray::countValidLines() {
    uint32_t Counter = 0;
    for (uint32_t i = 0; i < numLines; i++) {
        if (mapPointerArray[i] != -1)
            Counter++;
    }
    return Counter;
}

void uniDoppelgangerTagArray::print() {
    for (uint32_t i = 0; i < this->numLines; i++) {
        if (mapPointerArray[i] != -1)
            info("%i: %lu, %i, %i, %i, %s", i, tagArray[i] << lineBits, prevPointerArray[i], nextPointerArray[i], mapPointerArray[i], approximateArray[i]? "approximate":"exact");
    }
}

uniDoppelgangerDataArray::uniDoppelgangerDataArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf) : rp(_rp), hf(_hf), numLines(_numLines), assoc(_assoc)  {
    mtagArray = gm_calloc<int32_t>(numLines);
    tagPointerArray = gm_calloc<int32_t>(numLines);
    approximateArray = gm_calloc<bool>(numLines);
    for (uint32_t i = 0; i < numLines; i++) {
        tagPointerArray[i] = -1;
        mtagArray[i] = -1;
    }
    numSets = numLines/assoc;
    setMask = numSets - 1;
    validLines = 0;
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

uniDoppelgangerDataArray::~uniDoppelgangerDataArray() {
    gm_free(mtagArray);
    gm_free(tagPointerArray);
    gm_free(approximateArray);
}

int32_t uniDoppelgangerDataArray::lookup(uint32_t map, const MemReq* req, bool updateReplacement) {
    uint32_t set = hf->hash(0, map) & setMask;
    uint32_t first = set*assoc;
    for (uint32_t id = first; id < first + assoc; id++) {
        if (mtagArray[id] == (int32_t)map && approximateArray[id] == true) {
            if (updateReplacement) rp->update(id, req);
            return id;
        }
    }
    return -1;
}

uint32_t uniDoppelgangerDataArray::calculateMap(const DataLine data, DataType type, DataValue minValue, DataValue maxValue) {
    // Get hash and map values
    int64_t intAvgHash = 0, intRangeHash = 0;
    double floatAvgHash = 0, floatRangeHash = 0;
    int64_t intMax = std::numeric_limits<int64_t>::min(),
            intMin = std::numeric_limits<int64_t>::max(),
            intSum = 0;
    double floatMax = std::numeric_limits<double>::min(),
            floatMin = std::numeric_limits<double>::max(),
            floatSum = 0;
    double mapStep = 0;
    int32_t avgMap = 0, rangeMap = 0;
    uint32_t map = 0;
    switch (type)
    {
        case ZSIM_UINT8:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(uint8_t); i++) {
                intSum += ((uint8_t*) data)[i];
                if (((uint8_t*) data)[i] > intMax)
                    intMax = ((uint8_t*) data)[i];
                if (((uint8_t*) data)[i] < intMin)
                    intMin = ((uint8_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(uint8_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.UINT8)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.UINT8)
                panic("Received a value lower than the annotation's Min!!");
            if (zinfo->mapSize > sizeof(uint8_t)) {
                avgMap = intAvgHash;
                rangeMap = intRangeHash;
            } else {
                mapStep = (maxValue.UINT8 - minValue.UINT8)/std::pow(2,zinfo->mapSize-1);
                avgMap = intAvgHash/mapStep;
                rangeMap = intRangeHash/mapStep;
            }
            break;
        case ZSIM_INT8:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(int8_t); i++) {
                intSum += ((int8_t*) data)[i];
                if (((int8_t*) data)[i] > intMax)
                    intMax = ((int8_t*) data)[i];
                if (((int8_t*) data)[i] < intMin)
                    intMin = ((int8_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(int8_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.INT8)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.INT8)
                panic("Received a value lower than the annotation's Min!!");
            if (zinfo->mapSize > sizeof(int8_t)) {
                avgMap = intAvgHash;
                rangeMap = intRangeHash;
            } else {
                mapStep = (maxValue.INT8 - minValue.INT8)/std::pow(2,zinfo->mapSize-1);
                avgMap = intAvgHash/mapStep;
                rangeMap = intRangeHash/mapStep;
            }
            break;
        case ZSIM_UINT16:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(uint16_t); i++) {
                intSum += ((uint16_t*) data)[i];
                if (((uint16_t*) data)[i] > intMax)
                    intMax = ((uint16_t*) data)[i];
                if (((uint16_t*) data)[i] < intMin)
                    intMin = ((uint16_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(uint16_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.UINT16)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.UINT16)
                panic("Received a value lower than the annotation's Min!!");
            if (zinfo->mapSize > sizeof(uint16_t)) {
                avgMap = intAvgHash;
                rangeMap = intRangeHash;
            } else {
                mapStep = (maxValue.UINT16 - minValue.UINT16)/std::pow(2,zinfo->mapSize-1);
                avgMap = intAvgHash/mapStep;
                rangeMap = intRangeHash/mapStep;
            }
            break;
        case ZSIM_INT16:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(int16_t); i++) {
                intSum += ((int16_t*) data)[i];
                if (((int16_t*) data)[i] > intMax)
                    intMax = ((int16_t*) data)[i];
                if (((int16_t*) data)[i] < intMin)
                    intMin = ((int16_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(int16_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.INT16)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.INT16)
                panic("Received a value lower than the annotation's Min!!");
            if (zinfo->mapSize > sizeof(int16_t)) {
                avgMap = intAvgHash;
                rangeMap = intRangeHash;
            } else {
                mapStep = (maxValue.INT16 - minValue.INT16)/std::pow(2,zinfo->mapSize-1);
                avgMap = intAvgHash/mapStep;
                rangeMap = intRangeHash/mapStep;
            }
            break;
        case ZSIM_UINT32:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(uint32_t); i++) {
                intSum += ((uint32_t*) data)[i];
                if (((uint32_t*) data)[i] > intMax)
                    intMax = ((uint32_t*) data)[i];
                if (((uint32_t*) data)[i] < intMin)
                    intMin = ((uint32_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(uint32_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.UINT32)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.UINT32)
                panic("Received a value lower than the annotation's Min!!");
            mapStep = (maxValue.UINT32 - minValue.UINT32)/std::pow(2,zinfo->mapSize-1);
            avgMap = intAvgHash/mapStep;
            rangeMap = intRangeHash/mapStep;
            break;
        case ZSIM_INT32:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(int32_t); i++) {
                intSum += ((int32_t*) data)[i];
                if (((int32_t*) data)[i] > intMax)
                    intMax = ((int32_t*) data)[i];
                if (((int32_t*) data)[i] < intMin)
                    intMin = ((int32_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(int32_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.INT32)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.INT32)
                panic("Received a value lower than the annotation's Min!!");
            mapStep = (maxValue.INT32 - minValue.INT32)/std::pow(2,zinfo->mapSize-1);
            avgMap = intAvgHash/mapStep;
            rangeMap = intRangeHash/mapStep;
            break;
        case ZSIM_UINT64:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(uint64_t); i++) {
                intSum += ((uint64_t*) data)[i];
                if ((int64_t)(((uint64_t*) data)[i]) > intMax)
                    intMax = ((uint64_t*) data)[i];
                if ((int64_t)(((uint64_t*) data)[i]) < intMin)
                    intMin = ((uint64_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(uint64_t));
            intRangeHash = intMax - intMin;
            if (intMax > (int64_t)maxValue.UINT64)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < (int64_t)minValue.UINT64)
                panic("Received a value lower than the annotation's Min!!");
            mapStep = (maxValue.UINT64 - minValue.UINT64)/std::pow(2,zinfo->mapSize-1);
            avgMap = intAvgHash/mapStep;
            rangeMap = intRangeHash/mapStep;
            break;
        case ZSIM_INT64:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(int64_t); i++) {
                intSum += ((int64_t*) data)[i];
                if (((int64_t*) data)[i] > intMax)
                    intMax = ((int64_t*) data)[i];
                if (((int64_t*) data)[i] < intMin)
                    intMin = ((int64_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(int64_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.INT64)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.INT64)
                panic("Received a value lower than the annotation's Min!!");
            mapStep = (maxValue.INT64 - minValue.INT64)/std::pow(2,zinfo->mapSize-1);
            avgMap = intAvgHash/mapStep;
            rangeMap = intRangeHash/mapStep;
            break;
        case ZSIM_FLOAT:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(float); i++) {
                floatSum += ((float*) data)[i];
                if (((float*) data)[i] > floatMax)
                    floatMax = ((float*) data)[i];
                if (((float*) data)[i] < floatMin)
                    floatMin = ((float*) data)[i];
            }
            floatAvgHash = floatSum/(zinfo->lineSize/sizeof(float));
            floatRangeHash = floatMax - floatMin;
            // if (floatMax > maxValue.FLOAT)
                // warn("Received a value bigger than the annotation's Max!! %.10f, %.10f", floatMax, maxValue.FLOAT);
            // if (floatMin < minValue.FLOAT)
                // warn("Received a value lower than the annotation's Min!! %.10f, %.10f", floatMin, minValue.FLOAT);
            mapStep = (maxValue.FLOAT - minValue.FLOAT)/std::pow(2,zinfo->mapSize-1);
            avgMap = floatAvgHash/mapStep;
            rangeMap = floatRangeHash/mapStep;
            break;
        case ZSIM_DOUBLE:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(double); i++) {
                floatSum += ((double*) data)[i];
                if (((double*) data)[i] > floatMax)
                    floatMax = ((double*) data)[i];
                if (((double*) data)[i] < floatMin)
                    floatMin = ((double*) data)[i];
            }
            floatAvgHash = floatSum/(zinfo->lineSize/sizeof(double));
            floatRangeHash = floatMax - floatMin;
            // if (floatMax > maxValue.DOUBLE)
                // warn("Received a value bigger than the annotation's Max!! %.10f, %.10f", floatMax, maxValue.DOUBLE);
            // if (floatMin < minValue.DOUBLE)
                // warn("Received a value lower than the annotation's Min!! %.10f, %.10f", floatMin, minValue.DOUBLE);
            mapStep = (maxValue.DOUBLE - minValue.DOUBLE)/std::pow(2,zinfo->mapSize-1);
            avgMap = floatAvgHash/mapStep;
            rangeMap = floatRangeHash/mapStep;
            break;
        default:
            panic("Wrong Data Type!!");
    }
    map = ((uint32_t)avgMap << (32 - zinfo->mapSize)) >> (32 - zinfo->mapSize);
    rangeMap = ((uint32_t)rangeMap << (32 - zinfo->mapSize/2)) >> (32 - zinfo->mapSize/2);
    rangeMap = (rangeMap << zinfo->mapSize);
    map |= rangeMap;
    return map;
}

int32_t uniDoppelgangerDataArray::preinsert(uint32_t map, const MemReq* req, int32_t* tagId) {
    uint32_t set = hf->hash(0, map) & setMask;
    uint32_t first = set*assoc;

    uint32_t mapId = rp->rankCands(req, SetAssocCands(first, first+assoc));

    *tagId = tagPointerArray[mapId];
    return mapId;
}

void uniDoppelgangerDataArray::postinsert(int32_t map, const MemReq* req, int32_t mapId, int32_t tagId, bool approximate, bool updateReplacement) {
    if (tagPointerArray[mapId] == -1 && tagId != -1) {
        validLines++;
    } else if (tagPointerArray[mapId] != -1 && tagId == -1) {
        assert(validLines);
        validLines--;
    }
    rp->replaced(mapId);
    mtagArray[mapId] = map;
    tagPointerArray[mapId] = tagId;
    approximateArray[mapId] = approximate;
    if(updateReplacement) rp->update(mapId, req);
}

void uniDoppelgangerDataArray::changeInPlace(int32_t map, const MemReq* req, int32_t mapId, int32_t tagId, bool approximate, bool updateReplacement) {
    mtagArray[mapId] = map;
    tagPointerArray[mapId] = tagId;
    approximateArray[mapId] = approximate;
    if(updateReplacement) rp->update(mapId, req);
}

int32_t uniDoppelgangerDataArray::readListHead(int32_t mapId) {
    return tagPointerArray[mapId];
}

int32_t uniDoppelgangerDataArray::readMap(int32_t mapId) {
    return mtagArray[mapId];
}

void uniDoppelgangerDataArray::print() {
    for (uint32_t i = 0; i < this->numLines; i++) {
        if (tagPointerArray[i] != -1)
            info("%i: %i, %i, %s", i, mtagArray[i], tagPointerArray[i], approximateArray[i]? "approximate":"exact");
    }
}

uint32_t uniDoppelgangerDataArray::getValidLines() {
    return validLines;
}

uint32_t uniDoppelgangerDataArray::countValidLines() {
    uint32_t Counter = 0;
    for (uint32_t i = 0; i < numLines; i++) {
        if (tagPointerArray[i] != -1)
            Counter++;
    }
    return Counter;
}
// uniDoppelganger End

// BDI Begin
ApproximateBDITagArray::ApproximateBDITagArray(uint32_t _numLines, uint32_t _assoc, uint32_t _dataAssoc, ReplPolicy* _rp, HashFamily* _hf) : 
rp(_rp), hf(_hf), numLines(_numLines), assoc(_assoc), dataAssoc(_dataAssoc) {
    tagArray = gm_calloc<Address>(numLines);
    segmentPointerArray = gm_malloc<int32_t>(numLines);
    compressionEncodingArray = gm_malloc<BDICompressionEncoding>(numLines);
    for (uint32_t i = 0; i < numLines; i++) {
        segmentPointerArray[i] = -1;
        compressionEncodingArray[i] = NONE;
    }
    approximateArray = gm_calloc<bool>(numLines);
    numSets = numLines/assoc;
    setMask = numSets - 1;
    validLines = 0;
    dataValidSegments = 0;
    info("BDI Tag Array: %i lines and %i sets", numLines, numSets);
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

ApproximateBDITagArray::~ApproximateBDITagArray() {
    gm_free(tagArray);
    gm_free(segmentPointerArray);
    gm_free(compressionEncodingArray);
    gm_free(approximateArray);
}

int32_t ApproximateBDITagArray::lookup(Address lineAddr, const MemReq* req, bool updateReplacement) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;
    for (uint32_t id = first; id < first + assoc; id++) {
        if (tagArray[id] ==  lineAddr) {
            if (updateReplacement) rp->update(id, req);
            return id;
        }
    }
    return -1;
}

int32_t ApproximateBDITagArray::preinsert(Address lineAddr, const MemReq* req, Address* wbLineAddr) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;

    uint32_t candidate = rp->rankCands(req, SetAssocCands(first, first+assoc));

    *wbLineAddr = tagArray[candidate];
    return candidate;
}

int32_t ApproximateBDITagArray::needEviction(Address lineAddr, const MemReq* req, uint16_t size, g_vector<uint32_t>& alreadyEvicted, Address* wbLineAddr) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;
    uint16_t occupiedSpace = 0;
    for (uint32_t id = first; id < first + assoc; id++) {
        bool found = false;
        for (uint32_t i = 0; i < alreadyEvicted.size(); i++) {
            if (alreadyEvicted[i] == id) {found = true; break;}
        }
        if (segmentPointerArray[id] != -1 && !found) {
            occupiedSpace += BDICompressionToSize(compressionEncodingArray[id], zinfo->lineSize);
        }
    }
    if (dataAssoc*zinfo->lineSize - occupiedSpace >= size)
        return -1;
    else {
        uint32_t candidate = rp->rank(req, SetAssocCands(first, first+assoc), alreadyEvicted);
        *wbLineAddr = tagArray[candidate];
        return candidate;
    }
}

void ApproximateBDITagArray::postinsert(Address lineAddr, const MemReq* req, int32_t tagId, int8_t segmentId, BDICompressionEncoding compression, bool approximate, bool updateReplacement) {
    if (!tagArray[tagId] && lineAddr) {
        validLines++;
        dataValidSegments+=BDICompressionToSize(compression, zinfo->lineSize)/8;
    } else if (tagArray[tagId] && !lineAddr) {
        validLines--;
        dataValidSegments-=BDICompressionToSize(compressionEncodingArray[tagId], zinfo->lineSize)/8;
        assert(validLines);
        assert(dataValidSegments);
    } else {
        dataValidSegments-=BDICompressionToSize(compressionEncodingArray[tagId], zinfo->lineSize)/8;
        dataValidSegments+=BDICompressionToSize(compression, zinfo->lineSize)/8;
    }
    rp->replaced(tagId);
    tagArray[tagId] = lineAddr;
    segmentPointerArray[tagId] = segmentId;
    compressionEncodingArray[tagId] = compression;
    approximateArray[tagId] = approximate;
    if(updateReplacement) rp->update(tagId, req);
}

BDICompressionEncoding ApproximateBDITagArray::readCompressionEncoding(int32_t tagId) {
    return compressionEncodingArray[tagId];
}

int8_t ApproximateBDITagArray::readSegmentPointer(int32_t tagId) {
    return segmentPointerArray[tagId];
}

void ApproximateBDITagArray::writeCompressionEncoding(int32_t tagId, BDICompressionEncoding encoding) {
    dataValidSegments-=BDICompressionToSize(compressionEncodingArray[tagId], zinfo->lineSize)/8;
    compressionEncodingArray[tagId] = encoding;
    dataValidSegments+=BDICompressionToSize(encoding, zinfo->lineSize)/8;
}

uint32_t ApproximateBDITagArray::getValidLines() {
    return validLines;
}

uint32_t ApproximateBDITagArray::countValidLines() {
    uint32_t Counter = 0;
    for (uint32_t i = 0; i < numLines; i++) {
        if (segmentPointerArray[i] != -1)
            Counter++;
    }
    return Counter;
}

uint32_t ApproximateBDITagArray::getDataValidSegments() {
    return dataValidSegments;
}

uint32_t ApproximateBDITagArray::countDataValidSegments() {
    uint32_t Counter = 0;
    for (uint32_t i = 0; i < numLines; i++) {
        if (segmentPointerArray[i] != -1)
            Counter+=BDICompressionToSize(compressionEncodingArray[i], zinfo->lineSize)/8;
    }
    return Counter;
}

void ApproximateBDITagArray::print() {
    for (uint32_t i = 0; i < this->numLines; i++) {
        if (segmentPointerArray[i] != -1)
            info("%i: %lu, %i, %s", i, tagArray[i] << lineBits, BDICompressionToSize(compressionEncodingArray[i], zinfo->lineSize), approximateArray[i]? "approximate":"exact");
    }
}

// // TODO: This was copied varbatim from https://github.com/CMU-SAFARI/BDICompression/blob/master/compression.c
// // optimize for our purposes later.
// uint64_t ApproximateBDIDataArray::my_llabs(int64_t x) {
//     uint64_t t = x >> 63;
//     return (x ^ t) - t;
// }

// // TODO: This was copied varbatim from https://github.com/CMU-SAFARI/BDICompression/blob/master/compression.c
// // and/or Amin. optimize for our purposes later.
// uint8_t ApproximateBDIDataArray::multiBaseCompression(uint64_t * values, uint8_t size, uint8_t blimit, uint8_t bsize) {
//     uint64_t limit = 0;
//     uint8_t numBase = 2;

//     //define the appropriate size for the mask
//     switch(blimit){
//         case 1:
//             limit = 0xFF;
//             break;
//         case 2:
//             limit = 0xFFFF;
//             break;
//         case 4:
//             limit = 0xFFFFFFFF;
//             break;
//         default:
//             panic("Wrong BDI Size");
//             return 0;
//     }

//     uint64_t mnumBase [64];
//     uint8_t baseCount = 1;
//     mnumBase[0] = 0;// values[0];

//     uint8_t i,j;
//     for (i = 0; i < size; i++) {
//         bool isFound=0;
//         for(j = 0; j <  baseCount; j++) {
//             if(my_llabs((int64_t)(mnumBase[j] -  values[i])) <= limit) {
//                 isFound = 1;
//                 break;
//             }
//         }
//         if(isFound == 0)
//             mnumBase[baseCount++] = values[i];
//         if(baseCount >= numBase)
//             break;
//     }
//     uint8_t compCount = 0;
//     for (i = 0; i < size; i++) {
//         for(j = 0; j <  baseCount; j++) {
//             if(my_llabs((int64_t)(mnumBase[j] -  values[i])) <= limit) {
//                 compCount++;
//                 break;
//             }
//         }
//     }

//     //return compressed size
//     uint8_t mCompSize = blimit * compCount + bsize * (numBase-1) + (size - compCount) * bsize; // implicit zero base gozashtim

//     uint8_t retVal = mCompSize;
//     if(compCount < size) {
//         retVal =  size * bsize;
//     }

//     return retVal;
// }

static unsigned long long my_llabs ( long long x )
{
   unsigned long long t = x >> 63;
   return (x ^ t) - t;
}

long long unsigned * convertBuffer2Array (char * buffer, unsigned size, unsigned step)
{
      long long unsigned * values = (long long unsigned *) malloc(sizeof(long long unsigned) * size/step);
//      std::cout << std::dec << "ConvertBuffer = " << size/step << std::endl;
     //init
     unsigned int i,j; 
     for (i = 0; i < size / step; i++) {
          values[i] = 0;    // Initialize all elements to zero.
      }
      //SIM_printf("Element Size = %d \n", step);
      for (i = 0; i < size; i += step ){
          for (j = 0; j < step; j++){
              //SIM_printf("Buffer = %02x \n", (unsigned char) buffer[i + j]);
              values[i / step] += (long long unsigned)((unsigned char)buffer[i + j]) << (8*j);
              //SIM_printf("step %d value = ", j);
              //printLLwithSize(values[i / step], step);  
          }
          //std::cout << "Current value = " << values[i / step] << std::endl;
          //printLLwithSize(values[i / step], step);
          //SIM_printf("\n");
      }
      //std::cout << "End ConvertBuffer = " << size/step << std::endl;
      return values;
}

///
/// Check if the cache line consists of only zero values
///
int isZeroPackable ( long long unsigned * values, unsigned size){
  int nonZero = 0;
  unsigned int i;
  for (i = 0; i < size; i++) {
      if( values[i] != 0){
          nonZero = 1;
          break;
      }
  }
  return !nonZero;
}

///
/// Check if the cache line consists of only same values
///
int isSameValuePackable ( long long unsigned * values, unsigned size){
  int notSame = 0;
  unsigned int i;
  for (i = 0; i < size; i++) {
      if( values[0] != values[i]){
          notSame = 1;
          break;
      }
  }
  return !notSame;
}

///
/// Check if the cache line values can be compressed with multiple base + 1,2,or 4-byte offset 
/// Returns size after compression 
///
unsigned doubleExponentCompression ( long long unsigned * values, unsigned size, unsigned blimit, unsigned bsize){
  unsigned long long limit = 0;
  //define the appropriate size for the mask
  switch(blimit){
    case 1:
      limit = 56;
      break;
    case 2:
      limit = 48;
      break;
    default:
      // std::cout << "Wrong blimit value = " <<  blimit << std::endl;
      exit(1);
  }
  // finding bases: # BASES
  // find how many elements can be compressed with mbases
  unsigned compCount = 0;
  unsigned int i;
  for (i = 0; i < size; i++) {
         if( (values[0] >> limit) ==  (values[i] >> limit))  {
             compCount++;
         }
  }
  //return compressed size
  if(compCount != size )
     return size * bsize;
  return size * bsize - (compCount - 1) * blimit;
}


///
/// Check if the cache line values can be compressed with multiple base + 1,2,or 4-byte offset 
/// Returns size after compression 
///
unsigned multBaseCompression ( long long unsigned * values, unsigned size, unsigned blimit, unsigned bsize){
  unsigned long long limit = 0;
  unsigned BASES = 2;
  //define the appropriate size for the mask
  switch(blimit){
    case 1:
      limit = 0xFF;
      break;
    case 2:
      limit = 0xFFFF;
      break;
    case 4:
      limit = 0xFFFFFFFF;
      break;
    default:
      //std::cout << "Wrong blimit value = " <<  blimit << std::endl;
      exit(1);
  }
  // finding bases: # BASES
  //std::vector<unsigned long long> mbases;
  //mbases.push_back(values[0]); //add the first base
  unsigned long long mbases [64];
  unsigned baseCount = 1;
  mbases[0] = 0;
  unsigned int i,j;
  for (i = 0; i < size; i++) {
      for(j = 0; j <  baseCount; j++){
         if( my_llabs((long long int)(mbases[j] -  values[i])) > limit ){
             //mbases.push_back(values[i]); // add new base
             mbases[baseCount++] = values[i];  
         }
     }
     if(baseCount >= BASES) //we don't have more bases
       break;
  }
  // find how many elements can be compressed with mbases
  unsigned compCount = 0;
  for (i = 0; i < size; i++) {
      //ol covered = 0;
      for(j = 0; j <  baseCount; j++){
         if( my_llabs((long long int)(mbases[j] -  values[i])) <= limit ){
             compCount++;
             break;
         }
     }
  }
  //return compressed size
  unsigned mCompSize = blimit * compCount + bsize * (BASES-1) + (size - compCount) * bsize;
  if(compCount < size)
     return size * bsize;
  //VG_(printf)("%d-bases bsize = %d osize = %d CompCount = %d CompSize = %d\n", BASES, bsize, blimit, compCount, mCompSize);
  return mCompSize;
}

unsigned BDICompress (char * buffer, unsigned _blockSize)
{
  //char * dst = new char [_blockSize];
//  print_value(buffer, _blockSize);
 
  long long unsigned * values = convertBuffer2Array( buffer, _blockSize, 8);
  unsigned bestCSize = _blockSize;
  unsigned currCSize = _blockSize;
  if( isZeroPackable( values, _blockSize / 8))
      bestCSize = 1;
  if( isSameValuePackable( values, _blockSize / 8))
      currCSize = 8;
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  currCSize = multBaseCompression( values, _blockSize / 8, 1, 8);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  currCSize = multBaseCompression( values, _blockSize / 8, 2, 8);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  currCSize =  multBaseCompression( values, _blockSize / 8, 4, 8);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  free(values);
  values = convertBuffer2Array( buffer, _blockSize, 4);
  // if( isSameValuePackable( values, _blockSize / 4))
  //    currCSize = 4;
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  currCSize = multBaseCompression( values, _blockSize / 4, 1, 4);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  currCSize = multBaseCompression( values, _blockSize / 4, 2, 4);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  free(values);
  values = convertBuffer2Array( buffer, _blockSize, 2);
  currCSize = multBaseCompression( values, _blockSize / 2, 1, 2);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  free(values);

  //exponent base compression
  /*values = convertBuffer2Array( buffer, _blockSize, 8);
  currCSize = doubleExponentCompression( values, _blockSize / 8, 2, 8);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  currCSize = doubleExponentCompression( values, _blockSize / 8, 1, 8);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  VG_(free)(values);*/
 
  //delete [] buffer;
  buffer = NULL;
  values = NULL;
  //SIM_printf(" BestCSize = %d \n", bestCSize);
  return bestCSize;

}

BDICompressionEncoding ApproximateBDIDataArray::compress(const DataLine data, uint16_t* size) {
    // info("\tApproximate Data: %lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu", ((uint64_t*)data)[0], ((uint64_t*)data)[1], ((uint64_t*)data)[2], ((uint64_t*)data)[3], ((uint64_t*)data)[4], ((uint64_t*)data)[5], ((uint64_t*)data)[6], ((uint64_t*)data)[7]);
    *size = BDICompress((char*)data, 64);
    if (*size == 1){                                                                               // Size 1
        *size = 8;
        // info("Compression: ZERO, %i segments", 1);
        return ZERO;
    }
    if (*size == 8){                                                                               // Size 8
        *size = 8;
        // info("Compression: REPETITIVE, %i segments", 1);
        return REPETITIVE;
    }
    if (*size == 16){                                                                              // Size 16
        *size = 16;
        // info("Compression: BASE8DELTA1, %i segments", 2);
        return BASE8DELTA1;
    }
    if (*size == 20){        // Size 20
        *size = 24;
        // info("Compression: BASE4DELTA1, %i segments", 3);
        return BASE4DELTA1;
    }
    if (*size == 24){        // Size 24
        *size = 24;
        // info("Compression: BASE8DELTA2, %i segments", 3);
        return BASE8DELTA2;
    }
    if (*size == 34){        // Size 34
        *size = 40;
        // info("Compression: BASE2DELTA1, %i segments", 5);
        return BASE2DELTA1;
    }
    if (*size == 36){        // Size 36
        *size = 40;
        // info("Compression: BASE4DELTA2, %i segments", 5);
        return BASE4DELTA2;
    }
    if (*size == 40){        // Size 40
        *size = 40;
        // info("Compression: BASE8DELTA4, %i segments", 5);
        return BASE8DELTA4;
    }
    if (*size == zinfo->lineSize){
        return NONE;
    }
    panic("impossible compress size %i", *size);
}

void ApproximateBDIDataArray::approximate(const DataLine data, DataType type) {
    // info("\tExact Data: %lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu", ((uint64_t*)data)[0], ((uint64_t*)data)[1], ((uint64_t*)data)[2], ((uint64_t*)data)[3], ((uint64_t*)data)[4], ((uint64_t*)data)[5], ((uint64_t*)data)[6], ((uint64_t*)data)[7]);
    if (type == ZSIM_FLOAT) {
        for (uint16_t i = 0; i < zinfo->lineSize/4; i++)
        {
            ((uint32_t*) data)[i] = ((uint32_t*) data)[i] >> zinfo->floatCutSize;
            // ((uint32_t*) data)[i] = ((uint32_t*) data)[i] << zinfo->floatCutSize;
        }
    } else if (type == ZSIM_DOUBLE) {
        for (uint16_t i = 0; i < zinfo->lineSize/8; i++)
        {
            ((uint64_t*) data)[i] = ((uint64_t*) data)[i] >> zinfo->doubleCutSize;
            // ((uint64_t*) data)[i] = ((uint64_t*) data)[i] << zinfo->doubleCutSize;
        }
    } else {
        panic("We only approximate floats and doubles");
    }
}
// BDI end

// Dedup begin
ApproximateDedupTagArray::ApproximateDedupTagArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf) : rp(_rp), hf(_hf), numLines(_numLines), assoc(_assoc)  {
    tagArray = gm_calloc<Address>(numLines);
    prevPointerArray = gm_calloc<int32_t>(numLines);
    nextPointerArray = gm_calloc<int32_t>(numLines);
    dataPointerArray = gm_calloc<int32_t>(numLines);
    approximateArray = gm_calloc<bool>(numLines);
    for (uint32_t i = 0; i < numLines; i++) {
        prevPointerArray[i] = -1;
        nextPointerArray[i] = -1;
        dataPointerArray[i] = -1;
    }
    numSets = numLines/assoc;
    setMask = numSets - 1;
    validLines = 0;
    info("Dedup Tag Array: %i lines and %i sets", numLines, numSets);
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

ApproximateDedupTagArray::~ApproximateDedupTagArray() {
    gm_free(tagArray);
    gm_free(prevPointerArray);
    gm_free(nextPointerArray);
    gm_free(dataPointerArray);
    gm_free(approximateArray);
}

int32_t ApproximateDedupTagArray::lookup(Address lineAddr, const MemReq* req, bool updateReplacement) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;
    for (uint32_t id = first; id < first + assoc; id++) {
        if (tagArray[id] ==  lineAddr) {
            if (updateReplacement) rp->update(id, req);
            return id;
        }
    }
    return -1;
}

int32_t ApproximateDedupTagArray::preinsert(Address lineAddr, const MemReq* req, Address* wbLineAddr) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;

    uint32_t candidate = rp->rankCands(req, SetAssocCands(first, first+assoc));

    *wbLineAddr = tagArray[candidate];
    return candidate;
}

bool ApproximateDedupTagArray::evictAssociatedData(int32_t lineId, int32_t* newLLHead, bool* approximate) {
    *newLLHead = -1;
    if (dataPointerArray[lineId] == -1)
        return false;
    if (prevPointerArray[lineId] != -1)
        return false;
    else
        *newLLHead = nextPointerArray[lineId];
    if (nextPointerArray[lineId] != -1)
        return false;
    return true;
}

void ApproximateDedupTagArray::postinsert(Address lineAddr, const MemReq* req, int32_t tagId, int32_t dataId, int32_t listHead, bool approximate, bool updateReplacement) {
    if (!tagArray[tagId] && lineAddr) {
        validLines++;
    } else if (tagArray[tagId] && !lineAddr) {
        assert(validLines);
        validLines--;
    }
    rp->replaced(tagId);
    tagArray[tagId] = lineAddr;
    dataPointerArray[tagId] = dataId;
    approximateArray[tagId] = approximate;
    if (prevPointerArray[tagId] != -1)
        nextPointerArray[prevPointerArray[tagId]] = nextPointerArray[tagId];
    if (nextPointerArray[tagId] != -1)
        prevPointerArray[nextPointerArray[tagId]] = prevPointerArray[tagId];
    prevPointerArray[tagId] = -1;
    nextPointerArray[tagId] = listHead;
    if (listHead >= 0) {
        if(prevPointerArray[listHead] == -1) prevPointerArray[listHead] = tagId;
        else panic("List head %i is not actually a list head! %i is.", listHead, prevPointerArray[listHead]);
    }
    if(updateReplacement) rp->update(tagId, req);
    // info("Tag %i: %lu, %i, %i, %i, %s", tagId, tagArray[tagId] << lineBits, prevPointerArray[tagId], nextPointerArray[tagId], dataPointerArray[tagId], approximateArray[tagId]? "approximate":"exact");
    // if (prevPointerArray[tagId] != -1)
    //     info("Tag %i: %lu, %i, %i, %i, %s", prevPointerArray[tagId], tagArray[prevPointerArray[tagId]] << lineBits, prevPointerArray[prevPointerArray[tagId]], nextPointerArray[prevPointerArray[tagId]], dataPointerArray[prevPointerArray[tagId]], approximateArray[prevPointerArray[tagId]]? "approximate":"exact");
    // if (nextPointerArray[tagId] != -1)
    //     info("Tag %i: %lu, %i, %i, %i, %s", nextPointerArray[tagId], tagArray[nextPointerArray[tagId]] << lineBits, prevPointerArray[nextPointerArray[tagId]], nextPointerArray[nextPointerArray[tagId]], dataPointerArray[nextPointerArray[tagId]], approximateArray[nextPointerArray[tagId]]? "approximate":"exact");
}

void ApproximateDedupTagArray::changeInPlace(Address lineAddr, const MemReq* req, int32_t tagId, int32_t dataId, int32_t listHead, bool approximate, bool updateReplacement) {
    tagArray[tagId] = lineAddr;
    dataPointerArray[tagId] = dataId;
    approximateArray[tagId] = approximate;
    if (prevPointerArray[tagId] != -1)
        nextPointerArray[prevPointerArray[tagId]] = nextPointerArray[tagId];
    if (nextPointerArray[tagId] != -1)
        prevPointerArray[nextPointerArray[tagId]] = prevPointerArray[tagId];
    prevPointerArray[tagId] = -1;
    nextPointerArray[tagId] = listHead;
    if (listHead >= 0) {
        if(prevPointerArray[listHead] == -1) prevPointerArray[listHead] = tagId;
        else panic("List head %i is not actually a list head! %i is.", listHead, prevPointerArray[listHead]);
    }
    if(updateReplacement) rp->update(tagId, req);
    // info("Tag %i: %lu, %i, %i, %i, %s", tagId, tagArray[tagId] << lineBits, prevPointerArray[tagId], nextPointerArray[tagId], dataPointerArray[tagId], approximateArray[tagId]? "approximate":"exact");
    // if (prevPointerArray[tagId] != -1)
    //     info("Tag %i: %lu, %i, %i, %i, %s", prevPointerArray[tagId], tagArray[prevPointerArray[tagId]] << lineBits, prevPointerArray[prevPointerArray[tagId]], nextPointerArray[prevPointerArray[tagId]], dataPointerArray[prevPointerArray[tagId]], approximateArray[prevPointerArray[tagId]]? "approximate":"exact");
    // if (nextPointerArray[tagId] != -1)
    //     info("Tag %i: %lu, %i, %i, %i, %s", nextPointerArray[tagId], tagArray[nextPointerArray[tagId]] << lineBits, prevPointerArray[nextPointerArray[tagId]], nextPointerArray[nextPointerArray[tagId]], dataPointerArray[nextPointerArray[tagId]], approximateArray[nextPointerArray[tagId]]? "approximate":"exact");
}

int32_t ApproximateDedupTagArray::readDataId(const int32_t tagId) {
    // assert_msg(!approximate[tagId], "must be exact to read dataId");
    return dataPointerArray[tagId];
}

Address ApproximateDedupTagArray::readAddress(int32_t tagId) {
    return tagArray[tagId];
}

int32_t ApproximateDedupTagArray::readNextLL(int32_t tagId) {
    // info("Next LL: %i", nextPointerArray[tagId]);
    return nextPointerArray[tagId];
}

int32_t ApproximateDedupTagArray::readPrevLL(int32_t tagId) {
    return prevPointerArray[tagId];
}

uint32_t ApproximateDedupTagArray::getValidLines() {
    return validLines;
}

uint32_t ApproximateDedupTagArray::countValidLines() {
    uint32_t Counter = 0;
    for (uint32_t i = 0; i < numLines; i++) {
        if (dataPointerArray[i] != -1)
            Counter++;
    }
    return Counter;
}

void ApproximateDedupTagArray::print() {
    for (uint32_t i = 0; i < this->numLines; i++) {
        if (dataPointerArray[i] != -1)
            info("%i: %lu, %i, %i, %i, %s", i, tagArray[i] << lineBits, prevPointerArray[i], nextPointerArray[i], dataPointerArray[i], approximateArray[i]? "approximate":"exact");
    }
}

ApproximateDedupDataArray::ApproximateDedupDataArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf) : rp(_rp), hf(_hf), numLines(_numLines), assoc(_assoc)  {
    tagCounterArray = gm_calloc<int32_t>(numLines);
    tagPointerArray = gm_malloc<int32_t>(numLines);
    approximateArray = gm_calloc<bool>(numLines);
    dataArray = gm_calloc<DataLine>(numLines);
    for (uint32_t i = 0; i < numLines; i++) {
        tagPointerArray[i] = -1;
        dataArray[i] = gm_calloc<uint8_t>(zinfo->lineSize);
        freeList.push_back(i);
    }
    numSets = numLines/assoc;
    setMask = numSets - 1;
    validLines = 0;
    // srand (time(NULL));
    std::random_device rd;
    RNG = new std::mt19937(rd());
    DIST = new std::uniform_int_distribution<>(0, numLines-1);
    info("Dedup Data Array: %i lines and %i sets", numLines, numSets);
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

ApproximateDedupDataArray::~ApproximateDedupDataArray() {
    gm_free(tagCounterArray);
    gm_free(tagPointerArray);
    gm_free(approximateArray);
    for (uint32_t i = 0; i < numLines; i++) {
        gm_free(dataArray[i]);
    }
    gm_free(dataArray);
}

void ApproximateDedupDataArray::lookup(int32_t dataId, const MemReq* req, bool updateReplacement) {
    if (updateReplacement) rp->update(dataId, req);
}

int32_t ApproximateDedupDataArray::preinsert(int32_t* tagPointer) {
    int32_t leastValue = 999999;
    int32_t leastId = 0;
    if (freeList.size()) {
        leastId = freeList.back();
        freeList.pop_back();
        *tagPointer = tagPointerArray[leastId];
        return leastId;
    }
    for (uint32_t i = 0; i < 4; i++) {
        int32_t id = DIST->operator()(*RNG);
        if (tagPointerArray[id] == -1) {
            *tagPointer = tagPointerArray[id];
            panic("Shouldn't happen");
            return id;
        }
        if (tagCounterArray[id] < leastValue) {
            leastValue = tagCounterArray[id];
            leastId = id;
        }
    }
    *tagPointer = tagPointerArray[leastId];
    return leastId;
}

void ApproximateDedupDataArray::postinsert(int32_t tagId, const MemReq* req, int32_t counter, int32_t dataId, bool approximate, DataLine data, bool updateReplacement) {
    if (tagPointerArray[dataId] == -1 && tagId != -1) {
        validLines++;
        auto it = std::find(freeList.begin(), freeList.end(), dataId);
        if(it != freeList.end()) {
            auto index = std::distance(freeList.begin(), it);
            freeList.erase(freeList.begin() + index);
        }
    } else if (tagPointerArray[dataId] != -1 && tagId == -1) {
        validLines--;
        freeList.push_back(dataId);
        assert(validLines);
    }
    if (data)
        PIN_SafeCopy(dataArray[dataId], data, zinfo->lineSize);
    rp->replaced(dataId);
    tagCounterArray[dataId] = counter;
    tagPointerArray[dataId] = tagId;
    approximateArray[dataId] = approximate;
    if(updateReplacement) rp->update(dataId, req);
    // info("Data %i: %i, %i, %s", dataId, tagCounterArray[dataId], tagPointerArray[dataId], approximateArray[dataId]? "approximate":"exact");
}

void ApproximateDedupDataArray::changeInPlace(int32_t tagId, const MemReq* req, int32_t counter, int32_t dataId, bool approximate, DataLine data, bool updateReplacement) {
    if (tagPointerArray[dataId] == -1 && tagId != -1) {
        validLines++;
    } else if (tagPointerArray[dataId] != -1 && tagId == -1) {
        validLines--;
        assert(validLines);
    }
    if (data)
        PIN_SafeCopy(dataArray[dataId], data, zinfo->lineSize);
    // rp->replaced(dataId);
    tagCounterArray[dataId] = counter;
    tagPointerArray[dataId] = tagId;
    approximateArray[dataId] = approximate;
    if(updateReplacement) rp->update(dataId, req);
    // info("Data %i: %i, %i, %s", dataId, tagCounterArray[dataId], tagPointerArray[dataId], approximateArray[dataId]? "approximate":"exact");
}

bool ApproximateDedupDataArray::isSame(int32_t dataId, DataLine data) {
    for (uint32_t i = 0; i < zinfo->lineSize/8; i++)
        if (((uint64_t*)data)[i] != ((uint64_t*)dataArray[dataId])[i])
            return false;
    return true;
}

int32_t ApproximateDedupDataArray::readListHead(int32_t dataId) {
    return tagPointerArray[dataId];
}

int32_t ApproximateDedupDataArray::readCounter(int32_t dataId) {
    return tagCounterArray[dataId];
}

DataLine ApproximateDedupDataArray::readData(int32_t dataId) {
    return dataArray[dataId];
}

void ApproximateDedupDataArray::writeData(int32_t dataId, DataLine data, const MemReq* req, bool updateReplacement) {
    PIN_SafeCopy(dataArray[dataId], data, zinfo->lineSize);
    if(updateReplacement) rp->update(dataId, req);
}

uint32_t ApproximateDedupDataArray::getValidLines() {
    return validLines;
}

uint32_t ApproximateDedupDataArray::countValidLines() {
    uint32_t Counter = 0;
    for (uint32_t i = 0; i < numLines; i++) {
        if (tagPointerArray[i] != -1)
            Counter++;
    }
    return Counter;
}

void ApproximateDedupDataArray::print() {
    for (uint32_t i = 0; i < this->numLines; i++) {
        if (tagPointerArray[i] != -1)
            info("%i: %i, %i, %s", i, tagCounterArray[i], tagPointerArray[i], approximateArray[i]? "approximate":"exact");
    }
}

ApproximateDedupHashArray::ApproximateDedupHashArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf, H3HashFamily* _dataHash) : rp(_rp), hf(_hf), dataHash(_dataHash), numLines(_numLines), assoc(_assoc)  {
    hashArray = gm_malloc<uint64_t>(numLines);
    dataPointerArray = gm_malloc<int32_t>(numLines);
    for (uint32_t i = 0; i < numLines; i++) {
        dataPointerArray[i] = -1;
        hashArray[i] = -1;
    }
    numSets = numLines/assoc;
    setMask = numSets - 1;
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

ApproximateDedupHashArray::~ApproximateDedupHashArray() {
    gm_free(hashArray);
    gm_free(dataPointerArray);
}

void ApproximateDedupHashArray::registerDataArray(ApproximateDedupDataArray* dataArray) {
    this->dataArray = dataArray;
}

int32_t ApproximateDedupHashArray::lookup(uint64_t hash, const MemReq* req, bool updateReplacement) {
    uint32_t set = hf->hash(0, hash) & setMask;
    uint32_t first = set*assoc;
    for (uint32_t id = first; id < first + assoc; id++) {
        if (hashArray[id] ==  hash) {
            if (updateReplacement) rp->update(id, req);
            return id;
        }
    }
    return -1;
}

int32_t ApproximateDedupHashArray::preinsert(uint64_t hash, const MemReq* req) {
    uint32_t set = hf->hash(0, hash) & setMask;
    uint32_t first = set*assoc;

    // uint32_t candidate = rp->rankCands(req, SetAssocCands(first, first+assoc));
    for (uint32_t i = first; i < first+assoc; i++) {
        if (dataPointerArray[i] == -1) {
            return i;
        }
    }
    for (uint32_t i = first; i < first+assoc; i++) {
        if (dataArray->readCounter(dataPointerArray[i]) <= 1) {
            return i;
        }
    }

    return -1;
}

void ApproximateDedupHashArray::postinsert(uint64_t hash, const MemReq* req, int32_t dataPointer, int32_t hashId, bool updateReplacement) {
    rp->replaced(hashId);
    hashArray[hashId] = hash;
    dataPointerArray[hashId] = dataPointer;
    rp->update(hashId, req);
}

int32_t ApproximateDedupHashArray::readDataPointer(int32_t hashId) {
    return dataPointerArray[hashId];
}

void ApproximateDedupHashArray::approximate(const DataLine data, DataType type) {
    if (type == ZSIM_FLOAT) {
        for (uint16_t i = 0; i < zinfo->lineSize/4; i++)
        {
            ((uint32_t*) data)[i] = ((uint32_t*) data)[i] >> zinfo->floatCutSize;
            // ((uint32_t*) data)[i] = ((uint32_t*) data)[i] << zinfo->floatCutSize;
        }
    } else if (type == ZSIM_DOUBLE) {
        for (uint16_t i = 0; i < zinfo->lineSize/8; i++)
        {
            ((uint64_t*) data)[i] = ((uint64_t*) data)[i] >> zinfo->doubleCutSize;
            // ((uint64_t*) data)[i] = ((uint64_t*) data)[i] << zinfo->doubleCutSize;
        }
    } else {
        panic("We only approximate floats and doubles");
    }
}

uint64_t ApproximateDedupHashArray::hash(const DataLine data)
{
    uint8_t _0;
    uint8_t _1;
    uint8_t _2;
    uint8_t _3;
    uint8_t _4;
    uint8_t _5;
    uint8_t _6;
    uint8_t _7;
    uint8_t* dataLine = (uint8_t*) data;

    uint64_t XORs =0;
    uint8_t xorNeeded = zinfo->lineSize/8;

    if ((xorNeeded == 2) || (xorNeeded == 4) || (xorNeeded == 8)) {
        for (int i=0; i < xorNeeded; i++) {
            _0 =  dataLine[0+8*i];
            _1 =  dataLine[1+8*i];
            _2 =  dataLine[2+8*i];
            _3 =  dataLine[3+8*i];
            _4 =  dataLine[4+8*i];
            _5 =  dataLine[5+8*i];
            _6 =  dataLine[6+8*i];
            _7 =  dataLine[7+8*i];

            uint64_t step_64B = ((uint64_t) _0) + (((uint64_t) _1) << 8)  + (((uint64_t) _2) << 16) + (((uint64_t) _3) << 24)
                            + (((uint64_t) _4) << 32) + (((uint64_t) _5) << 40) + (((uint64_t) _6) << 48) + (((uint64_t) _7) << 56);

            XORs = XORs ^ dataHash->hash(0, step_64B);
        }
    } else {
        panic("not implemented yet for lines other than 16B/32B/64B");
    }

    return XORs & ((uint64_t)std::pow(2, (zinfo->hashSize))-1);
}

uint32_t ApproximateDedupHashArray::countValidLines() {
    uint32_t count = 0;
    for (uint32_t i = 0; i < this->numLines; i++) {
        if (dataPointerArray[i] != -1)
            count++;
    }
    return count;
}

void ApproximateDedupHashArray::print() {
    for (uint32_t i = 0; i < this->numLines; i++) {
        if (dataPointerArray[i] != -1)
            info("%i: %lu, %i", i, hashArray[i], dataPointerArray[i]);
    }
}
// Dedup end

// Dedup BDI Begin
ApproximateDedupBDITagArray::ApproximateDedupBDITagArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf) : rp(_rp), hf(_hf), numLines(_numLines), assoc(_assoc)  {
    tagArray = gm_calloc<Address>(numLines);
    segmentPointerArray = gm_calloc<int32_t>(numLines);
    prevPointerArray = gm_calloc<int32_t>(numLines);
    nextPointerArray = gm_calloc<int32_t>(numLines);
    dataPointerArray = gm_calloc<int32_t>(numLines);
    compressionEncodingArray = gm_calloc<BDICompressionEncoding>(numLines);
    for (uint32_t i = 0; i < numLines; i++) {
        prevPointerArray[i] = -1;
        nextPointerArray[i] = -1;
        dataPointerArray[i] = -1;
        segmentPointerArray[i] = -1;
        compressionEncodingArray[i] = NONE;
    }
    numSets = numLines/assoc;
    setMask = numSets - 1;
    validLines = 0;
    dataValidSegments = 0;
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

ApproximateDedupBDITagArray::~ApproximateDedupBDITagArray() {
    gm_free(tagArray);
    gm_free(segmentPointerArray);
    gm_free(prevPointerArray);
    gm_free(nextPointerArray);
    gm_free(dataPointerArray);
    gm_free(compressionEncodingArray);
}

int32_t ApproximateDedupBDITagArray::lookup(Address lineAddr, const MemReq* req, bool updateReplacement) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;
    for (uint32_t id = first; id < first + assoc; id++) {
        if (tagArray[id] ==  lineAddr) {
            if (updateReplacement) rp->update(id, req);
            return id;
        }
    }
    return -1;
}

int32_t ApproximateDedupBDITagArray::preinsert(Address lineAddr, const MemReq* req, Address* wbLineAddr) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;

    uint32_t candidate = rp->rankCands(req, SetAssocCands(first, first+assoc));

    *wbLineAddr = tagArray[candidate];
    return candidate;
}

bool ApproximateDedupBDITagArray::evictAssociatedData(int32_t lineId, int32_t* newLLHead) {
    *newLLHead = -1;
    if (dataPointerArray[lineId] == -1)
        return false;
    // if (!approximateArray[lineId])
    //     return true;
    // *approximate = true;
    if (prevPointerArray[lineId] != -1)
        return false;
    else
        *newLLHead = nextPointerArray[lineId];
    if (nextPointerArray[lineId] != -1)
        return false;
    return true;
}

void ApproximateDedupBDITagArray::postinsert(Address lineAddr, const MemReq* req, int32_t tagId, int32_t dataId, int32_t segmentId, BDICompressionEncoding encoding, int32_t listHead, bool updateReplacement, bool replace) {
    // info("Tag was %i: %lu, %i, %i, %i, %i, %i", tagId, tagArray[tagId] << lineBits, prevPointerArray[tagId], nextPointerArray[tagId], dataPointerArray[tagId], segmentPointerArray[tagId], BDICompressionToSize(compressionEncodingArray[tagId], zinfo->lineSize));
    // if (prevPointerArray[tagId] != -1)
    //     info("Tag was %i: %lu, %i, %i, %i, %i, %i", prevPointerArray[tagId], tagArray[prevPointerArray[tagId]] << lineBits, prevPointerArray[prevPointerArray[tagId]], nextPointerArray[prevPointerArray[tagId]], dataPointerArray[prevPointerArray[tagId]], segmentPointerArray[prevPointerArray[tagId]], BDICompressionToSize(compressionEncodingArray[prevPointerArray[tagId]], zinfo->lineSize));
    // if (nextPointerArray[tagId] != -1)
    //     info("Tag was %i: %lu, %i, %i, %i, %i, %i", nextPointerArray[tagId], tagArray[nextPointerArray[tagId]] << lineBits, prevPointerArray[nextPointerArray[tagId]], nextPointerArray[nextPointerArray[tagId]], dataPointerArray[nextPointerArray[tagId]], segmentPointerArray[nextPointerArray[tagId]], BDICompressionToSize(compressionEncodingArray[nextPointerArray[tagId]], zinfo->lineSize));
    if (!tagArray[tagId] && lineAddr) {
        if (listHead == -1) {
            dataValidSegments+=BDICompressionToSize(encoding, zinfo->lineSize)/8;
            // info("UP");
        }
        validLines++;
    } else if (tagArray[tagId] && !lineAddr) {
        validLines--;
        if (nextPointerArray[tagId] == -1 && prevPointerArray[tagId] == -1) {
            dataValidSegments-=BDICompressionToSize(compressionEncodingArray[tagId], zinfo->lineSize)/8;
            assert(dataValidSegments);
            // info("DOWN");
        }
    } else if (tagArray[tagId] && (nextPointerArray[tagId] > -1 || prevPointerArray[tagId] > -1) && lineAddr && listHead == -1) {
        dataValidSegments+=BDICompressionToSize(encoding, zinfo->lineSize)/8;
    }
    if(replace) rp->replaced(tagId);
    tagArray[tagId] = lineAddr;
    dataPointerArray[tagId] = dataId;
    segmentPointerArray[tagId] = segmentId;
    compressionEncodingArray[tagId] = encoding;
    if (prevPointerArray[tagId] != -1)
        nextPointerArray[prevPointerArray[tagId]] = nextPointerArray[tagId];
    if (nextPointerArray[tagId] != -1)
        prevPointerArray[nextPointerArray[tagId]] = prevPointerArray[tagId];
    prevPointerArray[tagId] = -1;
    nextPointerArray[tagId] = listHead;
    if (listHead >= 0) {
        if(prevPointerArray[listHead] == -1) prevPointerArray[listHead] = tagId;
        else panic("List head is not actually a list head!");
    }
    if(updateReplacement) rp->update(tagId, req);
    // info("Tag is %i: %lu, %i, %i, %i, %i, %i", tagId, tagArray[tagId] << lineBits, prevPointerArray[tagId], nextPointerArray[tagId], dataPointerArray[tagId], segmentPointerArray[tagId], BDICompressionToSize(compressionEncodingArray[tagId], zinfo->lineSize));
    // if (prevPointerArray[tagId] != -1)
    //     info("Tag is %i: %lu, %i, %i, %i, %i, %i", prevPointerArray[tagId], tagArray[prevPointerArray[tagId]] << lineBits, prevPointerArray[prevPointerArray[tagId]], nextPointerArray[prevPointerArray[tagId]], dataPointerArray[prevPointerArray[tagId]], segmentPointerArray[prevPointerArray[tagId]], BDICompressionToSize(compressionEncodingArray[prevPointerArray[tagId]], zinfo->lineSize));
    // if (nextPointerArray[tagId] != -1)
    //     info("Tag is %i: %lu, %i, %i, %i, %i, %i", nextPointerArray[tagId], tagArray[nextPointerArray[tagId]] << lineBits, prevPointerArray[nextPointerArray[tagId]], nextPointerArray[nextPointerArray[tagId]], dataPointerArray[nextPointerArray[tagId]], segmentPointerArray[nextPointerArray[tagId]], BDICompressionToSize(compressionEncodingArray[nextPointerArray[tagId]], zinfo->lineSize));
}

void ApproximateDedupBDITagArray::changeInPlace(Address lineAddr, const MemReq* req, int32_t tagId, int32_t dataId, int32_t segmentId, BDICompressionEncoding encoding, int32_t listHead, bool updateReplacement) {
    if (!tagArray[tagId] && lineAddr) {
        if (listHead == -1) {
            dataValidSegments+=BDICompressionToSize(encoding, zinfo->lineSize)/8;
        }
        validLines++;
    } else if (tagArray[tagId] && !lineAddr) {
        validLines--;
        assert(validLines);
        if (nextPointerArray[tagId] == -1 && prevPointerArray[tagId] == -1) {
            dataValidSegments-=BDICompressionToSize(compressionEncodingArray[tagId], zinfo->lineSize)/8;
        }
    } else if (nextPointerArray[tagId] == -1 && prevPointerArray[tagId] == -1) {
        dataValidSegments-=BDICompressionToSize(compressionEncodingArray[tagId], zinfo->lineSize)/8;
        dataValidSegments+=BDICompressionToSize(encoding, zinfo->lineSize)/8;
    }
    tagArray[tagId] = lineAddr;
    dataPointerArray[tagId] = dataId;
    segmentPointerArray[tagId] = segmentId;
    compressionEncodingArray[tagId] = encoding;
    if (prevPointerArray[tagId] != -1)
        nextPointerArray[prevPointerArray[tagId]] = nextPointerArray[tagId];
    if (nextPointerArray[tagId] != -1)
        prevPointerArray[nextPointerArray[tagId]] = prevPointerArray[tagId];
    prevPointerArray[tagId] = -1;
    nextPointerArray[tagId] = listHead;
    if (listHead >= 0) {
        if(prevPointerArray[listHead] == -1) prevPointerArray[listHead] = tagId;
        else panic("List head is not actually a list head!");
    }
    if(updateReplacement) rp->update(tagId, req);
}

BDICompressionEncoding ApproximateDedupBDITagArray::readCompressionEncoding(int32_t tagId) {
    return compressionEncodingArray[tagId];
}

void ApproximateDedupBDITagArray::writeCompressionEncoding(int32_t tagId, BDICompressionEncoding encoding) {
    // info("Tag was %i: %lu, %i, %i, %i, %i, %i", tagId, tagArray[tagId] << lineBits, prevPointerArray[tagId], nextPointerArray[tagId], dataPointerArray[tagId], segmentPointerArray[tagId], BDICompressionToSize(compressionEncodingArray[tagId], zinfo->lineSize));
    assert (nextPointerArray[tagId] == -1 && prevPointerArray[tagId] == -1);
    // info("CHANGE");
    dataValidSegments-=BDICompressionToSize(compressionEncodingArray[tagId], zinfo->lineSize)/8;
    dataValidSegments+=BDICompressionToSize(encoding, zinfo->lineSize)/8;
    compressionEncodingArray[tagId] = encoding;
    // info("Tag is %i: %lu, %i, %i, %i, %i, %i", tagId, tagArray[tagId] << lineBits, prevPointerArray[tagId], nextPointerArray[tagId], dataPointerArray[tagId], segmentPointerArray[tagId], BDICompressionToSize(compressionEncodingArray[tagId], zinfo->lineSize));
}

int32_t ApproximateDedupBDITagArray::readDataId(int32_t tagId) {
    // assert_msg(!approximate[tagId], "must be exact to read dataId");
    return dataPointerArray[tagId];
}

int32_t ApproximateDedupBDITagArray::readSegmentPointer(int32_t tagId) {
    // assert_msg(!approximate[tagId], "must be exact to read dataId");
    return segmentPointerArray[tagId];
}

Address ApproximateDedupBDITagArray::readAddress(int32_t tagId) {
    return tagArray[tagId];
}

int32_t ApproximateDedupBDITagArray::readNextLL(int32_t tagId) {
    return nextPointerArray[tagId];
}

int32_t ApproximateDedupBDITagArray::readPrevLL(int32_t tagId) {
    return prevPointerArray[tagId];
}

uint32_t ApproximateDedupBDITagArray::getValidLines() {
    return validLines;
}

uint32_t ApproximateDedupBDITagArray::countValidLines() {
    uint32_t Counter = 0;
    for (uint32_t i = 0; i < numLines; i++) {
        if (dataPointerArray[i] != -1)
            Counter++;
    }
    return Counter;
}

uint32_t ApproximateDedupBDITagArray::getDataValidSegments() {
    return dataValidSegments;
}

void ApproximateDedupBDITagArray::print() {
    for (uint32_t i = 0; i < this->numLines; i++) {
        if (dataPointerArray[i] != -1)
            info("%i: %lu, %i, %i, %i, %i", i, tagArray[i] << lineBits, prevPointerArray[i], nextPointerArray[i], dataPointerArray[i], segmentPointerArray[i]);
    }
}

ApproximateDedupBDIDataArray::ApproximateDedupBDIDataArray(uint32_t _numLines, uint32_t _assoc, HashFamily* _hf) : hf(_hf), numLines(_numLines), assoc(_assoc)  {
    numSets = numLines/assoc;
    tagCounterArray = gm_calloc<int32_t*>(numSets);
    tagPointerArray = gm_malloc<int32_t*>(numSets);
    // approximateArray = gm_calloc<bool>(numLines);
    compressedDataArray = gm_malloc<DataLine*>(numSets);
    rp = gm_calloc<DataLRUReplPolicy*>(numSets);
    // notice that you will always need to access freeList by [size-1]
    g_vector<g_vector<int32_t>> tmp(8);
    freeList = tmp;
    for (uint32_t i = 0; i < numSets; i++) {
        tagCounterArray[i] = gm_calloc<int32_t>(assoc*zinfo->lineSize/8);
        tagPointerArray[i] = gm_calloc<int32_t>(assoc*zinfo->lineSize/8);
        compressedDataArray[i] = gm_calloc<DataLine>(assoc*zinfo->lineSize/8);
        rp[i] = new DataLRUReplPolicy(assoc*zinfo->lineSize/8);
        freeList[7].push_back(i);
        for (uint32_t j = 0; j < assoc*zinfo->lineSize/8; j++) {
            tagPointerArray[i][j] = -1;
            compressedDataArray[i][j] = gm_calloc<uint8_t>(zinfo->lineSize);
        }
    }
    setMask = numSets - 1;
    validLines = 0;
    // srand (time(NULL));
    std::random_device rd;
    RNG = new std::mt19937(rd());
    DIST = new std::uniform_int_distribution<>(0, numSets-1);
    popped = false;

    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

ApproximateDedupBDIDataArray::~ApproximateDedupBDIDataArray() {
    for (uint32_t i = 0; i < numSets; i++) {
        for (uint32_t j = 0; j < assoc*zinfo->lineSize/8; j++) {
            gm_free(compressedDataArray[i][j]);
        }
        gm_free(tagCounterArray[i]);
        gm_free(tagPointerArray[i]);
        gm_free(compressedDataArray[i]);
    }
    gm_free(tagCounterArray);
    gm_free(tagPointerArray);
    gm_free(compressedDataArray);
}

void ApproximateDedupBDIDataArray::lookup(int32_t dataId, int32_t segmentId, const MemReq* req, bool updateReplacement) {
    if (updateReplacement) rp[dataId]->update(segmentId, req);
}

void ApproximateDedupBDIDataArray::assignTagArray(ApproximateDedupBDITagArray* _tagArray) {
        tagArray = _tagArray;
}

int32_t ApproximateDedupBDIDataArray::preinsert(uint16_t lineSize) {
    float leastValue = 999999;
    int32_t leastId = 0;
    for (uint32_t i = (lineSize/8)-1; i < 8; i++) {
        if (freeList[i].size()) {
            leastId = freeList[i].back();
            freeList[i].pop_back();
            popped = true;
            return leastId;
        }
    }
    int32_t zeroFound = 1;
    for (uint32_t i = 0; i < zinfo->randomLoopTrial; i++) {
        int32_t id = DIST->operator()(*RNG);
        int32_t counts = 0;
        int32_t sizes = 0;
        for (uint32_t j = 0; j < assoc*zinfo->lineSize/8; j++) {
            counts += tagCounterArray[id][j];
            if (tagCounterArray[id][j])
                sizes += BDICompressionToSize(tagArray->readCompressionEncoding(tagPointerArray[id][j]), zinfo->lineSize);
        }
        if (counts == 0)
            panic("Cannot happen");
        if ((assoc*zinfo->lineSize - sizes) >= lineSize)
            return id;
        g_vector<uint32_t> keptFromEvictions;
        counts = 0;
        do {
            int32_t candidate = rp[id]->rank(NULL, SetAssocCands(0, (assoc*zinfo->lineSize/8)), keptFromEvictions);
            if (tagCounterArray[id][candidate])
                sizes -= BDICompressionToSize(tagArray->readCompressionEncoding(tagPointerArray[id][candidate]), zinfo->lineSize);
            counts += tagCounterArray[id][candidate];
            keptFromEvictions.push_back(candidate);
        } while((assoc*zinfo->lineSize-sizes) < lineSize);
        if (counts <= leastValue) {
            leastId = id;
            leastValue = counts;
        }
    }
    if (zeroFound)
        leastId = DIST->operator()(*RNG);
    return leastId;
}

int32_t ApproximateDedupBDIDataArray::preinsert(int32_t dataId, int32_t* tagId, g_vector<uint32_t>& exceptions) {
    int32_t candidate = 0;
    for (uint32_t j = 0; j < assoc*zinfo->lineSize/8; j++) {
        bool Found = false;
        for (uint32_t i = 0; i < exceptions.size(); i++)
            if (j == exceptions[i]) {
                Found = true;
                break;
            }
        if (Found)
            continue;
        candidate = rp[dataId]->rank(NULL, SetAssocCands(0, (assoc*zinfo->lineSize/8)), exceptions);
        break;
    }
    *tagId = tagPointerArray[dataId][candidate];
    return candidate;
}

void ApproximateDedupBDIDataArray::postinsert(int32_t tagId, const MemReq* req, int32_t counter, int32_t dataId, int32_t segmentId, DataLine data, bool updateReplacement) {
    rp[dataId]->replaced(segmentId);

    if (!popped) {
        for (uint32_t i = 0; i < 8; i++) {
            auto it = std::find(freeList[i].begin(), freeList[i].end(), dataId);
            if(it != freeList[i].end()) {
                auto index = std::distance(freeList[i].begin(), it);
                freeList[i].erase(freeList[i].begin() + index);
                break;
            }
        }
    }

    tagCounterArray[dataId][segmentId] = counter;
    if (tagPointerArray[dataId][segmentId] == -1 && tagId != -1) {
        validLines++;
    } else if (tagPointerArray[dataId][segmentId] != -1 && tagId == -1) {
        validLines--;
    }
    tagPointerArray[dataId][segmentId] = tagId;
    if (data)
        PIN_SafeCopy(compressedDataArray[dataId][segmentId], data, zinfo->lineSize);
    if (updateReplacement) rp[dataId]->update(segmentId, req);

    int count = assoc*zinfo->lineSize/8;
    for (uint32_t i = 0; i < assoc*zinfo->lineSize/8; i++)
        if (tagPointerArray[dataId][i] != -1)
            count -= BDICompressionToSize(tagArray->readCompressionEncoding(tagPointerArray[dataId][i]), zinfo->lineSize)/8;
    if (count > 8)
        count = 8;
    if (count)
        freeList[count-1].push_back(dataId);
    popped = false;
    // info("Data was %i,%i: %i, %i", dataId, segmentId, tagCounterArray[dataId][segmentId], tagPointerArray[dataId][segmentId]);
    // info("Data is %i,%i: %i, %i", dataId, segmentId, counter, tagId);
}

void ApproximateDedupBDIDataArray::changeInPlace(int32_t tagId, const MemReq* req, int32_t counter, int32_t dataId, int32_t segmentId, DataLine data, bool updateReplacement) {
    tagCounterArray[dataId][segmentId] = counter;
    tagPointerArray[dataId][segmentId] = tagId;
    if (data)
        PIN_SafeCopy(compressedDataArray[dataId][segmentId], data, zinfo->lineSize);
    if (updateReplacement) rp[dataId]->update(segmentId, req);
    // info("Data was %i,%i: %i, %i", dataId, segmentId, tagCounterArray[dataId][segmentId], tagPointerArray[dataId][segmentId]);
    // info("Data is %i,%i: %i, %i", dataId, segmentId, counter, tagId);
}

bool ApproximateDedupBDIDataArray::isSame(int32_t dataId, int32_t segmentId, DataLine data) {
    for (uint32_t i = 0; i < zinfo->lineSize/8; i++)
        if (((uint64_t*)data)[i] != ((uint64_t*)compressedDataArray[dataId][segmentId])[i])
            return false;
    return true;
}

int32_t ApproximateDedupBDIDataArray::readListHead(int32_t dataId, int32_t segmentId) {
    return tagPointerArray[dataId][segmentId];
}

int32_t ApproximateDedupBDIDataArray::readCounter(int32_t dataId, int32_t segmentId) {
    return tagCounterArray[dataId][segmentId];
}

DataLine ApproximateDedupBDIDataArray::readData(int32_t dataId, int32_t segmentId) {
    return compressedDataArray[dataId][segmentId];
}

void ApproximateDedupBDIDataArray::writeData(int32_t dataId, int32_t segmentId, DataLine data, const MemReq* req, bool updateReplacement) {
    PIN_SafeCopy(compressedDataArray[dataId][segmentId], data, zinfo->lineSize);
    if (updateReplacement) rp[dataId]->update(segmentId, req);
}

uint32_t ApproximateDedupBDIDataArray::getValidLines() {
    return validLines;
}

void ApproximateDedupBDIDataArray::print() {
    for (uint32_t i = 0; i < numSets; i++) {
        for (uint32_t j = 0; j < assoc*zinfo->lineSize/8; j++) {
            if (tagPointerArray[i][j] != -1)
                info("%i,%i: %i, %i", i, j, tagCounterArray[i][j], tagPointerArray[i][j]);
        }
    }
}

ApproximateDedupBDIHashArray::ApproximateDedupBDIHashArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf, H3HashFamily* _dataHash) : rp(_rp), hf(_hf), dataHash(_dataHash), numLines(_numLines), assoc(_assoc)  {
    hashArray = gm_malloc<uint64_t>(numLines);
    dataPointerArray = gm_malloc<int32_t>(numLines);
    segmentPointerArray = gm_malloc<int32_t>(numLines);
    for (uint32_t i = 0; i < numLines; i++) {
        hashArray[i] = -1;
        dataPointerArray[i] = -1;
        segmentPointerArray[i] = -1;
    }
    numSets = numLines/assoc;
    setMask = numSets - 1;
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

ApproximateDedupBDIHashArray::~ApproximateDedupBDIHashArray() {
    gm_free(hashArray);
    gm_free(dataPointerArray);
    gm_free(segmentPointerArray);
}

void ApproximateDedupBDIHashArray::registerDataArray(ApproximateDedupBDIDataArray* dataArray) {
    this->dataArray = dataArray;
}

int32_t ApproximateDedupBDIHashArray::lookup(uint64_t hash, const MemReq* req, bool updateReplacement) {
    uint32_t set = hf->hash(0, hash) & setMask;
    uint32_t first = set*assoc;
    for (uint32_t id = first; id < first + assoc; id++) {
        if (hashArray[id] ==  hash) {
            if (updateReplacement) rp->update(id, req);
            return id;
        }
    }
    return -1;
}

int32_t ApproximateDedupBDIHashArray::preinsert(uint64_t hash, const MemReq* req) {
    uint32_t set = hf->hash(0, hash) & setMask;
    uint32_t first = set*assoc;

    // uint32_t candidate = rp->rankCands(req, SetAssocCands(first, first+assoc));

    for (uint32_t i = first; i < first+assoc; i++) {
        if (dataPointerArray[i] == -1) {
            return i;
        }
    }
    for (uint32_t i = first; i < first+assoc; i++) {
        if (dataArray->readCounter(dataPointerArray[i], segmentPointerArray[i]) <= 1) {
            return i;
        }
    }

    return -1;
}

void ApproximateDedupBDIHashArray::postinsert(uint64_t hash, const MemReq* req, int32_t dataPointer, int32_t segmentPointer, int32_t hashId, bool updateReplacement) {
    rp->replaced(hashId);
    hashArray[hashId] = hash;
    dataPointerArray[hashId] = dataPointer;
    segmentPointerArray[hashId] = segmentPointer;
    rp->update(hashId, req);
}

void ApproximateDedupBDIHashArray::changeInPlace(uint64_t hash, const MemReq* req, int32_t dataPointer, int32_t segmentPointer, int32_t hashId, bool updateReplacement) {
    hashArray[hashId] = hash;
    dataPointerArray[hashId] = dataPointer;
    segmentPointerArray[hashId] = segmentPointer;
    rp->update(hashId, req);
}

int32_t ApproximateDedupBDIHashArray::readDataPointer(int32_t hashId) {
    return dataPointerArray[hashId];
}

int32_t ApproximateDedupBDIHashArray::readSegmentPointer(int32_t hashId) {
    return segmentPointerArray[hashId];
}

void ApproximateDedupBDIHashArray::approximate(const DataLine data, DataType type) {
    if (type == ZSIM_FLOAT) {
        for (uint16_t i = 0; i < zinfo->lineSize/4; i++)
        {
            ((uint32_t*) data)[i] = ((uint32_t*) data)[i] >> zinfo->floatCutSize;
            // ((uint32_t*) data)[i] = ((uint32_t*) data)[i] << zinfo->floatCutSize;
        }
    } else if (type == ZSIM_DOUBLE) {
        for (uint16_t i = 0; i < zinfo->lineSize/8; i++)
        {
            ((uint64_t*) data)[i] = ((uint64_t*) data)[i] >> zinfo->doubleCutSize;
            // ((uint64_t*) data)[i] = ((uint64_t*) data)[i] << zinfo->doubleCutSize;
        }
    } else {
        panic("We only approximate floats and doubles");
    }
}

uint64_t ApproximateDedupBDIHashArray::hash(const DataLine data)
{
    uint8_t _0;
    uint8_t _1;
    uint8_t _2;
    uint8_t _3;
    uint8_t _4;
    uint8_t _5;
    uint8_t _6;
    uint8_t _7;
    uint8_t* dataLine = (uint8_t*) data;

    uint64_t XORs =0;
    uint8_t xorNeeded = zinfo->lineSize/8;

    if ((xorNeeded == 2) || (xorNeeded == 4) || (xorNeeded == 8)) {
        for (int i=0; i < xorNeeded; i++) {
            _0 =  dataLine[0+8*i];
            _1 =  dataLine[1+8*i];
            _2 =  dataLine[2+8*i];
            _3 =  dataLine[3+8*i];
            _4 =  dataLine[4+8*i];
            _5 =  dataLine[5+8*i];
            _6 =  dataLine[6+8*i];
            _7 =  dataLine[7+8*i];

            uint64_t step_64B = ((uint64_t) _0) + (((uint64_t) _1) << 8)  + (((uint64_t) _2) << 16) + (((uint64_t) _3) << 24)
                            + (((uint64_t) _4) << 32) + (((uint64_t) _5) << 40) + (((uint64_t) _6) << 48) + (((uint64_t) _7) << 56);

            XORs = XORs ^ dataHash->hash(0, step_64B);
        }
    } else {
        panic("not implemented yet for lines other than 16B/32B/64B");
    }

    return XORs & ((uint64_t)std::pow(2, (zinfo->hashSize))-1);
}

uint32_t ApproximateDedupBDIHashArray::countValidLines() {
    uint32_t count = 0;
    for (uint32_t i = 0; i < this->numLines; i++) {
        if (dataPointerArray[i] != -1)
            count++;
    }
    return count;
}

void ApproximateDedupBDIHashArray::print() {
    for (uint32_t i = 0; i < this->numLines; i++) {
        if (dataPointerArray[i] != -1)
            info("%i: %lu, %i", i, hashArray[i], dataPointerArray[i]);
    }
}
// BDI and ApproximateBDI End

ApproximateNaiiveDedupBDIDataArray::ApproximateNaiiveDedupBDIDataArray(uint32_t _numLines, uint32_t _assoc, HashFamily* _hf) : ApproximateDedupBDIDataArray(_numLines, _assoc, _hf) {
    for (uint32_t i = 0; i < numSets; i++) {
        freeList.push_back(i);
    }
}

ApproximateNaiiveDedupBDIDataArray::~ApproximateNaiiveDedupBDIDataArray() {
    for (uint32_t i = 0; i < numSets; i++) {
        for (uint32_t j = 0; j < assoc*zinfo->lineSize/8; j++) {
            gm_free(compressedDataArray[i][j]);
        }
        gm_free(tagCounterArray[i]);
        gm_free(tagPointerArray[i]);
        gm_free(compressedDataArray[i]);
    }
    gm_free(tagCounterArray);
    gm_free(tagPointerArray);
    gm_free(compressedDataArray);
}

int32_t ApproximateNaiiveDedupBDIDataArray::preinsert(uint16_t lineSize) {
    float leastValue = 999999;
    int32_t leastId = 0;
    if (freeList.size()) {
        leastId = freeList.back();
        freeList.pop_back();
        return leastId;
    }
    for (uint32_t i = 0; i < zinfo->randomLoopTrial; i++) {
        int32_t id = DIST->operator()(*RNG);
        int32_t counts = 0;
        for (uint32_t j = 0; j < assoc*zinfo->lineSize/8; j++) {
            counts += tagCounterArray[id][j];
        }
        if (counts == 0)
            panic("Cannot happen");
        if (counts <= leastValue) {
            leastId = id;
            leastValue = counts;
        }
    }
    return leastId;
}

int32_t ApproximateNaiiveDedupBDIDataArray::preinsert(int32_t dataId, int32_t* tagId, g_vector<uint32_t>& exceptions) {
    int32_t candidate = 0;
    for (uint32_t j = 0; j < assoc*zinfo->lineSize/8; j++) {
        bool Found = false;
        for (uint32_t i = 0; i < exceptions.size(); i++)
            if (j == exceptions[i]) {
                Found = true;
                break;
            }
        if (Found)
            continue;
        candidate = rp[dataId]->rank(NULL, SetAssocCands(0, (assoc*zinfo->lineSize/8)), exceptions);
        break;
    }
    *tagId = tagPointerArray[dataId][candidate];
    return candidate;
}

void ApproximateNaiiveDedupBDIDataArray::postinsert(int32_t tagId, const MemReq* req, int32_t counter, int32_t dataId, int32_t segmentId, DataLine data, bool updateReplacement) {
    rp[dataId]->replaced(segmentId);

    auto it = std::find(freeList.begin(), freeList.end(), dataId);
    if(it != freeList.end()) {
        auto index = std::distance(freeList.begin(), it);
        freeList.erase(freeList.begin() + index);
    }

    tagCounterArray[dataId][segmentId] = counter;
    if (tagPointerArray[dataId][segmentId] == -1 && tagId != -1) {
        validLines++;
    } else if (tagPointerArray[dataId][segmentId] != -1 && tagId == -1) {
        validLines--;
    }
    tagPointerArray[dataId][segmentId] = tagId;
    if (data)
        PIN_SafeCopy(compressedDataArray[dataId][segmentId], data, zinfo->lineSize);
    if (updateReplacement) rp[dataId]->update(segmentId, req);

    int count = 0;
    for (uint32_t i = 0; i < assoc*zinfo->lineSize/8; i++)
        if (tagPointerArray[dataId][i] != -1)
            count += BDICompressionToSize(tagArray->readCompressionEncoding(tagPointerArray[dataId][i]), zinfo->lineSize)/8;
    if (!count)
        freeList.push_back(dataId);
    // info("Data was %i,%i: %i, %i", dataId, segmentId, tagCounterArray[dataId][segmentId], tagPointerArray[dataId][segmentId]);
    // info("Data is %i,%i: %i, %i", dataId, segmentId, counter, tagId);
}

// uniDoppelganger BDI Start
uniDoppelgangerBDITagArray::uniDoppelgangerBDITagArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf) : rp(_rp), hf(_hf), numLines(_numLines), assoc(_assoc)  {
    tagArray = gm_calloc<Address>(numLines);
    prevPointerArray = gm_calloc<int32_t>(numLines);
    nextPointerArray = gm_calloc<int32_t>(numLines);
    mapPointerArray = gm_calloc<int32_t>(numLines);
    segmentPointerArray = gm_calloc<int32_t>(numLines);
    approximateArray = gm_calloc<bool>(numLines);
    for (uint32_t i = 0; i < numLines; i++) {
        prevPointerArray[i] = -1;
        nextPointerArray[i] = -1;
        mapPointerArray[i] = -1;
        segmentPointerArray[i] = -1;
    }
    numSets = numLines/assoc;
    setMask = numSets - 1;
    validLines = 0;
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

uniDoppelgangerBDITagArray::~uniDoppelgangerBDITagArray() {
    gm_free(tagArray);
    gm_free(prevPointerArray);
    gm_free(nextPointerArray);
    gm_free(mapPointerArray);
    gm_free(segmentPointerArray);
    gm_free(approximateArray);
}

int32_t uniDoppelgangerBDITagArray::lookup(Address lineAddr, const MemReq* req, bool updateReplacement) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;
    for (uint32_t id = first; id < first + assoc; id++) {
        if (tagArray[id] ==  lineAddr) {
            if (updateReplacement) rp->update(id, req);
            return id;
        }
    }
    return -1;
}

int32_t uniDoppelgangerBDITagArray::preinsert(Address lineAddr, const MemReq* req, Address* wbLineAddr) {
    uint32_t set = hf->hash(0, lineAddr) & setMask;
    uint32_t first = set*assoc;

    uint32_t candidate = rp->rankCands(req, SetAssocCands(first, first+assoc));

    *wbLineAddr = tagArray[candidate];
    return candidate;
}

bool uniDoppelgangerBDITagArray::evictAssociatedData(int32_t lineId, int32_t* newLLHead, bool* approximate) {
    *newLLHead = -1;
    if (mapPointerArray[lineId] == -1)
        return false;
    if (!approximateArray[lineId])
        return true;
    *approximate = true;
    if (prevPointerArray[lineId] != -1)
        return false;
    else
        *newLLHead = nextPointerArray[lineId];
    if (nextPointerArray[lineId] != -1)
        return false;
    return true;
}

void uniDoppelgangerBDITagArray::postinsert(Address lineAddr, const MemReq* req, int32_t tagId, int32_t mapId, int32_t segmentId, int32_t listHead, bool approximate, bool updateReplacement) {
    if (!tagArray[tagId] && lineAddr) {
        validLines++;
    } else if (tagArray[tagId] && !lineAddr) {
        assert(validLines);
        validLines--;
    }
    rp->replaced(tagId);
    tagArray[tagId] = lineAddr;
    mapPointerArray[tagId] = mapId;
    segmentPointerArray[tagId] = segmentId;
    approximateArray[tagId] = approximate;
    if (prevPointerArray[tagId] != -1)
        nextPointerArray[prevPointerArray[tagId]] = nextPointerArray[tagId];
    if (nextPointerArray[tagId] != -1)
        prevPointerArray[nextPointerArray[tagId]] = prevPointerArray[tagId];
    prevPointerArray[tagId] = -1;
    nextPointerArray[tagId] = listHead;
    if (listHead >= 0) {
        if(prevPointerArray[listHead] == -1) prevPointerArray[listHead] = tagId;
        else panic("List head is not actually a list head!");
    }
    if(updateReplacement) rp->update(tagId, req);
}

void uniDoppelgangerBDITagArray::changeInPlace(Address lineAddr, const MemReq* req, int32_t tagId, int32_t mapId, int32_t segmentId, int32_t listHead, bool approximate, bool updateReplacement) {
    // if (!tagArray[tagId] && lineAddr) {
    //     validLines++;
    // } else if (tagArray[tagId] && !lineAddr) {
    //     validLines--;
    // }
    // rp->replaced(tagId);
    tagArray[tagId] = lineAddr;
    mapPointerArray[tagId] = mapId;
    segmentPointerArray[tagId] = segmentId;
    approximateArray[tagId] = approximate;
    if (prevPointerArray[tagId] != -1)
        nextPointerArray[prevPointerArray[tagId]] = nextPointerArray[tagId];
    if (nextPointerArray[tagId] != -1)
        prevPointerArray[nextPointerArray[tagId]] = prevPointerArray[tagId];
    prevPointerArray[tagId] = -1;
    nextPointerArray[tagId] = listHead;
    if (listHead >= 0) {
        if(prevPointerArray[listHead] == -1) prevPointerArray[listHead] = tagId;
        else panic("List head is not actually a list head!");
    }
    if(updateReplacement) rp->update(tagId, req);
}

int32_t uniDoppelgangerBDITagArray::readMapId(const int32_t tagId) {
    // assert_msg(approximate[tagId], "must be approximate to read mapId");
    return mapPointerArray[tagId];
}

int32_t uniDoppelgangerBDITagArray::readSegmentId(const int32_t tagId) {
    // assert_msg(approximate[tagId], "must be approximate to read mapId");
    return segmentPointerArray[tagId];
}

int32_t uniDoppelgangerBDITagArray::readDataId(const int32_t tagId) {
    // assert_msg(!approximate[tagId], "must be exact to read dataId");
    return mapPointerArray[tagId];
}

Address uniDoppelgangerBDITagArray::readAddress(int32_t tagId) {
    return tagArray[tagId];
}

int32_t uniDoppelgangerBDITagArray::readNextLL(int32_t tagId) {
    return nextPointerArray[tagId];
}

uint32_t uniDoppelgangerBDITagArray::getValidLines() {
    return validLines;
}

uint32_t uniDoppelgangerBDITagArray::countValidLines() {
    uint32_t Counter = 0;
    for (uint32_t i = 0; i < numLines; i++) {
        if (mapPointerArray[i] != -1)
            Counter++;
    }
    return Counter;
}

void uniDoppelgangerBDITagArray::print() {
    for (uint32_t i = 0; i < this->numLines; i++) {
        if (mapPointerArray[i] != -1)
            info("%i: %lu, %i, %i, %i, %s", i, tagArray[i] << lineBits, prevPointerArray[i], nextPointerArray[i], mapPointerArray[i], approximateArray[i]? "approximate":"exact");
    }
}

uniDoppelgangerBDIDataArray::uniDoppelgangerBDIDataArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf, uint32_t _tagRatio) : rp(_rp), hf(_hf), numLines(_numLines), assoc(_assoc), tagRatio(_tagRatio) {
    numSets = numLines/assoc;
    tagCounterArray = gm_calloc<int32_t*>(numSets);
    tagPointerArray = gm_malloc<int32_t*>(numSets);
    mtagArray = gm_malloc<int32_t*>(numSets);
    approximateArray = gm_calloc<bool*>(numLines);
    compressionEncodingArray = gm_malloc<BDICompressionEncoding*>(numLines);
    info("%i, %i", numSets, assoc);
    for (uint32_t i = 0; i < numSets; i++) {
        tagCounterArray[i] = gm_calloc<int32_t>(assoc);
        tagPointerArray[i] = gm_malloc<int32_t>(assoc);
        compressionEncodingArray[i] = gm_malloc<BDICompressionEncoding>(assoc);
        mtagArray[i] = gm_malloc<int32_t>(assoc);
        approximateArray[i] = gm_calloc<bool>(assoc);
        for (uint32_t j = 0; j < assoc; j++) {
            tagPointerArray[i][j] = -1;
            mtagArray[i][j] = -1;
            compressionEncodingArray[i][j] = NONE;
        }
    }
    setMask = numSets - 1;
    validSegments = 0;
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
}

uniDoppelgangerBDIDataArray::~uniDoppelgangerBDIDataArray() {
    for (uint32_t i = 0; i < numSets; i++) {
        gm_free(mtagArray[i]);
        gm_free(tagCounterArray[i]);
        gm_free(tagPointerArray[i]);
        gm_free(approximateArray[i]);
    }
    gm_free(mtagArray);
    gm_free(tagPointerArray);
    gm_free(tagCounterArray);
    gm_free(approximateArray);
}

int32_t uniDoppelgangerBDIDataArray::lookup(uint32_t map) {
    uint32_t set = hf->hash(0, map) & setMask;
    return set;
}

int32_t uniDoppelgangerBDIDataArray::lookup(uint32_t map, uint32_t set, const MemReq* req, bool updateReplacement) {
    for (uint32_t id = 0; id < assoc; id++) {
        if (mtagArray[set][id] == (int32_t)map && approximateArray[set][id] == true) {
            if (updateReplacement) rp->update(set*assoc+id, req);
            return id;
        }
    }
    return -1;
}

uint32_t uniDoppelgangerBDIDataArray::calculateMap(const DataLine data, DataType type, DataValue minValue, DataValue maxValue) {
    // Get hash and map values
    int64_t intAvgHash = 0, intRangeHash = 0;
    double floatAvgHash = 0, floatRangeHash = 0;
    int64_t intMax = std::numeric_limits<int64_t>::min(),
            intMin = std::numeric_limits<int64_t>::max(),
            intSum = 0;
    double floatMax = std::numeric_limits<double>::min(),
            floatMin = std::numeric_limits<double>::max(),
            floatSum = 0;
    double mapStep = 0;
    int32_t avgMap = 0, rangeMap = 0;
    uint32_t map = 0;
    switch (type)
    {
        case ZSIM_UINT8:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(uint8_t); i++) {
                intSum += ((uint8_t*) data)[i];
                if (((uint8_t*) data)[i] > intMax)
                    intMax = ((uint8_t*) data)[i];
                if (((uint8_t*) data)[i] < intMin)
                    intMin = ((uint8_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(uint8_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.UINT8)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.UINT8)
                panic("Received a value lower than the annotation's Min!!");
            if (zinfo->mapSize > sizeof(uint8_t)) {
                avgMap = intAvgHash;
                rangeMap = intRangeHash;
            } else {
                mapStep = (maxValue.UINT8 - minValue.UINT8)/std::pow(2,zinfo->mapSize-1);
                avgMap = intAvgHash/mapStep;
                rangeMap = intRangeHash/mapStep;
            }
            break;
        case ZSIM_INT8:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(int8_t); i++) {
                intSum += ((int8_t*) data)[i];
                if (((int8_t*) data)[i] > intMax)
                    intMax = ((int8_t*) data)[i];
                if (((int8_t*) data)[i] < intMin)
                    intMin = ((int8_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(int8_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.INT8)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.INT8)
                panic("Received a value lower than the annotation's Min!!");
            if (zinfo->mapSize > sizeof(int8_t)) {
                avgMap = intAvgHash;
                rangeMap = intRangeHash;
            } else {
                mapStep = (maxValue.INT8 - minValue.INT8)/std::pow(2,zinfo->mapSize-1);
                avgMap = intAvgHash/mapStep;
                rangeMap = intRangeHash/mapStep;
            }
            break;
        case ZSIM_UINT16:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(uint16_t); i++) {
                intSum += ((uint16_t*) data)[i];
                if (((uint16_t*) data)[i] > intMax)
                    intMax = ((uint16_t*) data)[i];
                if (((uint16_t*) data)[i] < intMin)
                    intMin = ((uint16_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(uint16_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.UINT16)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.UINT16)
                panic("Received a value lower than the annotation's Min!!");
            if (zinfo->mapSize > sizeof(uint16_t)) {
                avgMap = intAvgHash;
                rangeMap = intRangeHash;
            } else {
                mapStep = (maxValue.UINT16 - minValue.UINT16)/std::pow(2,zinfo->mapSize-1);
                avgMap = intAvgHash/mapStep;
                rangeMap = intRangeHash/mapStep;
            }
            break;
        case ZSIM_INT16:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(int16_t); i++) {
                intSum += ((int16_t*) data)[i];
                if (((int16_t*) data)[i] > intMax)
                    intMax = ((int16_t*) data)[i];
                if (((int16_t*) data)[i] < intMin)
                    intMin = ((int16_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(int16_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.INT16)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.INT16)
                panic("Received a value lower than the annotation's Min!!");
            if (zinfo->mapSize > sizeof(int16_t)) {
                avgMap = intAvgHash;
                rangeMap = intRangeHash;
            } else {
                mapStep = (maxValue.INT16 - minValue.INT16)/std::pow(2,zinfo->mapSize-1);
                avgMap = intAvgHash/mapStep;
                rangeMap = intRangeHash/mapStep;
            }
            break;
        case ZSIM_UINT32:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(uint32_t); i++) {
                intSum += ((uint32_t*) data)[i];
                if (((uint32_t*) data)[i] > intMax)
                    intMax = ((uint32_t*) data)[i];
                if (((uint32_t*) data)[i] < intMin)
                    intMin = ((uint32_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(uint32_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.UINT32)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.UINT32)
                panic("Received a value lower than the annotation's Min!!");
            mapStep = (maxValue.UINT32 - minValue.UINT32)/std::pow(2,zinfo->mapSize-1);
            avgMap = intAvgHash/mapStep;
            rangeMap = intRangeHash/mapStep;
            break;
        case ZSIM_INT32:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(int32_t); i++) {
                intSum += ((int32_t*) data)[i];
                if (((int32_t*) data)[i] > intMax)
                    intMax = ((int32_t*) data)[i];
                if (((int32_t*) data)[i] < intMin)
                    intMin = ((int32_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(int32_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.INT32)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.INT32)
                panic("Received a value lower than the annotation's Min!!");
            mapStep = (maxValue.INT32 - minValue.INT32)/std::pow(2,zinfo->mapSize-1);
            avgMap = intAvgHash/mapStep;
            rangeMap = intRangeHash/mapStep;
            break;
        case ZSIM_UINT64:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(uint64_t); i++) {
                intSum += ((uint64_t*) data)[i];
                if ((int64_t)(((uint64_t*) data)[i]) > intMax)
                    intMax = ((uint64_t*) data)[i];
                if ((int64_t)(((uint64_t*) data)[i]) < intMin)
                    intMin = ((uint64_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(uint64_t));
            intRangeHash = intMax - intMin;
            if (intMax > (int64_t)maxValue.UINT64)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < (int64_t)minValue.UINT64)
                panic("Received a value lower than the annotation's Min!!");
            mapStep = (maxValue.UINT64 - minValue.UINT64)/std::pow(2,zinfo->mapSize-1);
            avgMap = intAvgHash/mapStep;
            rangeMap = intRangeHash/mapStep;
            break;
        case ZSIM_INT64:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(int64_t); i++) {
                intSum += ((int64_t*) data)[i];
                if (((int64_t*) data)[i] > intMax)
                    intMax = ((int64_t*) data)[i];
                if (((int64_t*) data)[i] < intMin)
                    intMin = ((int64_t*) data)[i];
            }
            intAvgHash = intSum/(zinfo->lineSize/sizeof(int64_t));
            intRangeHash = intMax - intMin;
            if (intMax > maxValue.INT64)
                panic("Received a value bigger than the annotation's Max!!");
            if (intMin < minValue.INT64)
                panic("Received a value lower than the annotation's Min!!");
            mapStep = (maxValue.INT64 - minValue.INT64)/std::pow(2,zinfo->mapSize-1);
            avgMap = intAvgHash/mapStep;
            rangeMap = intRangeHash/mapStep;
            break;
        case ZSIM_FLOAT:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(float); i++) {
                floatSum += ((float*) data)[i];
                if (((float*) data)[i] > floatMax)
                    floatMax = ((float*) data)[i];
                if (((float*) data)[i] < floatMin)
                    floatMin = ((float*) data)[i];
            }
            floatAvgHash = floatSum/(zinfo->lineSize/sizeof(float));
            floatRangeHash = floatMax - floatMin;
            // if (floatMax > maxValue.FLOAT)
                // warn("Received a value bigger than the annotation's Max!! %.10f, %.10f", floatMax, maxValue.FLOAT);
            // if (floatMin < minValue.FLOAT)
                // warn("Received a value lower than the annotation's Min!! %.10f, %.10f", floatMin, minValue.FLOAT);
            mapStep = (maxValue.FLOAT - minValue.FLOAT)/std::pow(2,zinfo->mapSize-1);
            avgMap = floatAvgHash/mapStep;
            rangeMap = floatRangeHash/mapStep;
            break;
        case ZSIM_DOUBLE:
            for (uint32_t i = 0; i < zinfo->lineSize/sizeof(double); i++) {
                floatSum += ((double*) data)[i];
                if (((double*) data)[i] > floatMax)
                    floatMax = ((double*) data)[i];
                if (((double*) data)[i] < floatMin)
                    floatMin = ((double*) data)[i];
            }
            floatAvgHash = floatSum/(zinfo->lineSize/sizeof(double));
            floatRangeHash = floatMax - floatMin;
            // if (floatMax > maxValue.DOUBLE)
                // warn("Received a value bigger than the annotation's Max!! %.10f, %.10f", floatMax, maxValue.DOUBLE);
            // if (floatMin < minValue.DOUBLE)
                // warn("Received a value lower than the annotation's Min!! %.10f, %.10f", floatMin, minValue.DOUBLE);
            mapStep = (maxValue.DOUBLE - minValue.DOUBLE)/std::pow(2,zinfo->mapSize-1);
            avgMap = floatAvgHash/mapStep;
            rangeMap = floatRangeHash/mapStep;
            break;
        default:
            panic("Wrong Data Type!!");
    }
    map = ((uint32_t)avgMap << (32 - zinfo->mapSize)) >> (32 - zinfo->mapSize);
    rangeMap = ((uint32_t)rangeMap << (32 - zinfo->mapSize/2)) >> (32 - zinfo->mapSize/2);
    rangeMap = (rangeMap << zinfo->mapSize);
    map |= rangeMap;
    return map;
}

int32_t uniDoppelgangerBDIDataArray::preinsert(uint32_t map) {
    uint32_t set = hf->hash(0, map) & setMask;
    return set;
}

int32_t uniDoppelgangerBDIDataArray::preinsert(uint32_t set, const MemReq* req, int32_t* tagId, g_vector<uint32_t>& exceptions) {
    uint32_t first = set*assoc;

    uint32_t mapId = rp->rank(req, SetAssocCands(first, first+assoc), exceptions);

    *tagId = tagPointerArray[set][mapId%assoc];
    return mapId%assoc;
}

void uniDoppelgangerBDIDataArray::postinsert(int32_t map, const MemReq* req, int32_t mapId, int32_t segmentId, int32_t tagId, int32_t counter, BDICompressionEncoding compression, bool approximate, bool updateReplacement) {
    if (tagPointerArray[mapId][segmentId] == -1 && tagId != -1) {
        validSegments+=BDICompressionToSize(compression, zinfo->lineSize)/8;
            // info("UP");
        // validLines++;
    } else if (tagPointerArray[mapId][segmentId] != -1 && tagId == -1) {
        validSegments-=BDICompressionToSize(compressionEncodingArray[mapId][segmentId], zinfo->lineSize)/8;
        assert(validSegments);
    } else if (tagPointerArray[mapId][segmentId] != -1 && tagId != -1) {
        validSegments+=BDICompressionToSize(compression, zinfo->lineSize)/8;
        validSegments-=BDICompressionToSize(compressionEncodingArray[mapId][segmentId], zinfo->lineSize)/8;
    }
    rp->replaced(mapId*assoc+segmentId);
    mtagArray[mapId][segmentId] = map;
    tagPointerArray[mapId][segmentId] = tagId;
    tagCounterArray[mapId][segmentId] = counter;
    compressionEncodingArray[mapId][segmentId] = compression;
    approximateArray[mapId][segmentId] = approximate;
    if(updateReplacement) rp->update(mapId*assoc+segmentId, req);
}

void uniDoppelgangerBDIDataArray::changeInPlace(int32_t map, const MemReq* req, int32_t mapId, int32_t segmentId, int32_t tagId, int32_t counter, BDICompressionEncoding compression, bool approximate, bool updateReplacement) {
    validSegments-=BDICompressionToSize(compressionEncodingArray[mapId][segmentId], zinfo->lineSize)/8;
    validSegments+=BDICompressionToSize(compression, zinfo->lineSize)/8;
    mtagArray[mapId][segmentId] = map;
    tagPointerArray[mapId][segmentId] = tagId;
    tagCounterArray[mapId][segmentId] = counter;
    compressionEncodingArray[mapId][segmentId] = compression;
    approximateArray[mapId][segmentId] = approximate;
    if(updateReplacement) rp->update(mapId*assoc+segmentId, req);
}

int32_t uniDoppelgangerBDIDataArray::readListHead(int32_t mapId, int32_t segmentId) {
    return tagPointerArray[mapId][segmentId];
}

int32_t uniDoppelgangerBDIDataArray::readCounter(int32_t mapId, int32_t segmentId) {
    return tagCounterArray[mapId][segmentId];
}

bool uniDoppelgangerBDIDataArray::readApproximate(int32_t mapId, int32_t segmentId) {
    return approximateArray[mapId][segmentId];
}

BDICompressionEncoding uniDoppelgangerBDIDataArray::readCompressionEncoding(int32_t mapId, int32_t segmentId) {
    return compressionEncodingArray[mapId][segmentId];
}

int32_t uniDoppelgangerBDIDataArray::readMap(int32_t mapId, int32_t segmentId) {
    return mtagArray[mapId][segmentId];
}

void uniDoppelgangerBDIDataArray::print() {
    for (uint32_t i = 0; i < this->numSets; i++) {
        for (uint32_t j = 0; j < assoc; j++) {
            if (tagPointerArray[i][j] != -1)
                info("%i, %i: %i, %i, %i, %s, %s", i,j, mtagArray[i][j], tagPointerArray[i][j], tagCounterArray[i][j], BDICompressionName(compressionEncodingArray[i][j]), approximateArray[i][j]? "approximate":"exact");
        }
    }
}

uint32_t uniDoppelgangerBDIDataArray::getValidSegments() {
    return validSegments;
}

// uint32_t uniDoppelgangerBDIDataArray::countValidLines() {
//     uint32_t Counter = 0;
//     for (uint32_t i = 0; i < numSets; i++) {
//         for (uint32_t j = 0; j < assoc; j++) {
//             if (tagPointerArray[i][j] != -1)
//                 Counter++;
//         }
//     }
//     return Counter;
// }
// Doppelganger BDI End

/* ZCache implementation */

ZArray::ZArray(uint32_t _numLines, uint32_t _ways, uint32_t _candidates, ReplPolicy* _rp, HashFamily* _hf) //(int _size, int _lineSize, int _assoc, int _zassoc, ReplacementPolicy<T>* _rp, int _hashType)
    : rp(_rp), hf(_hf), numLines(_numLines), ways(_ways), cands(_candidates)
{
    assert_msg(ways > 1, "zcaches need >=2 ways to work");
    assert_msg(cands >= ways, "candidates < ways does not make sense in a zcache");
    assert_msg(numLines % ways == 0, "number of lines is not a multiple of ways");

    //Populate secondary parameters
    numSets = numLines/ways;
    assert_msg(isPow2(numSets), "must have a power of 2 # sets, but you specified %d", numSets);
    setMask = numSets - 1;

    lookupArray = gm_calloc<uint32_t>(numLines);
    array = gm_calloc<Address>(numLines);
    for (uint32_t i = 0; i < numLines; i++) {
        lookupArray[i] = i;  // start with a linear mapping; with swaps, it'll get progressively scrambled
    }
    swapArray = gm_calloc<uint32_t>(cands/ways + 2);  // conservative upper bound (tight within 2 ways)
}

void ZArray::initStats(AggregateStat* parentStat) {
    AggregateStat* objStats = new AggregateStat();
    objStats->init("array", "ZArray stats");
    statSwaps.init("swaps", "Block swaps in replacement process");
    objStats->append(&statSwaps);
    parentStat->append(objStats);
}

int32_t ZArray::lookup(const Address lineAddr, const MemReq* req, bool updateReplacement) {
    /* Be defensive: If the line is 0, panic instead of asserting. Now this can
     * only happen on a segfault in the main program, but when we move to full
     * system, phy page 0 might be used, and this will hit us in a very subtle
     * way if we don't check.
     */
    if (unlikely(!lineAddr)) panic("ZArray::lookup called with lineAddr==0 -- your app just segfaulted");

    for (uint32_t w = 0; w < ways; w++) {
        uint32_t lineId = lookupArray[w*numSets + (hf->hash(w, lineAddr) & setMask)];
        if (array[lineId] == lineAddr) {
            if (updateReplacement) {
                rp->update(lineId, req);
            }
            return lineId;
        }
    }
    return -1;
}

uint32_t ZArray::preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr) {
    ZWalkInfo candidates[cands + ways]; //extra ways entries to avoid checking on every expansion

    bool all_valid = true;
    uint32_t fringeStart = 0;
    uint32_t numCandidates = ways; //seeds

    //info("Replacement for incoming 0x%lx", lineAddr);

    //Seeds
    for (uint32_t w = 0; w < ways; w++) {
        uint32_t pos = w*numSets + (hf->hash(w, lineAddr) & setMask);
        uint32_t lineId = lookupArray[pos];
        candidates[w].set(pos, lineId, -1);
        all_valid &= (array[lineId] != 0);
        //info("Seed Candidate %d addr 0x%lx pos %d lineId %d", w, array[lineId], pos, lineId);
    }

    //Expand fringe in BFS fashion
    while (numCandidates < cands && all_valid) {
        uint32_t fringeId = candidates[fringeStart].lineId;
        Address fringeAddr = array[fringeId];
        assert(fringeAddr);
        for (uint32_t w = 0; w < ways; w++) {
            uint32_t hval = hf->hash(w, fringeAddr) & setMask;
            uint32_t pos = w*numSets + hval;
            uint32_t lineId = lookupArray[pos];

            // Logically, you want to do this...
#if 0
            if (lineId != fringeId) {
                //info("Candidate %d way %d addr 0x%lx pos %d lineId %d parent %d", numCandidates, w, array[lineId], pos, lineId, fringeStart);
                candidates[numCandidates++].set(pos, lineId, (int32_t)fringeStart);
                all_valid &= (array[lineId] != 0);
            }
#endif
            // But this compiles as a branch and ILP sucks (this data-dependent branch is long-latency and mispredicted often)
            // Logically though, this is just checking for whether we're revisiting ourselves, so we can eliminate the branch as follows:
            candidates[numCandidates].set(pos, lineId, (int32_t)fringeStart);
            all_valid &= (array[lineId] != 0);  // no problem, if lineId == fringeId the line's already valid, so no harm done
            numCandidates += (lineId != fringeId); // if lineId == fringeId, the cand we just wrote will be overwritten
        }
        fringeStart++;
    }

    //Get best candidate (NOTE: This could be folded in the code above, but it's messy since we can expand more than zassoc elements)
    assert(!all_valid || numCandidates >= cands);
    numCandidates = (numCandidates > cands)? cands : numCandidates;

    //info("Using %d candidates, all_valid=%d", numCandidates, all_valid);

    uint32_t bestCandidate = rp->rankCands(req, ZCands(&candidates[0], &candidates[numCandidates]));
    assert(bestCandidate < numLines);

    //Fill in swap array

    //Get the *minimum* index of cands that matches lineId. We need the minimum in case there are loops (rare, but possible)
    uint32_t minIdx = -1;
    for (uint32_t ii = 0; ii < numCandidates; ii++) {
        if (bestCandidate == candidates[ii].lineId) {
            minIdx = ii;
            break;
        }
    }
    assert(minIdx >= 0);
    //info("Best candidate is %d lineId %d", minIdx, bestCandidate);

    lastCandIdx = minIdx; //used by timing simulation code to schedule array accesses

    int32_t idx = minIdx;
    uint32_t swapIdx = 0;
    while (idx >= 0) {
        swapArray[swapIdx++] = candidates[idx].pos;
        idx = candidates[idx].parentIdx;
    }
    swapArrayLen = swapIdx;
    assert(swapArrayLen > 0);

    //Write address of line we're replacing
    *wbLineAddr = array[bestCandidate];

    return bestCandidate;
}

void ZArray::postinsert(const Address lineAddr, const MemReq* req, uint32_t candidate) {
    //We do the swaps in lookupArray, the array stays the same
    assert(lookupArray[swapArray[0]] == candidate);
    for (uint32_t i = 0; i < swapArrayLen-1; i++) {
        //info("Moving position %d (lineId %d) <- %d (lineId %d)", swapArray[i], lookupArray[swapArray[i]], swapArray[i+1], lookupArray[swapArray[i+1]]);
        lookupArray[swapArray[i]] = lookupArray[swapArray[i+1]];
    }
    lookupArray[swapArray[swapArrayLen-1]] = candidate; //note that in preinsert() we walk the array backwards when populating swapArray, so the last elem is where the new line goes
    //info("Inserting lineId %d in position %d", candidate, swapArray[swapArrayLen-1]);

    rp->replaced(candidate);
    array[candidate] = lineAddr;
    rp->update(candidate, req);

    statSwaps.inc(swapArrayLen-1);
}

