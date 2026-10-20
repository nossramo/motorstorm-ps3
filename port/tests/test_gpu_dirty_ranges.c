#include "ps3emu/guest_read_sync.h"
#include <assert.h>
#include <stdio.h>

static unsigned calls;
static void read_request(uint32_t address, uint32_t bytes) {
    (void)address; (void)bytes; ++calls;
}
int main(void) {
    ps3_gpu_sync_set_handler(read_request);
    ps3_gpu_sync_mark(0x20003, 1, 1);
    ps3_gpu_read_sync(0x20003, 1);
    assert(calls == 1);
    ps3_gpu_sync_mark(0x20003, 1, 0);
    ps3_gpu_read_sync(0x20003, 1);
    assert(calls == 2); /* partial line cannot clear dirty neighbours */
    ps3_gpu_sync_mark(0x20000, 64, 0);
    ps3_gpu_read_sync(0x20003, 1);
    assert(calls == 2);

    ps3_gpu_sync_mark(0x20040, 128, 1);
    ps3_gpu_sync_mark(0x20040, 64, 0);
    ps3_gpu_read_sync(0x20040, 1);
    assert(calls == 2);
    ps3_gpu_read_sync(0x20080, 1);
    assert(calls == 3); /* second dirty line in the same page survives */
    ps3_gpu_sync_mark(0x20040, 1, 1);
    ps3_gpu_read_sync(0x20040, 1);
    assert(calls == 4); /* redraw after clean */

    ps3_gpu_sync_mark(0x22000, 1, 1);
    ps3_gpu_read_sync(0x21fff, 2);
    assert(calls == 5); /* dirty second page of one CPU load */
    ps3_gpu_sync_mark(0x22000, 64, 0);
    ps3_gpu_read_sync(0x21fff, 2);
    assert(calls == 5);

    ps3_gpu_sync_mark(0xffffffffu, 1, 1);
    ps3_gpu_read_sync(0xffffffffu, 1);
    assert(calls == 6);
    ps3_gpu_sync_mark(0xffffffc0u, 64, 0);
    ps3_gpu_read_sync(0xffffffffu, 1);
    ps3_gpu_sync_mark(0xffffffffu, 2, 1);
    ps3_gpu_read_sync(0xffffffffu, 2);
    ps3_gpu_read_sync(0x22000, 0);
    assert(calls == 6); /* no overflow/zero-length wrap into another page */
    puts("GPU dirty line ranges: PASS");
}
