#include "copy.inc"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
uint8_t* vm_base;
unsigned char ps3_gpu_dirty_pages[1u << 20];
static unsigned syncs;
void ps3_gpu_sync_dispatch(uint32_t, uint32_t) { ++syncs; }
}
static uint64_t rng=0x123456789;
static uint64_t next() { rng^=rng<<13; rng^=rng>>7; return rng^=rng<<17; }
static constexpr unsigned SIZE=256*1024;
static void initial(ppu_context& ctx,unsigned dst,unsigned src,unsigned length) {
    auto* bytes=(uint8_t*)&ctx;
    for(size_t i=0;i<sizeof ctx;++i) bytes[i]=(uint8_t)next();
    ctx.gpr[3]=dst; ctx.gpr[4]=src; ctx.gpr[5]=length;
}
int main() {
    std::vector<uint8_t> a(SIZE),b(SIZE),original(SIZE);
    for(auto& v:original) v=(uint8_t)next();
    unsigned cases=0;
    for(unsigned sa=0;sa<16;++sa) for(unsigned da=0;da<16;++da)
    for(unsigned length:{0u,1u,2u,7u,15u,16u,17u,31u,32u,33u,127u,128u,129u,1023u,4096u})
    for(unsigned overlap=0;overlap<3;++overlap) {
        const unsigned src=0x10000+sa;
        const unsigned dst=overlap==0 ? 0x18000+da : overlap==1 ? 0x10010+da : 0xffe0+da;
        ppu_context ca{},cb{}; initial(ca,dst,src,length); cb=ca; a=original; b=original;
        // GPU visibility remains on both paths, including dirty pages.
        memset(ps3_gpu_dirty_pages,cases&1,sizeof ps3_gpu_dirty_pages);
        syncs=0; vm_base=a.data(); reference_copy(&ca); unsigned old_syncs=syncs;
        syncs=0; vm_base=b.data(); candidate_copy(&cb);
        if(memcmp(&ca,&cb,sizeof ca) || a!=b || old_syncs!=syncs) {
            fprintf(stderr,"FAIL copy src=%u dst=%u n=%u overlap=%u\n",sa,da,length,overlap); return 1;
        }
        ++cases;
    }
    printf("PASS copy: %u full-context/memory cases, all alignments, overlap, zero/tails, GPU hooks\n",cases);
    memset(ps3_gpu_dirty_pages,0,sizeof ps3_gpu_dirty_pages);
    // Independent calls through opaque production memory helpers. Repeated
    // A/B order alternates; report medians, without asserting a speedup.
    using Clock=std::chrono::steady_clock;
    for(unsigned length:{256u,4096u,65536u}) {
        std::vector<double> times[2]; const unsigned iterations=20000;
        for(unsigned round=0;round<8;++round) for(unsigned order=0;order<2;++order) {
            unsigned which=order^(round&1); vm_base=a.data();
            ppu_context c{}; initial(c,0x20000,0x1000,length);
            auto start=Clock::now();
            for(unsigned i=0;i<iterations;++i) {
                c.gpr[3]=0x20000; c.gpr[4]=0x1000; c.gpr[5]=length;
                if(which) candidate_copy(&c); else reference_copy(&c);
            }
            auto end=Clock::now();
            times[which].push_back(std::chrono::duration<double,std::nano>(end-start).count()/iterations);
            assert(c.gpr[5] == 0 || c.gpr[5] <= length);
        }
        for(auto& t:times) std::sort(t.begin(),t.end());
        double old=times[0][4],now=times[1][4];
        printf("Host copy %u bytes: original %.1f ns, locals %.1f ns, %.2fx (kernel only)\n",length,old,now,old/now);
    }
}
