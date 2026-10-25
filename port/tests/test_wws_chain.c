#include "wws_native_chain.h"
#include "spu_thread_rtz.h"
#include "spu_float_scope.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <time.h>

int g_spu_ls_watch_n, g_spu_hop_fast=1;
void* volatile g_pm_flow_ctx;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
static unsigned stop_pc, hops;
static void replay_stop(spu_context* ctx, unsigned pc) { (void)ctx; stop_pc=pc; }
void spu_indirect_branch(spu_context* ctx) { fprintf(stderr,"unexpected indirect %x\n",(unsigned)ctx->pc);abort(); }
void spu_img_restore(spu_context* ctx,int32_t image) { ctx->image_id=image; }
void spu_drain_call(spu_context* ctx,uint32_t ret) {
    while(g_spu_trampoline_fn && (uint32_t)ctx->pc!=ret && !stop_pc) {
        assert(++hops<1000000);
        void (*fn)(spu_context*)=g_spu_trampoline_fn;g_spu_trampoline_fn=NULL;
        ++ctx->steps;fn(ctx);
    }
    assert(!stop_pc);g_spu_trampoline_fn=NULL;
}
#include "wws_reference.inc"

static void init(spu_context* c,const ms_wws_chain_fixture* f) {
    memset(c,0,sizeof(*c));c->ls=c->ls_store;
    memcpy(c->gpr,f->gpr,sizeof(f->gpr));memcpy(c->ls,f->ls,SPU_LS_SIZE);
    c->pc=f->header[4];c->steps=f->header[5];c->image_id=f->header[6];
    c->resident_ovl=f->header[7];c->policy_mode=f->header[8];c->host_depth=f->header[9];
    c->int_enable=f->header[10];c->event_mask=f->header[11];c->event_status=f->header[12];
    c->status=f->header[13];
    uint64_t mode=f->header[14]|((uint64_t)f->header[15]<<32);ms_rtz_leave(mode);
}
static void reference(spu_context* c) {
    stop_pc=hops=0;g_spu_trampoline_fn=NULL;
    jobmod_be896a6374ba_e12230_spu_func_000088D8(c);
    while(g_spu_trampoline_fn&&!stop_pc) {
        assert(++hops<1000000);
        void(*fn)(spu_context*)=g_spu_trampoline_fn;g_spu_trampoline_fn=NULL;++c->steps;fn(c);
    }
}
static uint32_t load(const uint8_t*p,unsigned a) {uint32_t x;memcpy(&x,p+a,4);return SPU_BSWAP32(x);}
static void put(uint8_t*p,unsigned a,uint32_t x) {x=SPU_BSWAP32(x);memcpy(p+a,&x,4);}
static uint32_t bits(float x) {uint32_t r;memcpy(&r,&x,4);return r;}
static uint64_t rng=0x789fa53;
static unsigned rnd(void) {rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (unsigned)rng;}
static uint64_t now(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC_RAW,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static uint64_t elapsed[2],bench_objects;
static void benchmark(spu_context*c,const ms_wws_chain_fixture*f) {
    uint8_t*memory=malloc(SPU_LS_SIZE);assert(memory);
    for(unsigned round=0;round<64;round++)for(unsigned order=0;order<2;order++) {
        unsigned native=order^(round&1);init(c,f);memcpy(memory,f->ls,SPU_LS_SIZE);
        uint64_t start=now();
        if(native) {c->ls=memory;assert(ms_wws_classify_batch(memory,f->gpr[83]._u32[0]));wws_epilogue(c);}
        else reference(c);
        elapsed[native]+=now()-start;
    }
    bench_objects+=(uint64_t)f->gpr[83]._u32[0]*64;free(memory);
}
static int compare(spu_context* c,const ms_wws_chain_fixture*f,unsigned tag) {
    uint8_t* native=malloc(SPU_LS_SIZE);assert(native);memcpy(native,f->ls,SPU_LS_SIZE);
    unsigned sp=f->gpr[1]._u32[0],n=f->gpr[83]._u32[0];
    init(c,f);reference(c);
    int accepted=ms_wws_classify_batch(native,n);
    if(!accepted) {assert(!memcmp(native,f->ls,SPU_LS_SIZE));free(native);return 0;}
    assert(stop_pc==0x8BE0);
    spu_context* finish=malloc(sizeof(*finish));assert(finish);init(finish,f);
    assert(ms_wws_chain_try(finish));assert(!memcmp(native,finish->ls,SPU_LS_SIZE));
    finish->ls=native;
    wws_epilogue(finish);
    /* SPU r11..r41 are volatile scratch in this void routine. The checked
     * caller contract verifies none of those incoming values is consumed.
     * Compare all other registers through the retained epilogue. */
    for(unsigned r=0;r<128;r++)if(r<11||r>41)assert(!memcmp(&c->gpr[r],&finish->gpr[r],sizeof(u128)));
    assert(c->pc==finish->pc);free(finish);
    unsigned differences=0;
    for(unsigned i=0;i<SPU_LS_SIZE;i++) {
        /* The old callee's two private stack temporaries die at its epilogue.
         * All other memory, including every lane of counters, must agree. */
        if((i>=sp+32&&i<sp+96)||(i>=sp-96&&i<sp-80))continue;
        if(native[i]!=c->ls[i]) {
            if(differences<12)fprintf(stderr,"DIFF case=%u address=%05x ref=%02x native=%02x\n",tag,i,c->ls[i],native[i]);
            ++differences;
        }
    }
    free(native);if(differences) {fprintf(stderr,"%u differing bytes (n=%u)\n",differences,n);return -1;}return 1;
}
static void guards(spu_context*c,const ms_wws_chain_fixture*f) {
    spu_context* before=malloc(sizeof(*before));assert(before);
    for(unsigned test=0;test<12;test++) {
        init(c,f);g_spu_trampoline_fn=NULL;
        switch(test) {
        case 0:c->image_id=209;break;
        case 1:c->resident_ovl=209;break;
        case 2:c->status=SPU_STATUS_STOPPED;break;
        case 3:c->event_status=c->event_mask=2;c->int_enable=1;break;
        case 4:c->irq_saved=1;break;
        case 5:c->gpr[1]=spu_il(0x30000);break;
        case 6:put(c->ls,c->gpr[1]._u32[0]+320,0x1234);break;
        case 7:c->gpr[83]=spu_il(257);break;
        case 8:put(c->ls,0x16770,4095);c->gpr[83]=spu_il(1);break;
        case 9:put(c->ls,0x16A30,0x16780);c->gpr[83]=spu_il(1);break;
        case 10:g_spu_hop_fast=0;break;
        case 11:c->host_depth=2000;break;
        }
        memcpy(before,c,sizeof(*c));assert(!ms_wws_chain_try(c));assert(!memcmp(before,c,sizeof(*c)));
        g_spu_hop_fast=1;
    }
    free(before);puts("PASS 12 unsupported-context/layout guards; no mutation");
}
static void synthetic(ms_wws_chain_fixture*f,unsigned trial) {
    uint8_t*p=f->ls;unsigned n=1+rnd()%256;f->gpr[83]=spu_il(n);
    put(p,0x16770,0);put(p,0x17AC0,0);put(p,0x17AD0,0);put(p,0x16AB0,0);put(p,0x265E0,0);
    put(p,0x16A30,0x2F020);put(p,0x16A60,0x30420);put(p,0x16A50,0x34420);put(p,0x16A40,0x35420);
    put(p,0x16780,0x27000);put(p,0x167B0,0x27020);
    put(p,0x16790,(trial&1)?0x2D020:0);put(p,0x167A0,(trial&2)?0x2B020:0);
    p[0x16810]=(trial>>2)&1;p[0x16920]=(trial>>3)&1;p[0x16820]=(trial>>4)&1;p[0x16830]=(trial>>7)&1;
    put(p,0x167C0,(trial>>5)%4);
    if(p[0x16830]) {
        unsigned occluders=1+rnd()%8;put(p,0x265E0,occluders);
        for(unsigned o=0;o<occluders;o++) {
            unsigned index=rnd()%144,base=0x1BAE0+304*index;
            put(p,0x265F0+4*o,index);unsigned planes=rnd()%19;put(p,base+288,planes);
            for(unsigned j=0;j<planes;j++)for(unsigned k=0;k<4;k++)
                put(p,base+16*j+4*k,bits(((int)(rnd()%65)-32)*(k==3?1.f:0.0625f)));
        }
    }
    unsigned records=load(p,0x16A30),matrices=load(p,0x16A60),spheres=load(p,0x16A50),boxes=load(p,0x16A40);
    for(unsigned i=0;i<n;i++) {
        unsigned record=records+20*i;put(p,record,bits((rnd()%16+1)*0.25f));put(p,record+4,bits(0.125f));
        put(p,record+8,0x70000+(rnd()%12));put(p,record+12,(rnd()%4)*2+1);
        if(i%9==0)put(p,record+12,0);
        for(unsigned col=0;col<4;col++)for(unsigned row=0;row<4;row++) {
            float v=col==row?1.f:0.f;if(col==3&&row<3)v=(int)(rnd()%100)-50;
            if((trial&256)&&col<3&&row<3)v=((int)(rnd()%33)-16)*0.125f;
            put(p,matrices+64*i+16*col+4*row,bits(v));
            float b=col==0?0.f:row==col-1?1.f:0.f;if(row==3)b=col?1.f:0.f;
            put(p,boxes+64*i+16*col+4*row,bits(b));
        }
        for(unsigned axis=0;axis<4;axis++)put(p,spheres+16*i+4*axis,bits(axis==3?2.f:(int)(rnd()%20)-10));
        if(trial&512) {
            static const uint32_t edge[]={0,0x80000000,1,0x007fffff,0x00800000,0x80800000,
                0x30000000,0x4f000001,0x7f7fffff,0x7f800000,0x7fffffff,0xffffffff};
            put(p,record,edge[rnd()%12]);put(p,record+4,edge[rnd()%12]);
            put(p,spheres+16*i+4*(rnd()%4),edge[rnd()%12]);
            put(p,matrices+64*i+4*(rnd()%16),edge[rnd()%12]);
        }
    }
}
int main(int argc,char**argv) {
    assert(argc==2);DIR*dir=opendir(argv[1]);assert(dir);
    ms_wws_chain_fixture* f=malloc(sizeof(*f));spu_context*c=malloc(sizeof(*c));
    struct dirent*ent;unsigned files=0,accepted=0;
    while((ent=readdir(dir))) {
        if(strncmp(ent->d_name,"wws-88d8-",9) || !strstr(ent->d_name,".bin"))continue;
        char path[4096];snprintf(path,sizeof(path),"%s/%s",argv[1],ent->d_name);
        FILE*in=fopen(path,"rb");assert(in&&fread(f,sizeof(*f),1,in)==1&&fgetc(in)==EOF);fclose(in);
        assert(f->header[0]==0x4d535731&&f->header[1]==1&&f->header[2]==sizeof(*f)&&f->header[3]==0x88D8);
        int result=compare(c,f,files);assert(result>=0);accepted+=result;
        if(result==1)benchmark(c,f);
        printf("%s objects=%u stop=%04x hops=%u native=%d\n",ent->d_name,f->gpr[83]._u32[0],stop_pc,hops,result);
        assert(stop_pc==0x8BE0||stop_pc==0x8F08);++files;
    }
    assert(files);printf("PASS %u captured whole-batch replays; native accepted %u\n",files,accepted);
    printf("BENCH objects=%llu reference_ms=%.3f native_ms=%.3f speedup=%.3f (host kernel only; not phone FPS)\n",
        (unsigned long long)bench_objects,elapsed[0]/1.e6,elapsed[1]/1.e6,(double)elapsed[0]/elapsed[1]);
    ms_wws_chain_fixture* base=malloc(sizeof(*base));assert(base);memcpy(base,f,sizeof(*base));
    guards(c,base);
    unsigned checks=0;
    for(unsigned trial=0;trial<768;trial++) {
        memcpy(f,base,sizeof(*f));synthetic(f,trial);
        int result=compare(c,f,1000+trial);if(result!=1)fprintf(stderr,"synthetic trial=%u rejected/result=%d\n",trial,result);assert(result==1);++checks;
    }
    printf("PASS %u synthetic complete-batch output comparisons\n",checks);free(base);
    free(f);free(c);closedir(dir);return 0;
}
