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
/* Same scoped SPU arithmetic, with one horizontal operand-range reduction
 * per instruction. Shifting left removes the sign without losing exponent
 * information: exponent zero is below 0x01000000, exponent 255 starts at
 * 0xff000000. Keep the exact extended/overflow fallback and positive zero. */
static inline __attribute__((always_inline)) uint32x4_t ms_audio_compact_normalize(uint32x4_t x) {
    return vandq_u32(x, vcgeq_u32(vshlq_n_u32(x, 1), vdupq_n_u32(0x01000000u)));
}
#define MS_AUDIO_COMPACT_BINARY(NAME, INSTRUCTION, OP) \
static inline __attribute__((always_inline)) u128 NAME(u128 a,u128 b,uint64_t old) { \
    uint32x4_t x=vld1q_u32(a._u32),y=vld1q_u32(b._u32),r; \
    uint32x4_t magnitudes=vmaxq_u32(vshlq_n_u32(x,1),vshlq_n_u32(y,1)); \
    if(vmaxvq_u32(magnitudes)>=0xff000000u)return ms_rtz_fallback(a,b,a,OP,old); \
    x=ms_audio_compact_normalize(x);y=ms_audio_compact_normalize(y); \
    __asm__ volatile(INSTRUCTION " %0.4s, %1.4s, %2.4s" : "=w"(r) : "w"(x),"w"(y)); \
    if(vmaxvq_u32(vshlq_n_u32(r,1))>=0xfefffffeu)return ms_rtz_fallback(a,b,a,OP,old); \
    return spu__nv_u128(ms_audio_compact_normalize(r)); \
}
MS_AUDIO_COMPACT_BINARY(ms_audio_compact_fa,"fadd",0)
MS_AUDIO_COMPACT_BINARY(ms_audio_compact_fm,"fmul",1)
#undef MS_AUDIO_COMPACT_BINARY
static inline __attribute__((always_inline)) u128 ms_audio_compact_fs(u128 a,u128 b,uint64_t old) {
    return ms_audio_compact_fa(a,spu__nv_u128(veorq_u32(vld1q_u32(b._u32),vdupq_n_u32(0x80000000u))),old);
}
static inline __attribute__((always_inline)) u128 ms_audio_compact_fma(u128 a,u128 b,u128 c,uint64_t old) {
    uint32x4_t x=vld1q_u32(a._u32),y=vld1q_u32(b._u32),r=vld1q_u32(c._u32);
    uint32x4_t magnitudes=vmaxq_u32(vmaxq_u32(vshlq_n_u32(x,1),vshlq_n_u32(y,1)),vshlq_n_u32(r,1));
    if(vmaxvq_u32(magnitudes)>=0xff000000u)return ms_rtz_fallback(a,b,c,2,old);
    x=ms_audio_compact_normalize(x);y=ms_audio_compact_normalize(y);r=ms_audio_compact_normalize(r);
    __asm__ volatile("fmla %0.4s, %1.4s, %2.4s" : "+w"(r) : "w"(x),"w"(y));
    if(vmaxvq_u32(vshlq_n_u32(r,1))>=0xfefffffeu)return ms_rtz_fallback(a,b,c,2,old);
    return spu__nv_u128(ms_audio_compact_normalize(r));
}
static inline __attribute__((always_inline)) u128 ms_audio_compact_fms(u128 a,u128 b,u128 c,uint64_t old) {
    return ms_audio_compact_fma(a,b,spu__nv_u128(veorq_u32(vld1q_u32(c._u32),vdupq_n_u32(0x80000000u))),old);
}
/* Used only after the generated whole-run input guard and exponent proof.
 * Every intermediate is <= 2**125, so extended/overflow fallback cannot be
 * needed. Preserve SPU input/output denormal and signed-zero handling, exact
 * operation order and RTZ. Hardware float operations stay explicit asm. */
#define MS_AUDIO_BOUNDED_BINARY(NAME, INSTRUCTION) \
static inline __attribute__((always_inline)) u128 NAME(u128 a,u128 b,uint64_t old) { \
    (void)old; \
    uint32x4_t x=ms_audio_compact_normalize(vld1q_u32(a._u32)); \
    uint32x4_t y=ms_audio_compact_normalize(vld1q_u32(b._u32)), r; \
    __asm__ volatile(INSTRUCTION " %0.4s, %1.4s, %2.4s" : "=w"(r) : "w"(x),"w"(y)); \
    return spu__nv_u128(ms_audio_compact_normalize(r)); \
}
MS_AUDIO_BOUNDED_BINARY(ms_audio_bounded_fa,"fadd")
MS_AUDIO_BOUNDED_BINARY(ms_audio_bounded_fm,"fmul")
#undef MS_AUDIO_BOUNDED_BINARY
static inline __attribute__((always_inline)) u128 ms_audio_bounded_fs(u128 a,u128 b,uint64_t old) {
    return ms_audio_bounded_fa(a,spu__nv_u128(veorq_u32(vld1q_u32(b._u32),vdupq_n_u32(0x80000000u))),old);
}
static inline __attribute__((always_inline)) u128 ms_audio_bounded_fma(u128 a,u128 b,u128 c,uint64_t old) {
    (void)old;
    uint32x4_t x=ms_audio_compact_normalize(vld1q_u32(a._u32));
    uint32x4_t y=ms_audio_compact_normalize(vld1q_u32(b._u32));
    uint32x4_t r=ms_audio_compact_normalize(vld1q_u32(c._u32));
    __asm__ volatile("fmla %0.4s, %1.4s, %2.4s" : "+w"(r) : "w"(x),"w"(y));
    return spu__nv_u128(ms_audio_compact_normalize(r));
}
static inline __attribute__((always_inline)) u128 ms_audio_bounded_fms(u128 a,u128 b,u128 c,uint64_t old) {
    return ms_audio_bounded_fma(a,b,spu__nv_u128(veorq_u32(vld1q_u32(c._u32),vdupq_n_u32(0x80000000u))),old);
}
static inline int ms_audio_worker_scope_safe(const spu_context* ctx) {
    /* LS helpers only touch bytes in this mode. Diagnostic callbacks and
     * another image/policy must never execute with the arithmetic FPCR. */
    return ctx->image_id == 286484578 && !ctx->policy_mode &&
           !g_spu_ls_probe && !g_spu_ls_watch_n && !g_spu_smc_watch;
}
/* Only inside a checked Multistream arithmetic scope. The wrapper has already
 * excluded policy-mode probes, LS watchpoints and self-modifying-code watches.
 * Preserve quadword alignment, 256 KiB wrapping and per-word byte order while
 * avoiding the same inactive diagnostic checks on every sample access. */
static inline __attribute__((always_inline)) u128 ms_audio_ls_read128(const spu_context* ctx, uint32_t lsa) {
    const uint8_t* p = ctx->ls + (lsa & (SPU_LS_MASK & ~15u));
    return spu__nv_u128(vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p))));
}
static inline __attribute__((always_inline)) void ms_audio_ls_write128(spu_context* ctx, uint32_t lsa, u128 val) {
    uint8_t* p = ctx->ls + (lsa & (SPU_LS_MASK & ~15u));
    vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_u32(vld1q_u32(val._u32))));
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
    ms_audio_merge_enabled = setting ? atoi(setting) : 3;
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
