/* Differential tests of the actual hash-checked game arithmetic segments.
 * Synthetic register fixtures, not a gameplay FPS or battery benchmark. */
#include "spu_job_scope.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "job_arithmetic.inc"

#ifdef MOTORSTORM_JOB_BENCHMARK
static FILE* job_report;
#define printf(...) fprintf(job_report, __VA_ARGS__)
#define puts(s) fprintf(job_report, "%s\n", s)
#define main motorstorm_job_benchmark_main
#endif

static uint64_t seed = 0x3b59f15c64;
static uint32_t random32(void) {
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    return (uint32_t)seed;
}
static uint64_t fpcr(void) {
    uint64_t r; __asm__ volatile("mrs %0, fpcr" : "=r"(r)); return r;
}
static double cpu(void) {
    struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void fill(spu_context* c, unsigned distribution) {
    static const uint32_t edges[] = {0,0x80000000,1,0x007fffff,0x00800000,
        0x00800001,0x3f800000,0x3f800001,0x7f7ffffe,0x7f7fffff,0x7f800000,
        0x7fffffff,0xff800000,0xffffffff};
    for(unsigned r=0;r<128;r++) for(unsigned l=0;l<4;l++) {
        uint32_t bits = random32();
        if(distribution==1) bits=(bits&0x807fffff)|((120+random32()%15)<<23);
        if(distribution==2) bits=edges[random32()%(sizeof(edges)/sizeof(*edges))];
        c->gpr[r]._u32[l]=bits;
    }
}
int main(int argc, char** argv) {
#ifdef MOTORSTORM_JOB_BENCHMARK
    if(argc!=3 || !(job_report=fopen(argv[2],"w")))return 2;
    setvbuf(job_report,NULL,_IOLBF,0);
#endif
    const unsigned count=sizeof(job_cases)/sizeof(*job_cases);
    spu_context* a=calloc(1,sizeof(*a));
    spu_context* b=calloc(1,sizeof(*b));
    spu_context* input=calloc(32,sizeof(*input));
    assert(a&&b&&input);
    const uint64_t host=fpcr(), nearest=host&~0xc00000ull;
    for(unsigned n=0;n<1000000;n++) {
        u128 x,y;
        for(unsigned l=0;l<4;l++) {x._u32[l]=random32();y._u32[l]=random32();}
        u128 expected=spu_fi(x,y),actual=ms_job_fi(x,y);
        assert(!memcmp(&expected,&actual,sizeof(expected)));
        expected=spu_fcgt(x,y);actual=ms_cull_fcgt(x,y);
        assert(!memcmp(&expected,&actual,sizeof(expected)));
    }
    puts("PASS 4,000,000 arbitrary interpolation and comparison lanes");
    for(unsigned c=0;c<count;c++) {
        for(unsigned n=0;n<30000;n++) {
            fill(input,n%3); memcpy(a,input,sizeof(*a)); memcpy(b,input,sizeof(*b));
            const uint64_t caller=nearest|((uint64_t)(n%4)<<22);
            ms_rtz_leave(caller); job_cases[c][0](a);
            ms_rtz_leave(caller); job_cases[c][1](b);
            if(fpcr()!=caller || memcmp(a,b,sizeof(*a))) {
                fprintf(stderr,"FAIL case=%u iteration=%u FPCR=%llx/%llx\n",c,n,
                        (unsigned long long)fpcr(),(unsigned long long)caller);
                for(unsigned r=0;r<128;r++)for(unsigned l=0;l<4;l++)
                    if(a->gpr[r]._u32[l]!=b->gpr[r]._u32[l])
                        fprintf(stderr," r%u[%u] expected=%08x actual=%08x\n",r,l,a->gpr[r]._u32[l],b->gpr[r]._u32[l]);
                return 1;
            }
        }
        printf("PASS case=%u contexts=30000 including four caller rounding modes\n",c);
    }
    ms_rtz_leave(nearest);
    for(unsigned n=0;n<32;n++)fill(input+n,1);
    volatile uint32_t checksum=0;
    for(unsigned c=0;c<count;c++) {
        if(argc>1) {
            for(unsigned n=0;n<32;n++) {
                char path[4096];snprintf(path,sizeof(path),"%s/segment-%u-%02u.bin",argv[1],c,n);
                FILE* f=fopen(path,"rb");assert(f);
                assert(fread(input[n].gpr,sizeof(input[n].gpr),1,f)==1);
                assert(fgetc(f)==EOF);fclose(f);
                for(unsigned mode=0;mode<4;mode++) {
                    memcpy(a,input+n,sizeof(*a));memcpy(b,input+n,sizeof(*b));
                    uint64_t caller=nearest|((uint64_t)mode<<22);
                    ms_rtz_leave(caller);job_cases[c][0](a);
                    ms_rtz_leave(caller);job_cases[c][1](b);
                    assert(fpcr()==caller && !memcmp(a,b,sizeof(*a)));
                }
            }
            ms_rtz_leave(nearest);
            printf("CAPTURE case=%u actual_register_snapshots=32 all rounding modes match\n",c);
        }
        double times[2]={0,0};
        for(unsigned trial=0;trial<6;trial++)for(unsigned order=0;order<2;order++) {
            unsigned mode=(trial+order)%2;
            double begin=cpu();
            for(unsigned n=0;n<100000;n++) {
                memcpy(a->gpr,input[n%32].gpr,sizeof(a->gpr));
                job_cases[c][mode](a);
                checksum^=a->gpr[n%128]._u32[n%4];
            }
            times[mode]+=cpu()-begin;
        }
        printf("BENCH case=%u reference_ns=%.3f scope_ns=%.3f saving_percent=%.2f\n",
               c,times[0]*1e9/600000,times[1]*1e9/600000,100*(1-times[1]/times[0]));
    }
    ms_rtz_leave(host);
    printf("PASS %u segments; checksum=%u; no LS/host/guest calls within scope\n",count,checksum);
    free(a);free(b);free(input);
#ifdef MOTORSTORM_JOB_BENCHMARK
    fclose(job_report);
#endif
    return 0;
}
