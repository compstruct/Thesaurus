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

#include "memory_hierarchy.h"

static const char* accessTypeNames[] = {"GETS", "GETX", "PUTS", "PUTX"};
static const char* invTypeNames[] = {"INV", "INVX"};
static const char* mesiStateNames[] = {"I", "S", "E", "M"};
static const char* dataTypeNames[] = {"UINT8", "INT8", "UINT16", "INT16", "UINT32", "INT32", "UINT64", "INT64", "FLOAT", "DOUBLE"};
static const char* BDICompressionNames[] = {"ZERO", "REPETITIVE", "BASE8DELTA1", "BASE8DELTA2", "BASE8DELTA4", "BASE4DELTA1", "BASE4DELTA2", "BASE2DELTA1", "NONE"};

const char* AccessTypeName(AccessType t) {
    assert_msg(t >= 0 && (size_t)t < sizeof(accessTypeNames)/sizeof(const char*), "AccessTypeName got an out-of-range input, %d", t);
    return accessTypeNames[t];
}

const char* InvTypeName(InvType t) {
    assert_msg(t >= 0 && (size_t)t < sizeof(invTypeNames)/sizeof(const char*), "InvTypeName got an out-of-range input, %d", t);
    return invTypeNames[t];
}

const char* MESIStateName(MESIState s) {
    assert_msg(s >= 0 && (size_t)s < sizeof(mesiStateNames)/sizeof(const char*), "MESIStateName got an out-of-range input, %d", s);
    return mesiStateNames[s];
}

const char* DataTypeName(DataType t) {
    assert_msg(t >= 0 && (size_t)t < sizeof(dataTypeNames)/sizeof(const char*), "DataTypeName got an out-of-range input, %d", t);
    return dataTypeNames[t];
}

const char* BDICompressionName(BDICompressionEncoding encoding) {
    assert_msg(encoding >= 0 && (size_t)encoding < sizeof(BDICompressionNames)/sizeof(const char*), "BDICompressionName got an out-of-range input, %d", encoding);
    return BDICompressionNames[encoding];
}

uint16_t BDICompressionToSize(BDICompressionEncoding encoding, uint32_t lineSize) {
    switch(encoding) {
        case ZERO:
        case REPETITIVE:
            return 8;
        case BASE8DELTA1:
            return 16;
        case BASE4DELTA1:
        case BASE8DELTA2:
            return 24;
        case BASE2DELTA1:
        case BASE4DELTA2:
        case BASE8DELTA4:
            return 40;
        default:
            return lineSize;
    }
}

#include <type_traits>

static inline void CompileTimeAsserts() {
    static_assert(std::is_pod<MemReq>::value, "MemReq not POD!");
}

