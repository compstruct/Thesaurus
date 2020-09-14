#ifndef __ZSIM_HOOKS_H__
#define __ZSIM_HOOKS_H__

#include <stdint.h>
#include <stdio.h>
#include <float.h>
#include <malloc.h>

//Avoid optimizing compilers moving code around this barrier
#define COMPILER_BARRIER() { __asm__ __volatile__("" ::: "memory");}

//These need to be in sync with the simulator
#define ZSIM_MAGIC_OP_ROI_BEGIN         (1025)
#define ZSIM_MAGIC_OP_ROI_END           (1026)
#define ZSIM_MAGIC_OP_REGISTER_THREAD   (1027)
#define ZSIM_MAGIC_OP_HEARTBEAT         (1028)
#define ZSIM_MAGIC_OP_WORK_BEGIN        (1029) //ubik
#define ZSIM_MAGIC_OP_WORK_END          (1030) //ubik

#ifdef __x86_64__
#define HOOKS_STR  "HOOKS"
static inline void zsim_magic_op(uint64_t op) {
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op));
    COMPILER_BARRIER();
}
#else
#define HOOKS_STR  "NOP-HOOKS"
static inline void zsim_magic_op(uint64_t op) {
    //NOP
}
#endif

static inline void zsim_roi_begin() {
    printf("[" HOOKS_STR "] ROI begin\n");
    zsim_magic_op(ZSIM_MAGIC_OP_ROI_BEGIN);
}

static inline void zsim_roi_end() {
    zsim_magic_op(ZSIM_MAGIC_OP_ROI_END);
    printf("[" HOOKS_STR  "] ROI end\n");
}

static inline void zsim_heartbeat() {
    zsim_magic_op(ZSIM_MAGIC_OP_HEARTBEAT);
}

static inline void zsim_work_begin() { zsim_magic_op(ZSIM_MAGIC_OP_WORK_BEGIN); }
static inline void zsim_work_end() { zsim_magic_op(ZSIM_MAGIC_OP_WORK_END); }

typedef enum {
    HOOKS_UINT8 = 0,
    HOOKS_INT8 = 1,
    HOOKS_UINT16 = 2,
    HOOKS_INT16 = 3,
    HOOKS_UINT32 = 4,
    HOOKS_INT32 = 5,
    HOOKS_UINT64 = 6,
    HOOKS_INT64 = 7,
    HOOKS_FLOAT = 8,
    HOOKS_DOUBLE = 9,
    HOOKS_float = 8,
    HOOKS_double = 9
} DataType;

typedef union
{
    uint8_t HOOKS_UINT8;
    int8_t HOOKS_INT8;
    uint16_t HOOKS_UINT16;
    int16_t HOOKS_INT16;
    uint32_t HOOKS_UINT32;
    int32_t HOOKS_INT32;
    uint64_t HOOKS_UINT64;
    int64_t HOOKS_INT64;
    float HOOKS_FLOAT;
    double HOOKS_DOUBLE;
} DataValue;

static inline void zsim_allocate_approximate(void* Start, uint64_t ByteLength, DataType Type)
{
    // printf("[" HOOKS_STR "] Approximate Allocation1\n");
    DataValue* minValue = (DataValue*) malloc(sizeof(DataValue));
    DataValue* maxValue = (DataValue*) malloc(sizeof(DataValue));
    // milc
    if (Type == HOOKS_DOUBLE)
    {
        minValue->HOOKS_DOUBLE = -4.1588886540090808;
        maxValue->HOOKS_DOUBLE = 5.0871395561061092;
    }
    else
    {
        minValue->HOOKS_FLOAT = -4.1588886540090808;
	    maxValue->HOOKS_FLOAT = 5.0871395561061092;
    }
    // gromacs
    if (Type == HOOKS_DOUBLE)
    {
        minValue->HOOKS_DOUBLE = -6.9526329538056441e-05;
        maxValue->HOOKS_DOUBLE = 4.0843751882354336e+19;
    }
    else
    {
        minValue->HOOKS_FLOAT = -6.9526329538056441e-05;
        maxValue->HOOKS_FLOAT = 4.0843751882354336e+19;
    }
    // namd
    if (Type == HOOKS_DOUBLE)
    {
        minValue->HOOKS_DOUBLE = -99999;
        maxValue->HOOKS_DOUBLE = 1850;
    }
    else
    {
        minValue->HOOKS_FLOAT = -71.6800003;
        maxValue->HOOKS_FLOAT = 89128.9609;
    }
    // soplex
    if (Type == HOOKS_DOUBLE)
    {
        minValue->HOOKS_DOUBLE = -25.01;
        maxValue->HOOKS_DOUBLE = 25.01;
    }
    else
    {
        minValue->HOOKS_FLOAT = 89128.9609;
        maxValue->HOOKS_FLOAT = -71.6800003;
    }
    // calculix
    if (Type == HOOKS_DOUBLE)
    {
        minValue->HOOKS_DOUBLE = -180000000;
        maxValue->HOOKS_DOUBLE = 1.0000001e+100;
    }
    else
    {
        minValue->HOOKS_FLOAT = -180000000;
        maxValue->HOOKS_FLOAT = 1.0000001e+100;
    }
    // lbm
    if (Type == HOOKS_DOUBLE)
    {
        minValue->HOOKS_DOUBLE = 0;
        maxValue->HOOKS_DOUBLE = 1311114;
    }
    else
    {
        minValue->HOOKS_FLOAT = 0;
        maxValue->HOOKS_FLOAT = 1311114;
    }
    // __asm__ __volatile__
    // (
    //     ".byte 0x0F, 0x1F, 0x80, 0xFF, 0x00, 0x11, 0x55 ;\n\t"
    //     "add %3, %0 ;\n\t"
    //     "add %3, %1 ;\n\t"
    //     "add %3, %2 ;\n\t"
    //     :
    //     : "r" ((uint64_t)Start), "r" (ByteLength), "r" (Type), "i" (0)
    //     :
    // );
    __asm__ __volatile__
    (
        ".byte 0x0F, 0x1F, 0x80, 0xFF, 0x00, 0x11, 0x22 ;\n\t"
        "add %5, %0 ;\n\t"
        "add %5, %1 ;\n\t"
        "add %5, %2 ;\n\t"
        "add %5, %3 ;\n\t"
        "add %5, %4 ;\n\t"
        :
        : "r" ((uint64_t)Start), "r" (ByteLength), "r" (Type), "r" ((uint64_t)minValue), "r" ((uint64_t)maxValue), "i" (0)
        :
    );
}

static inline void zsim_elaborate_allocate_approximate(void* Start, uint64_t ByteLength, DataType Type, DataValue* Min, DataValue* Max)
{
    // printf("[" HOOKS_STR "] Approximate Allocation2\n");
    __asm__ __volatile__
    (
        ".byte 0x0F, 0x1F, 0x80, 0xFF, 0x00, 0x11, 0x22 ;\n\t"
        "add %5, %0 ;\n\t"
        "add %5, %1 ;\n\t"
        "add %5, %2 ;\n\t"
        "add %5, %3 ;\n\t"
        "add %5, %4 ;\n\t"
        :
        : "r" ((uint64_t)Start), "r" (ByteLength), "r" (Type), "r" ((uint64_t)Min), "r" ((uint64_t)Max), "i" (0)
        :
    );
}

static inline void zsim_reallocate_approximate(void* Start, uint64_t ByteLength)
{
    // printf("[" HOOKS_STR "] Approximate Reallocation\n");
    __asm__ __volatile__
    (
        ".byte 0x0F, 0x1F, 0x80, 0xFF, 0x00, 0x11, 0x33 ;\n\t"
        "add %2, %0 ;\n\t"
        "add %2, %1 ;\n\t"
        :
        : "r" ((uint64_t)Start), "r" (ByteLength), "i" (0)
        :
    );
}

static inline void zsim_deallocate_approximate(void* Start)
{
    // printf("[" HOOKS_STR "] Approximate Deallocation\n");
    __asm__ __volatile__
    (
        ".byte 0x0F, 0x1F, 0x80, 0xFF, 0x00, 0x11, 0x44 ;\n\t"
        "add %1, %0 ;\n\t"
        "add %1, %0 ;\n\t"
        :
        : "r" ((uint64_t)Start), "i" (0)
        :
    );
}

#endif /*__ZSIM_HOOKS_H__*/
