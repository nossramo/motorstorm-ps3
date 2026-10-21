#include "../../vendor/ps3recomp/include/ps3emu/passive_threads.h"
/* A C producer must share the registry and TLS destructor with ObjC++ logger. */
void offline_test_cpu_worker(int role)
{
    ps3_metrics_thread_role((enum ps3_cpu_role)role);
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
    uint64_t start=(uint64_t)t.tv_sec*1000000000ull+t.tv_nsec, now=start;
    volatile unsigned value=1;
    while (now-start<5000000) {
        for (unsigned i=0;i<10000;i++) value=value*1664525u+1013904223u;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
        now=(uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;
    }
}
