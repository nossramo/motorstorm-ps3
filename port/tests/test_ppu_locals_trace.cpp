#include "ppu_recomp.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <unordered_map>
#include <vector>

struct Stop {};
static std::unordered_map<uint32_t, uint8_t> memory;
static std::vector<uint8_t> trace;
static std::set<unsigned> coverage;
static uint64_t rng, seed;
static unsigned mode, steps, calls, returns;
static uint64_t mix(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL; return x ^ (x >> 31);
}
static uint64_t random64() { return rng = mix(rng + 0x9e3779b97f4a7c15ULL); }
static void record(const void* p, size_t n) {
    if (trace.size() + n > 128 * 1024) throw Stop{};
    const auto* b = (const uint8_t*)p; trace.insert(trace.end(), b, b+n);
}
static void event(uint64_t kind, uint64_t a, uint64_t b) {
    uint64_t words[] = {kind, a, b}; record(words, sizeof words);
}
void ms_step(unsigned label) {
    if (++steps > 250) throw Stop{};
    coverage.insert(label); event('L', label, 0);
}
static uint8_t byte(uint32_t a) {
    auto i = memory.find(a);
    if (i != memory.end()) return i->second;
    // Stable byte-addressed memory, ranging from zeroed structs and short
    // counts to arbitrary bit patterns, floats and switch-table targets.
    const uint64_t x = mix((a & ~3u) ^ seed);
    uint32_t v = mode == 0 ? 0 : mode == 1 ? (x & 3) : mode == 2 ? (x & 63) :
                 mode == 3 ? ((x & 0xffff) << 4) : mode == 4 ? (uint32_t)x :
                 mode == 5 ? (0x3f000000u | (x & 0x7fffff)) : 0xffffffffu;
    return (uint8_t)(v >> (8 * (3 - (a & 3))));
}
uint64_t ms_read(unsigned n, uint64_t address) {
    uint64_t v = 0; for (unsigned i=0;i<n;++i) v=(v<<8)|byte((uint32_t)address+i);
    event('R'+n, address, v); return v;
}
void ms_write(unsigned n, uint64_t address, uint64_t value) {
    event('W'+n, address, value);
    for (unsigned i=0;i<n;++i) memory[(uint32_t)address+i]=(uint8_t)(value>>(8*(n-1-i)));
}
void ms_raw_load(void* dst, uint64_t a) {
    uint8_t bytes[16]; for(unsigned i=0;i<16;++i) bytes[i]=byte((uint32_t)a+i);
    event('V',a,16); record(bytes,16); memcpy(dst,bytes,16);
}
void ms_raw_store(uint64_t a, const void* src) {
    event('S',a,16); record(src,16);
    for(unsigned i=0;i<16;++i) memory[(uint32_t)a+i]=((const uint8_t*)src)[i];
}
uint64_t ms_timebase() { uint64_t t=random64(); event('T',t,0); return t; }
extern "C" {
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*) = nullptr;
}
static void clobber(ppu_context* c) {
    // Deliberately violate the ABI: opaque hooks and restore routines must
    // not have their nonvolatile results overwritten by cached old values.
    for(unsigned i=0;i<32;++i) {
        c->gpr[i]=random64() & 0xffff;
        const uint64_t bits=random64(); memcpy(&c->fpr[i],&bits,8);
        c->vr[i].d[0]=random64(); c->vr[i].d[1]=random64();
    }
    c->cr=(uint32_t)random64(); c->xer=(uint32_t)random64();
    c->lr=random64(); c->ctr=random64(); c->vscr=(uint32_t)random64();
}
static void tail(void* ptr) {
    auto* c=(ppu_context*)ptr; event('D',0,0); record(c,sizeof *c); clobber(c);
}
void ms_call(ppu_context* c, unsigned tag) {
    ++calls; event('C',tag,0); record(c,sizeof *c); clobber(c);
    if ((rng & 3)==0) g_trampoline_fn=tail;
}
extern "C" void ps3_indirect_call(ppu_context* c) { ms_call(c,0xdead0001); }
extern "C" void ps3_hle_call(unsigned n, ppu_context* c) { ms_call(c,n); }
extern "C" void lv2_syscall(ppu_context* c) { ms_call(c,0xdead0002); }
struct Entry { unsigned address; void (*reference)(ppu_context*); void (*candidate)(ppu_context*); };
#include "entries.inc"

struct Result {
    std::vector<uint8_t> events;
    std::unordered_map<uint32_t,uint8_t> bytes;
    ppu_context ctx;
    void (*trampoline)(void*);
    bool complete;
};
static Result run(void(*fn)(ppu_context*), unsigned trial) {
    seed=trial; rng=mix(seed+1); mode=trial%7; steps=0;
    memory.clear(); trace.clear(); g_trampoline_fn=nullptr;
    ppu_context c{}; clobber(&c);
    c.gpr[1]=0x800000; c.gpr[2]=0x100000;
    // Keep the memory-copy test bounded, including every alignment.
    c.gpr[3]=0x20000+(trial&15); c.gpr[4]=0x40000+((trial>>4)&15);
    c.gpr[5]=(trial%257)+1; c.gpr[6]=trial;
    bool complete=false;
    try { fn(&c); complete=true; ++returns; } catch(const Stop&) {}
    return {trace,memory,c,g_trampoline_fn,complete};
}
int main() {
    unsigned trials=0;
    for(const auto& e:entries) {
        unsigned before_calls=calls, before_returns=returns;
        for(unsigned trial=0;trial<512;++trial) {
            Result a=run(e.reference,trial), b=run(e.candidate,trial);
            if(a.events!=b.events || a.bytes!=b.bytes || a.complete!=b.complete ||
               (a.complete && (memcmp(&a.ctx,&b.ctx,sizeof a.ctx) || a.trampoline!=b.trampoline))) {
                size_t first=0; while(first<std::min(a.events.size(),b.events.size()) && a.events[first]==b.events[first]) ++first;
                fprintf(stderr,"FAIL %08x trial=%u events=%zu/%zu first=%zu complete=%d/%d\n",
                        e.address,trial,a.events.size(),b.events.size(),first,a.complete,b.complete);
                return 1;
            }
            ++trials;
        }
        printf("PASS %08x: 512 bounded differential traces; calls=%u returns=%u (both variants)\n",
               e.address,calls-before_calls,returns-before_returns);
    }
    printf("PASS %u traces, %zu guest labels visited; full context at calls/returns, memory operations identical\n",
           trials,coverage.size());
}
