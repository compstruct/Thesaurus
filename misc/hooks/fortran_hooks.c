#include "zsim_hooks.h"
#include "zsim_malloc.h"

void zsim_roi_begin_() {
    zsim_roi_begin();
}

void zsim_roi_end_() {
    zsim_roi_end();
}

void zsim_heartbeat_() {
    zsim_heartbeat();
}

void zsim_mallocs_init_() {
    zsim_mallocs_init();
}

void zsim_allocate_approximate_(void* Start, uint64_t ByteLength, DataType Type) {
    zsim_allocate_approximate(Start, ByteLength, Type);
}

void zsim_deallocate_approximate_(void* Start) {
    zsim_deallocate_approximate(Start);
}
