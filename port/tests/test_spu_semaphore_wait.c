#include "spu_semaphore_wait.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
/* Include production coherence to inspect registration under its own lock,
 * making before-sleep and after-timeout tests deterministic. */
#include "spu_coherency.c"
int g_spu_ls_watch_n = 0;
void spu_ls_watch_slow(uint32_t a, int b, const uint8_t* c, uint32_t d, uint32_t e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; assert(0); }
void spu_ch_wake(spu_context* c) { (void)c; }
static int lockstep;
int yz_lockstep_enabled(void) { return lockstep; }
#include "semaphore_loops.inc"

static uint32_t seed = 0x267815AF;
static uint32_t random32(void) { seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5; return seed; }
static void test_observer_guards(void)
{
    spu_context initial = {0};
    initial.image_id=507917221; initial.pc=0xAAE8;
    initial.gpr[1]._u32[0]=0x30000;
    initial.gpr[7]=spu_il(1000);
    initial.gpr[82]._u32[0]=initial.mfc_lsa=0x20000;
    initial.mfc_size=128;
    int* gates[] = {&g_spu_ls_probe,&g_spu_ls_watch_n,&g_spu_smc_watch};
    for (unsigned gate=0; gate<3; ++gate) {
        for (int value=-1; value<=1; ++value) {
            *gates[gate]=value;
            spu_context candidate=initial, expected=initial;
            int allowed=!SPU_LS_DIAGNOSTICS || value==0;
            assert(motorstorm_spu_semaphore_delay(&candidate)==allowed);
            if (allowed) expected.gpr[6]=spu_il(999);
            assert(!memcmp(&candidate,&expected,sizeof expected));
            assert(*gates[gate]==value); // Do not initialize or erase observers.
        }
        *gates[gate]=0;
    }
}
static void compare_loops(unsigned trial)
{
    spu_context a = {0}, b;
    uint8_t* ls1 = malloc(SPU_LS_SIZE), *ls2 = malloc(SPU_LS_SIZE);
    assert(ls1 && ls2);
    for (unsigned i=0; i<SPU_LS_SIZE; ++i) ls1[i] = (uint8_t)random32();
    for (unsigned r=0; r<128; ++r)
        for (unsigned lane=0; lane<4; ++lane) a.gpr[r]._u32[lane] = random32();
    a.ls=ls1; a.image_id=507917221; a.pc=0xAAE8;
    a.gpr[1]._u32[0] = 0x30000 + (random32()%4080)*16;
    a.gpr[6]=spu_il(0); a.gpr[7]=spu_il(1000); a.gpr[8]=spu_il(0);
    a.gpr[82]._u32[0]=a.mfc_lsa=0x20000; a.mfc_size=128;
    // Exercise original-loop fallback independently of the optimized entry.
    switch (trial % 8) {
    case 1: a.gpr[1]._u32[0] |= 4; break;
    case 2: a.gpr[6]._u32[1]=53; break;
    case 3: a.gpr[8]._u32[0]=random32(); break;
    case 4: a.gpr[7]._u32[2]=21; break;
    case 5: a.mfc_eah=1; break;
    case 6: lockstep=1; break;
    default: break;
    }
    b=a; b.ls=ls2; memcpy(ls2,ls1,SPU_LS_SIZE);
    reference_loop(&a); int mode=optimized_loop(&b);
    assert(mode == (trial%8==0 || trial%8==7 ? 1 : 0));
    assert(!memcmp(ls1,ls2,SPU_LS_SIZE));
    b.ls=ls1;
    assert(!memcmp(&a,&b,sizeof(a)));
    lockstep=0; free(ls1); free(ls2);
}
typedef struct { spu_context ctx; int result; atomic_int done; } task;
static void reserve(task* t, unsigned ea) {
    spu_lockline_lock();
    t->ctx.resv_ea=ea; t->ctx.resv_valid=1;
    t->ctx.event_status=0x80;
    spu_coh_reserve(&t->ctx,ea);
    spu_lockline_unlock();
}
static void* waiter(void* p) {
    task* t=p; t->result=spu_coh_wait_reservation(&t->ctx,500000);
    atomic_store(&t->done,1); return NULL;
}
static int registered(spu_context* ctx) {
    int found=0; spu_lockline_lock();
    for (unsigned s=0;s<COH_STRIPES;++s)
        for (spu_host_resv_waiter* w=s_host_waiters[s];w;w=w->next)
            if(w->ctx==ctx) found=1;
    spu_lockline_unlock(); return found;
}
static int any_registered(void) {
    for (unsigned s=0;s<COH_STRIPES;++s) if(s_host_waiters[s]) return 1;
    return 0;
}
static void await_registration(task* t) {
    struct timespec start,now; clock_gettime(CLOCK_MONOTONIC,&start);
    while(!registered(&t->ctx)) {
        clock_gettime(CLOCK_MONOTONIC,&now); assert(now.tv_sec-start.tv_sec < 3);
        assert(!atomic_load(&t->done)); sched_yield();
    }
}
static void notify(unsigned ea) {
    spu_lockline_lock(); spu_coh_notify_write(ea); spu_lockline_unlock();
}
int main(void) {
    setenv("MOTORSTORM_SPU_SEMAPHORE_WAIT","1",1);
    g_spu_ls_probe=g_spu_smc_watch=0;
    test_observer_guards();
#if !SPU_LS_DIAGNOSTICS
    // Actual production state: compiled-out initializers leave every gate -1.
    g_spu_ls_probe=g_spu_ls_watch_n=g_spu_smc_watch=-1;
#endif
    for(unsigned i=0;i<2000;++i) compare_loops(i);
    task t={0}; reserve(&t,0x1000);
    // Producer wins before waiter: return immediately, preserve existing bits.
    notify(0x1000); assert(spu_coh_wait_reservation(&t.ctx,500000)==0);
    assert(t.ctx.event_status==(0x80|SPU_EVENT_LR));
    reserve(&t,0x1000);
    spu_context before=t.ctx;
    assert(spu_coh_wait_reservation(&t.ctx,100)==2);
    assert(!memcmp(&before,&t.ctx,sizeof(before))); assert(!registered(&t.ctx));
    notify(0x1000); // late notify must not access an unlinked stack waiter
    spu_coh_unregister(&t.ctx);
    // Six independent sleepers, two sharing a line. Unrelated writes must
    // leave reservations intact; both notify variants must reach waiters.
    task tasks[6]={0}; pthread_t threads[6];
    for(unsigned i=0;i<6;++i) {
        reserve(&tasks[i],0x2000+(i/2)*128);
        assert(!pthread_create(&threads[i],NULL,waiter,&tasks[i]));
        await_registration(&tasks[i]);
    }
    notify(0x8000);
    for(unsigned i=0;i<6;++i) assert(!atomic_load(&tasks[i].done));
    spu_lockline_lock();
    spu_coh_notify_write_except(0x2000,&tasks[0].ctx);
    assert(tasks[0].ctx.resv_valid);
    spu_lockline_unlock();
    notify(0x2000); notify(0x2080); notify(0x2100);
    for(unsigned i=0;i<6;++i) {
        assert(!pthread_join(threads[i],NULL));
        assert(tasks[i].result==1 && !tasks[i].ctx.resv_valid);
        assert(tasks[i].ctx.event_status==(0x80|SPU_EVENT_LR));
        spu_coh_unregister(&tasks[i].ctx);
    }
    // Repeated race at the check-to-sleep boundary with real notification.
    for(unsigned i=0;i<1000;++i) {
        task race={0}; pthread_t th; reserve(&race,0x3000);
        assert(!pthread_create(&th,NULL,waiter,&race));
        if(i&1) await_registration(&race);
        notify(0x3000); assert(!pthread_join(th,NULL));
        assert(race.result==0 || race.result==1);
        assert(!registered(&race.ctx)); spu_coh_unregister(&race.ctx);
    }
    assert(!any_registered());
    puts("PASS: 2000 full-register/LS differential cases, fallback guards, 1000 wake races, timeout, late notify, six sleepers, self exclusion, preserved events");
}
