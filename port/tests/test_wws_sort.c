#include "wws_native_chain.h"
#include "spu_thread_rtz.h"
#include "spu_float_scope.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <time.h>

int g_spu_ls_watch_n,g_spu_hop_fast=1;
void* volatile g_pm_flow_ctx;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
static unsigned stop_pc,hops,min_sp;
static void replay_stop(spu_context* ctx,unsigned pc) { (void)ctx;stop_pc=pc; }
void spu_indirect_branch(spu_context* ctx) { fprintf(stderr,"unexpected indirect %x\n",(unsigned)ctx->pc);abort(); }
void spu_halt(spu_context* ctx) { fprintf(stderr,"original sort halt %x\n",(unsigned)ctx->pc);abort(); }
void spu_img_restore(spu_context* ctx,int32_t image) {ctx->image_id=image;}
static void step(spu_context* ctx) {
    assert(++hops<1000000);
    unsigned sp=ctx->gpr[1]._u32[0];if(sp<min_sp)min_sp=sp;
    void(*fn)(spu_context*)=g_spu_trampoline_fn;g_spu_trampoline_fn=NULL;++ctx->steps;fn(ctx);
}
void spu_drain_call(spu_context* ctx,uint32_t ret) {
    while(g_spu_trampoline_fn&&(uint32_t)ctx->pc!=ret&&!stop_pc)step(ctx);
    assert(!stop_pc);g_spu_trampoline_fn=NULL;
}
#include "wws_sort_reference.inc"
static uint32_t load(const uint8_t*p,unsigned a) {uint32_t x;memcpy(&x,p+a,4);return SPU_BSWAP32(x);}
static void put(uint8_t*p,unsigned a,uint32_t x) {x=SPU_BSWAP32(x);memcpy(p+a,&x,4);}
static uint64_t rng=0x9181fa53;
static unsigned rnd(void) {rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (unsigned)rng;}
static uint32_t bits(float x) {uint32_t b;memcpy(&b,&x,4);return b;}
static uint64_t now(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC_RAW,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static void init(spu_context*c,const ms_wws_chain_fixture*f) {
    memset(c,0,sizeof(*c));c->ls=c->ls_store;
    memcpy(c->gpr,f->gpr,sizeof(f->gpr));memcpy(c->ls,f->ls,SPU_LS_SIZE);
    c->pc=0xDCBC;c->steps=f->header[5];c->image_id=f->header[6];c->resident_ovl=f->header[7];
    c->policy_mode=f->header[8];c->host_depth=f->header[9];c->int_enable=f->header[10];
    c->event_mask=f->header[11];c->event_status=f->header[12];c->status=f->header[13];
    ms_rtz_leave(f->header[14]|((uint64_t)f->header[15]<<32));
}
static void reference(spu_context*c) {
    stop_pc=hops=0;min_sp=c->gpr[1]._u32[0];g_spu_trampoline_fn=NULL;
    jobmod_be896a6374ba_e12230_spu_func_0000DCBC(c);
    while(g_spu_trampoline_fn&&!stop_pc)step(c);
#ifdef TEST_WHOLE_PHASE
    assert(stop_pc==0xF530);
#else
    assert(stop_pc==0xFAE0||stop_pc==0xF020);
#endif
    g_spu_trampoline_fn=NULL;
}
static unsigned comparisons,rejections;
static void compare(spu_context*ref,spu_context*native,const ms_wws_chain_fixture*f,unsigned tag) {
    init(ref,f);reference(ref);unsigned expected_stop=stop_pc;
    init(native,f);int accepted=ms_wws_sort_try(native);
    if(!accepted) {assert(!memcmp(native->ls,f->ls,SPU_LS_SIZE));assert(!memcmp(native->gpr,f->gpr,sizeof(f->gpr)));++rejections;return;}
    stop_pc=0;
#ifdef TEST_WHOLE_PHASE
    assert(accepted==2);wws_sort_epilogue(native);stop_pc=0xF530;
#else
    jobmod_be896a6374ba_e12230_spu_func_0000F010(native);
#endif
    while(g_spu_trampoline_fn&&!stop_pc)step(native);
    assert(stop_pc==expected_stop);g_spu_trampoline_fn=NULL;
    unsigned differences=0,sp=f->gpr[1]._u32[0];
    /* Only dead recursive-sort stack storage is excluded. Bounded recursion
     * sorts the smaller half first; for <=144 items it needs <=3 frames. */
    assert(min_sp>=sp-480);
    for(unsigned a=0;a<SPU_LS_SIZE;a++) {
        if(a>=sp-480&&a<sp+32)continue;
        if(ref->ls[a]!=native->ls[a]) {
            if(differences<10)fprintf(stderr,"case=%u n=%u diff @%x ref=%02x native=%02x\n",tag,f->gpr[18]._u32[0],a,ref->ls[a],native->ls[a]);
            ++differences;
        }
    }
    if(differences)abort();
    for(unsigned r=0;r<128;r++) {
#ifdef TEST_WHOLE_PHASE
        if(r>=2&&r<=28)continue;
#else
        if(r==0||(r>=2&&r<=28)||(r>=80&&r<=86))continue;
#endif
        if(memcmp(&ref->gpr[r],&native->gpr[r],16)) {fprintf(stderr,"case=%u register=%u differs\n",tag,r);abort();}
    }
    ++comparisons;
}
static void prepare(ms_wws_chain_fixture*f,unsigned n) {
    f->gpr[1]=spu_ila(0x3D000);f->gpr[18]=spu_ila(n);f->gpr[87]=spu_ila(0x26CC0);
    put(f->ls,0x16AA0,n);
}
static void permutation(ms_wws_chain_fixture*f,unsigned n) {
    for(unsigned i=0;i<n;i++)put(f->ls,0x26CC0+4*i,i);
    for(unsigned i=n;i>1;i--) {unsigned j=rnd()%i,a=0x26CC0+4*(i-1),b=0x26CC0+4*j;uint32_t t=load(f->ls,a);put(f->ls,a,load(f->ls,b));put(f->ls,b,t);}
}
static void guards(spu_context*c,const ms_wws_chain_fixture*f) {
    spu_context*before=malloc(sizeof(*before));assert(before);
    for(unsigned i=0;i<9;i++) {
        init(c,f);g_spu_trampoline_fn=NULL;
        switch(i) {
        case 0:c->image_id=209;break;
        case 1:c->event_mask=c->event_status=2;c->int_enable=1;break;
        case 2:c->irq_saved=1;break;
        case 3:c->gpr[1]=spu_ila(0x30000);break;
        case 4:c->gpr[87]=spu_ila(0x26CC4);break;
        case 5:c->gpr[18]=spu_ila(145);put(c->ls,0x16AA0,145);break;
        case 6:c->gpr[18]=spu_ila(1);put(c->ls,0x16AA0,1);put(c->ls,0x26CC0,144);break;
        case 7:g_spu_hop_fast=0;break;
        case 8:c->status=SPU_STATUS_STOPPED;break;
        }
        memcpy(before,c,sizeof(*c));assert(!ms_wws_sort_try(c));assert(!memcmp(c,before,sizeof(*c)));g_spu_hop_fast=1;
    }
    free(before);puts("PASS 9 rejected contexts/layouts; no mutation");
}
int main(int argc,char**argv) {
    assert(argc==2);DIR*dir=opendir(argv[1]);assert(dir);
    ms_wws_chain_fixture*f=malloc(sizeof(*f)),*base=malloc(sizeof(*base));
    spu_context*ref=malloc(sizeof(*ref)),*native=malloc(sizeof(*native));assert(f&&base&&ref&&native);
    struct dirent*ent;unsigned captured=0,derived=0;uint64_t elapsed[2]={0},objects=0;
    while((ent=readdir(dir))) {
        if(strncmp(ent->d_name,"wws-",4)||!strstr(ent->d_name,".bin"))continue;
        char path[4096];snprintf(path,sizeof(path),"%s/%s",argv[1],ent->d_name);
        FILE*in=fopen(path,"rb");assert(in&&fread(f,sizeof(*f),1,in)==1&&fgetc(in)==EOF);fclose(in);
        assert(f->header[0]==0x4d535731&&f->header[2]==sizeof(*f));
        unsigned n=load(f->ls,0x16AA0);assert(n<=144);
        if(f->header[3]==0xDCBC)++captured;
        else {assert(f->header[3]==0x88D8);prepare(f,n);permutation(f,n);++derived;}
        compare(ref,native,f,captured+derived);
        memcpy(base,f,sizeof(*f));
        for(unsigned round=0;round<128;round++)for(unsigned order=0;order<2;order++) {
            unsigned use_native=order^(round&1);init(ref,f);
            uint64_t begin=now();
            if(use_native) {
                assert(ms_wws_sort_occluders(ref->ls,n));
#ifdef TEST_WHOLE_PHASE
                unsigned selected;assert(ms_wws_select_occluders(ref->ls,n,&selected));
                ref->gpr[10]=spu_il(selected);wws_sort_epilogue(ref);
#endif
            } else reference(ref);
            elapsed[use_native]+=now()-begin;
        }
        objects+=(uint64_t)n*128;
    }
    assert(captured+derived);closedir(dir);
    printf("PASS actual sort entries=%u; shuffled arrays derived from other captures=%u\n",captured,derived);
    printf("BENCH objects=%llu reference_ms=%.3f native_ms=%.3f speedup=%.3f (host sort only; not phone FPS)\n",
        (unsigned long long)objects,elapsed[0]/1.e6,elapsed[1]/1.e6,(double)elapsed[0]/elapsed[1]);
    guards(ref,base);
    for(unsigned trial=0;trial<11600;trial++) {
        memcpy(f,base,sizeof(*f));unsigned n=trial%145;prepare(f,n);permutation(f,n);
        for(unsigned i=0;i<144;i++) {
            unsigned kind=(trial/145)%8;uint32_t key;
            switch(kind) {
            case 0:key=rnd();break;
            case 1:key=0x3f000000+((rnd()%6)<<18);break;
            case 2:key=0x80000000;break;
            case 3:key=i%2?0:0x7fffff;break;
            case 4:key=0x7f800000+(rnd()%1024);break;
            case 5:key=0xff800000+(rnd()%1024);break;
            case 6:key=0x3f000000+i;put(f->ls,0x26CC0+4*i,i);break;
            default:key=0x3f000000-i;put(f->ls,0x26CC0+4*i,i);break;
            }
            put(f->ls,0x26A80+4*i,key);
        }
        compare(ref,native,f,1000+trial);
    }
#ifdef TEST_WHOLE_PHASE
    for(unsigned trial=0;trial<512;trial++) {
        memcpy(f,base,sizeof(*f));unsigned n=trial%145;prepare(f,n);permutation(f,n);
        unsigned spheres=load(f->ls,0x16A90);
        for(unsigned i=0;i<144;i++) {
            put(f->ls,0x26A80+4*i,rnd());
            for(unsigned k=0;k<4;k++) {
                uint32_t b=bits(k==3?(1+rnd()%20)*0.5f:((int)(rnd()%100)-50)*0.25f);
                if((trial&3)==0&&!(i%11)) {
                    static const uint32_t edges[]={0,0x80000000,1,0x007fffff,0x00800000,0x80800000,0x7f7fffff,0x7f800000,0x7fffffff,0xffffffff};
                    b=edges[rnd()%10];
                }
                put(f->ls,spheres+16*i+4*k,b);
            }
            unsigned planes=rnd()%19;put(f->ls,0x1BAE0+304*i+288,planes);
            for(unsigned j=0;j<planes;j++)for(unsigned k=0;k<4;k++)
                put(f->ls,0x1BAE0+304*i+16*j+4*k,bits(((int)(rnd()%33)-16)*0.125f));
        }
        for(unsigned i=0;i<5;i++)for(unsigned k=0;k<4;k++)
            put(f->ls,0x168A0+16*i+4*k,bits(((int)(rnd()%33)-16)*(k==3?0.5f:0.0625f)));
        compare(ref,native,f,13000+trial);
    }
    for(unsigned trial=0;trial<5;trial++) {
        memcpy(f,base,sizeof(*f));prepare(f,1);
        switch(trial) {
        case 0:put(f->ls,0x16A90,0x26840);break;
        case 1:put(f->ls,0x16A90,0x3FFFF);break;
        case 2:put(f->ls,0x16370,0);break;
        case 3:put(f->ls,0x26CC0,144);break;
        case 4:put(f->ls,0x26CC0,0);put(f->ls,0x1BAE0+288,19);break;
        }
        init(ref,f);unsigned selected=999;
        assert(!ms_wws_select_occluders(ref->ls,1,&selected));assert(selected==999);
        assert(!memcmp(ref->ls,f->ls,SPU_LS_SIZE));
    }
    puts("PASS 512 whole-phase geometry edge cases; 5 selection guards with no mutation");
#endif
    printf("PASS %u complete sort comparisons including exact ties, extended values and all counts 0..144; fallback=%u\n",comparisons,rejections);
    free(f);free(base);free(ref);free(native);return 0;
}
