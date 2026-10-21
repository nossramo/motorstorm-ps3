#include "spu_audio_worker.h"
#include <assert.h>
#include "audio_bounds.inc"

static uint32_t random_bits = 0x71853aca;
static uint32_t next(void) {
    random_bits ^= random_bits << 13; random_bits ^= random_bits >> 17;
    random_bits ^= random_bits << 5; return random_bits;
}
int main(void) {
    const uint32_t edges[] = {0,0x80000000,1,0x7fffff,0x800000,0x80800000,
        0x3f800000,0xbf800000,0x47800000,0xc7800000,0x477fffff,0x47800001,
        0x7f7fffff,0x7f800000,0xffffffff};
    u128 input[128], reference[128], candidate[128];
    unsigned groups = sizeof(audio_groups) / sizeof(*audio_groups);
    for (unsigned group = 0; group < groups; ++group) {
        for (unsigned n = 0; n < 30000; ++n) {
            for (unsigned r = 0; r < 128; ++r) for (unsigned lane = 0; lane < 4; ++lane) {
                uint32_t v = next();
                if (n % 4 == 0) v = (v & 0x80000000u) | ((v & 0x7fffffffu) % 0x47800001u);
                if (n % 4 == 1) v = edges[v % (sizeof(edges) / sizeof(*edges))];
                if (n % 4 == 2) v = (v & 0x807fffffu) | ((next() % 20 + 120) << 23);
                input[r]._u32[lane] = v;
            }
            memcpy(reference,input,sizeof input); memcpy(candidate,input,sizeof input);
            uint64_t host = ms_rtz_enter();
            audio_groups[group][0](reference,host);
            audio_groups[group][1](candidate,host);
            ms_rtz_leave(host);
            if (memcmp(reference,candidate,sizeof reference)) {
                fprintf(stderr,"FAIL group=%u case=%u\n",group,n); return 1;
            }
        }
    }
    printf("PASS %u actual audio groups x 30000 contexts: bounded, arbitrary, normal and threshold inputs\n",groups);
}
