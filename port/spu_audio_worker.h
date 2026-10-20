#pragma once
#include "spu_audio_conversions.h"
#include "spu_float_scope.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

static inline u128 ms_audio_rtz_fms(u128 a, u128 b, u128 c, uint64_t old) {
    uint32x4_t negative_c = veorq_u32(vld1q_u32(c._u32), vdupq_n_u32(0x80000000));
    return ms_rtz_fma(a, b, spu__nv_u128(negative_c), old);
}
static inline int ms_audio_worker_scope_safe(const spu_context* ctx) {
    /* LS helpers only touch bytes in this mode. Diagnostic callbacks and
     * another image/policy must never execute with the arithmetic FPCR. */
    return ctx->image_id == 286484578 && !ctx->policy_mode &&
           !g_spu_ls_probe && !g_spu_ls_watch_n && !g_spu_smc_watch;
}
#ifdef MOTORSTORM_AUDIO_WORKER_TEST
extern int ms_audio_worker_test_mode;
static inline int ms_audio_worker_mode(void) { return ms_audio_worker_test_mode; }
static inline void ms_audio_worker_capture(spu_context* ctx, unsigned pc, unsigned slot) {}
#else
/* Initialized before guest worker creation; no getenv/locking in the hot path. */
static int ms_audio_merge_enabled;
static int ms_audio_timing_enabled;
static int ms_audio_qos_enabled;
static const char* ms_audio_capture_dir;
__attribute__((constructor)) static void ms_audio_worker_init(void) {
    const char* setting = getenv("MOTORSTORM_AUDIO_MERGE");
    ms_audio_merge_enabled = setting ? atoi(setting) : 2;
    ms_audio_capture_dir = getenv("MOTORSTORM_AUDIO_WORKER_CAPTURE");
    ms_audio_timing_enabled = getenv("MOTORSTORM_AUDIO_WORKER_TIMING") != NULL;
    setting = getenv("MOTORSTORM_AUDIO_QOS");
    ms_audio_qos_enabled = setting ? atoi(setting) != 0 : 1;
}
static inline int ms_audio_worker_mode(void) { return ms_audio_merge_enabled; }
static inline void ms_audio_worker_host_setup(const spu_context* ctx) {
#if defined(__APPLE__)
    static SPU_THREAD_LOCAL int configured;
    if (!ms_audio_qos_enabled || configured || ctx->image_id != 286484578 || ctx->policy_mode) return;
    configured = 1;
    /* This verified Multistream task has its own async thread, which exits
     * with the task (not a reusable job-pool thread). Its 256-sample output
     * deadline is 5.33 ms. Change host QoS once, leaving guest priorities and
     * core selection alone. Called before any scoped guest arithmetic. */
    qos_class_t before = qos_class_self();
    int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    fprintf(stderr, "[audio-worker-qos] before=%u actual=%u rc=%d\n",
            (unsigned)before, (unsigned)qos_class_self(), rc);
#else
    (void)ctx;
#endif
}
static void ms_audio_worker_timing(void) {
    static SPU_THREAD_LOCAL unsigned calls;
    if ((++calls & 127) != 0) return;
    static SPU_THREAD_LOCAL double last_wall, last_cpu;
    struct timespec wall, cpu;
    clock_gettime(CLOCK_MONOTONIC, &wall);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    double w = wall.tv_sec + wall.tv_nsec * 1e-9;
    double c = cpu.tv_sec + cpu.tv_nsec * 1e-9;
    if (!last_wall) {
#if defined(__APPLE__)
        fprintf(stderr, "[audio-worker-time] qos=%u mode=%d\n",
                (unsigned)qos_class_self(), ms_audio_merge_enabled);
#endif
        last_wall = w; last_cpu = c; calls = 0; return;
    }
    if (w - last_wall >= 1) {
        fprintf(stderr, "[audio-worker-time] wall_ms=%.2f cpu_ms=%.2f calls=%u\n",
                (w-last_wall)*1000, (c-last_cpu)*1000, calls);
        last_wall = w; last_cpu = c; calls = 0;
    }
}
static void ms_audio_worker_capture(spu_context* ctx, unsigned pc, unsigned slot) {
    ms_audio_worker_host_setup(ctx);
    if (ms_audio_timing_enabled) ms_audio_worker_timing();
    if (!ms_audio_capture_dir) return;
    static SPU_THREAD_LOCAL unsigned counts[4];
    if (slot >= 4 || counts[slot] >= 8) return;
    char path[1024];
    int length = snprintf(path, sizeof path, "%s/%05X-%u-before.bin", ms_audio_capture_dir, pc, counts[slot]++);
    if (length < 0 || (size_t)length >= sizeof path) return;
    FILE* file = fopen(path, "wb");
    if (!file) { perror("audio worker capture"); return; }
    /* Keep large capture storage out of TLS: every game job uses TLS, even
     * with capture disabled, and should not inherit a 256 KiB audio fixture. */
    spu_context* copy = malloc(sizeof *copy);
    if (!copy) { fclose(file); return; }
    *copy = *ctx;
    memcpy(copy->ls_store, ctx->ls, SPU_LS_SIZE);
    copy->pc = pc;
    if (fwrite(copy, sizeof *copy, 1, file) != 1) perror("audio worker capture write");
    free(copy);
    fclose(file);
}
#endif
