#include "native_geometry.h"
#include "spu_float_scope.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

int g_spu_ls_watch_n, g_spu_hop_fast=1, ms_geometry_test_enabled;
unsigned ms_geometry_test_hits;
void* volatile g_pm_flow_ctx;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
void spu_indirect_branch(spu_context* ctx) { (void)ctx; abort(); }
#include "geometry.inc"

static uint64_t seed=0x94adcedad;
static uint32_t random32(void) {seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
static uint64_t fpcr(void) {uint64_t r;__asm__ volatile("mrs %0, fpcr":"=r"(r));return r;}
static double cpu(void) {struct timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static uint32_t bits(float v) {uint32_t r;memcpy(&r,&v,4);return r;}
static void put(spu_context* c,unsigned address,unsigned lane,uint32_t value)
{ value=SPU_BSWAP32(value);memcpy(c->ls+((address&0x3fff0)+4*lane),&value,4); }

static uint32_t bounded_value(void)
{
    unsigned r=random32();
    return (r&7)==0 ? 0 : (r&0x807fffffu)|((97+random32()%60)<<23);
}
static void check_kernel(void)
{
    ms_geometry_corners box;
    for(unsigned trial=0;trial<200000;++trial) {
        uint32_t data[3][8];u128 plane;
        for(unsigned l=0;l<4;++l)plane._u32[l]=bounded_value();
        assert(ms_geometry_in_range(ms_geometry_vec(plane)));
        for(unsigned i=0;i<8;++i)for(unsigned axis=0;axis<3;++axis)data[axis][i]=bounded_value();
        for(unsigned group=0;group<2;++group) {
            box.x[group]=vld1q_u32(data[0]+4*group);box.y[group]=vld1q_u32(data[1]+4*group);
            box.z[group]=vld1q_u32(data[2]+4*group);
        }
        int any=0,all=1;
        for(unsigned i=0;i<8;++i) {
            uint32_t d=spu__fm_lane(plane._u32[0],data[0][i]);
            d=spu__fma_lane(plane._u32[1],data[1][i],d,0,0);
            d=spu__fma_lane(plane._u32[2],data[2][i],d,0,0);
            d=spu__fa_lane(d,plane._u32[3]);
            unsigned nonzero=(d&0x7f800000u)!=0;
            any|=nonzero && (d>>31);all&=nonzero && !(d>>31);
        }
        assert(ms_geometry_plane(&box,plane,1)==any);
        assert(ms_geometry_plane(&box,plane,0)==all);
    }
    puts("PASS 200000 eight-corner kernels against integer SPU arithmetic across the entire admitted exponent range");
}

static void run(spu_context* ctx,int kind)
{
    g_spu_trampoline_fn=NULL;
    if(kind)jobmod_be896a6374ba_e12230_spu_func_00008380(ctx);
    else jobmod_be896a6374ba_e12230_spu_func_00008120(ctx);
    unsigned hops=0;
    while(g_spu_trampoline_fn && g_spu_trampoline_fn!=spu_indirect_branch) {
        if(!spu_hop_release_ok(ctx) || (ctx->int_enable && (ctx->event_status&ctx->event_mask)))break;
        assert(++hops<200);
        void(*fn)(spu_context*)=g_spu_trampoline_fn;g_spu_trampoline_fn=NULL;
        ++ctx->steps;fn(ctx);
    }
}

static void fixture(spu_context* c,unsigned n,unsigned distribution)
{
    memset(c,0,sizeof(*c));c->ls=c->ls_store;
    for(unsigned r=0;r<128;++r)for(unsigned l=0;l<4;++l)c->gpr[r]._u32[l]=random32();
    c->gpr[3]._u32[0]=0x500;c->gpr[4]._u32[0]=0x1000;c->gpr[5]._u32[0]=n;
    c->gpr[0]._u32[0]=0x9644;c->host_depth=1;c->pc=0x9618;
    c->image_id=205;c->policy_mode=1;c->status=SPU_STATUS_RUNNING;c->steps=random32();
    const uint32_t shuffle[4]={0x00010203,0x04050607,0x08090a0b,0x10111213};
    const uint32_t edges[]={0,0x80000000,1,0x007fffff,0x00800000,0x307fffff,0x30800000,
        0x4e7fffff,0x4e800000,0x7f7fffff,0x7f800000,0xffffffff};
    for(unsigned l=0;l<4;++l)put(c,0x16370,l,shuffle[l]);
    for(unsigned i=0;i<4+n;++i)for(unsigned l=0;l<4;++l) {
        uint32_t v=bits((int)(random32()%33)-16);
        if(distribution==1)v=random32();
        if(distribution==2)v=edges[random32()%(sizeof(edges)/sizeof(*edges))];
        put(c,i<4?0x500+16*i:0x1000+16*(i-4),l,v);
    }
}

int main(int argc, char** argv)
{
    const uint64_t host=fpcr();
    spu_context *input=malloc(sizeof(*input)),*a=malloc(sizeof(*a)),*b=malloc(sizeof(*b));
    uint8_t *memory=malloc(SPU_LS_SIZE), *expected=malloc(SPU_LS_SIZE);
    assert(input&&a&&b&&memory&&expected);
    ms_rtz_leave((host&~0xc00000ull)|0xc00000ull);check_kernel();
    unsigned checks=0,hits[2]={0,0};
    const unsigned counts[]={0,1,2,5,8,32,64,65};
    for(unsigned test=0;test<16000;++test) {
        unsigned n=counts[test%8];fixture(input,n,(test/8)%3);
        if((test/768)%2) { input->image_id=900;input->resident_ovl=205; }
        uint64_t mode=(host&~0xc00000ull)|((uint64_t)((test/24)%4)<<22);
        switch((test/96)%8) {
        case 1:input->image_id=204;break;
        case 2:input->policy_mode=0;break;
        case 3:input->int_enable=1;input->event_mask=input->event_status=1;break;
        case 4:g_spu_hop_fast=0;break;
        default:break;
        }
        input->host_depth=(test/768)%2;
        if(test%97==0) {
            /* Misaligned addresses and LS wrap at both read-only inputs. */
            uint8_t box[64],planes[65][16];
            memcpy(box,input->ls+0x500,64);memcpy(planes,input->ls+0x1000,16*n);
            input->gpr[3]._u32[0]=0x7fff9;input->gpr[4]._u32[0]=0x3ffe7;
            /* These spans overlap, so populate them in one shared LS exactly
             * as the runtime sees them; the original is the oracle. */
            for(unsigned i=0;i<4;++i)memcpy(input->ls+((0x7fff9+16*i)&0x3fff0),box+16*i,16);
            for(unsigned i=0;i<n;++i)memcpy(input->ls+((0x3ffe7+16*i)&0x3fff0),planes[i],16);
        }
        for(unsigned kind=0;kind<2;++kind) {
            memcpy(a,input,sizeof(*a));memcpy(b,input,sizeof(*b));
            /* Shared read-only LS pointer makes whole-context comparison valid. */
            a->ls=b->ls=memory;
            memcpy(memory,input->ls,SPU_LS_SIZE);
            g_pm_flow_ctx=(test/96)%8==5?a:NULL;
            ms_geometry_test_enabled=0;ms_rtz_leave(mode);run(a,kind);
            memcpy(expected,memory,SPU_LS_SIZE);
            void(*pending)(spu_context*)=g_spu_trampoline_fn;
            g_pm_flow_ctx=(test/96)%8==5?b:NULL;
            unsigned before=ms_geometry_test_hits;
            memcpy(memory,input->ls,SPU_LS_SIZE);
            ms_geometry_test_enabled=1;ms_rtz_leave(mode);run(b,kind);
            hits[kind]+=ms_geometry_test_hits-before;
            if(memcmp(a,b,sizeof(*a)) || memcmp(expected,memory,SPU_LS_SIZE) || pending!=g_spu_trampoline_fn || fpcr()!=mode) {
                fprintf(stderr,"FAIL test=%u kind=%u n=%u mode=%llx steps=%u/%u pc=%x/%x\n",test,kind,n,
                    (unsigned long long)mode,a->steps,b->steps,(unsigned)a->pc,(unsigned)b->pc);
                for(unsigned r=0;r<128;++r)if(memcmp(a->gpr+r,b->gpr+r,16)) {
                    fprintf(stderr," r%u",r);
                    for(unsigned l=0;l<4;++l)fprintf(stderr," %08x/%08x",a->gpr[r]._u32[l],b->gpr[r]._u32[l]);
                    fputc('\n',stderr);
                }
                return 1;
            }
            ++checks;
        }
        g_spu_hop_fast=1;g_pm_flow_ctx=NULL;
    }
    assert(hits[0]>100&&hits[1]>100);
    printf("PASS %u complete-call context/LS/exit/FPCR checks; native hits=%u/%u; edge inputs, counts and fallback guards\n",checks,hits[0],hits[1]);fflush(stdout);
    ms_rtz_leave((host&~0xc00000ull)|0xc00000ull);
    for(unsigned guard=0;guard<2;++guard)for(unsigned kind=0;kind<2;++kind) {
        fixture(input,5,0);
        if(guard)input->host_depth=2000;
        g_spu_trampoline_fn=guard?NULL:spu_indirect_branch;
        memcpy(a,input,sizeof(*a));
        const unsigned before=ms_geometry_test_hits;
        assert(!ms_geometry_try(input,kind));
        assert(!memcmp(a,input,sizeof(*a))&&before==ms_geometry_test_hits);
        assert(g_spu_trampoline_fn==(guard?NULL:spu_indirect_branch));
    }
    g_spu_trampoline_fn=NULL;
    puts("PASS pending-transfer and recursion-limit guards leave the full context untouched for original dispatch");
    const int owners[][3]={{205,0,1},{205,205,1},{900,205,1},{900,204,0},{205,204,0},{900,0,0},{204,205,0}};
    for(unsigned owner=0;owner<sizeof(owners)/sizeof(*owners);++owner)for(unsigned kind=0;kind<2;++kind) {
        fixture(input,5,0);
        input->image_id=owners[owner][0];input->resident_ovl=owners[owner][1];
        for(unsigned i=0;i<4;++i)for(unsigned l=0;l<4;++l)put(input,0x500+16*i,l,bits(l==i?1:0));
        for(unsigned i=0;i<5;++i)for(unsigned l=0;l<4;++l)put(input,0x1000+16*i,l,bits(l==3?100:0));
        memcpy(a,input,sizeof(*a));memcpy(b,input,sizeof(*b));a->ls=b->ls=memory;
        memcpy(memory,input->ls,SPU_LS_SIZE);ms_geometry_test_enabled=0;run(a,kind);
        void(*pending)(spu_context*)=g_spu_trampoline_fn;
        memcpy(expected,memory,SPU_LS_SIZE);
        const unsigned before=ms_geometry_test_hits;
        memcpy(memory,input->ls,SPU_LS_SIZE);ms_geometry_test_enabled=1;run(b,kind);
        assert(ms_geometry_test_hits-before==(unsigned)owners[owner][2]);
        assert(!memcmp(a,b,sizeof(*a))&&!memcmp(expected,memory,SPU_LS_SIZE)&&pending==g_spu_trampoline_fn);
        if(owners[owner][0]==900 && owners[owner][1]==205 &&
           getenv("MOTORSTORM_GEOMETRY_SELF_CAPTURE")) ms_geometry_capture(input,kind);
    }
    puts("PASS full-call direct/resident-overlay activation and unrelated-owner rejection");
    if(argc>1) {
        unsigned fixtures=0,native=0;
        double times[2]={0,0};
        for(unsigned kind=0;kind<2;++kind)for(unsigned slot=0;slot<32;++slot) {
            char path[4096];snprintf(path,sizeof(path),"%s/geometry-%u-%02u.bin",argv[1],kind,slot);
            FILE* file=fopen(path,"rb");if(!file)continue;
            ms_geometry_fixture f;assert(fread(&f,sizeof(f),1,file)==1&&fgetc(file)==EOF);fclose(file);
            const uint32_t* h=f.header;assert(h[0]==0x4d534731&&(h[1]==1||h[1]==2)&&h[2]==kind&&h[3]>0&&h[3]<=64);
            memset(input,0,sizeof(*input));input->ls=input->ls_store;memcpy(input->gpr,f.gpr,sizeof(f.gpr));
            input->pc=h[4];input->steps=h[5];input->image_id=h[6];input->policy_mode=h[7];input->host_depth=h[8];
            input->int_enable=h[9];input->event_mask=h[10];input->status=h[11];input->event_status=h[12];
            input->resident_ovl=h[1]>=2?(int)h[15]:0;
            for(unsigned i=0;i<4;++i)memcpy(input->ls+((f.gpr[3]._u32[0]+16*i)&0x3fff0),f.box+16*i,16);
            memcpy(input->ls+0x16370,f.shuffle,16);
            for(unsigned i=0;i<h[3];++i)memcpy(input->ls+((f.gpr[4]._u32[0]+16*i)&0x3fff0),f.planes[i],16);
            const uint64_t mode=h[13]|((uint64_t)h[14]<<32);
            memcpy(a,input,sizeof(*a));memcpy(b,input,sizeof(*b));a->ls=b->ls=memory;
            memcpy(memory,input->ls,SPU_LS_SIZE);ms_rtz_leave(mode);ms_geometry_test_enabled=0;run(a,kind);
            void(*pending)(spu_context*)=g_spu_trampoline_fn;memcpy(expected,memory,SPU_LS_SIZE);
            memcpy(memory,input->ls,SPU_LS_SIZE);ms_rtz_leave(mode);ms_geometry_test_enabled=1;
            unsigned before=ms_geometry_test_hits;run(b,kind);native+=ms_geometry_test_hits-before;
            assert(!memcmp(a,b,sizeof(*a))&&!memcmp(expected,memory,SPU_LS_SIZE)&&fpcr()==mode&&pending==g_spu_trampoline_fn);
            ++fixtures;
            for(unsigned round=0;round<6;++round)for(unsigned order=0;order<2;++order) {
                unsigned opt=order^(round&1);ms_geometry_test_enabled=opt;
                double start=cpu();
                for(unsigned i=0;i<10000;++i) {
                    memcpy(a->gpr,input->gpr,sizeof(a->gpr));a->ls=input->ls;a->pc=input->pc;a->steps=input->steps;
                    run(a,kind);
                }
                times[opt]+=cpu()-start;
            }
        }
        assert(fixtures>0);
        printf("CAPTURE %u complete calls matched, %u native; original %.1f ns native %.1f ns speedup %.2fx (host component, not FPS)\n",
            fixtures,native,times[0]*1e9/(fixtures*60000),times[1]*1e9/(fixtures*60000),times[0]/times[1]);
    }
    ms_rtz_leave((host&~0xc00000ull)|0xc00000ull);
    for(unsigned kind=0;kind<2;++kind)for(unsigned scenario=0;scenario<4;++scenario) {
        unsigned n=scenario==3?32:5;fixture(input,n,0);
        for(unsigned i=0;i<4;++i)for(unsigned l=0;l<4;++l)put(input,0x500+16*i,l,bits(l==i?1:0));
        for(unsigned i=0;i<n;++i)for(unsigned l=0;l<4;++l) {
            int trigger=scenario==0?i==0:scenario==1?i==n-1:0;
            float d=kind?(trigger?-100:100):(trigger?100:-100);
            put(input,0x1000+16*i,l,bits(l==3?d:0));
        }
        double times[2]={0,0};volatile unsigned checksum=0;
        for(unsigned round=0;round<6;++round)for(unsigned order=0;order<2;++order) {
            unsigned opt=order^(round&1);ms_geometry_test_enabled=opt;
            double start=cpu();
            for(unsigned i=0;i<50000;++i) {
                memcpy(a->gpr,input->gpr,sizeof(a->gpr));a->ls=input->ls;a->image_id=205;
                a->policy_mode=1;a->host_depth=1;a->int_enable=0;a->pc=0x9618;a->steps=0;
                run(a,kind);checksum+=a->gpr[3]._u32[0];
            }
            times[opt]+=cpu()-start;
        }
        printf("BENCH kind=%u scenario=%u planes=%u original=%.1f ns native=%.1f ns speedup=%.2fx checksum=%u\n",
            kind,scenario,n,times[0]*1e9/300000,times[1]*1e9/300000,times[0]/times[1],checksum);
    }
    ms_rtz_leave(host);free(input);free(a);free(b);free(memory);free(expected);return 0;
}
