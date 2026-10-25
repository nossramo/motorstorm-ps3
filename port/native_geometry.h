#pragma once
/* iOS geometry kernels. The adapter is installed only in the signature-checked
 * WWS image 205. Unsupported inputs leave the context untouched for the lift.
 */
#include "spu_thread_rtz.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>

/* Compact, pointer-free diagnostic fixture. Raw LS quadwords retain guest
 * byte order. Normal builds record nothing unless the capture path is set.
 */
typedef struct ms_geometry_fixture {
    uint32_t header[16];
    u128 gpr[128];
    uint8_t box[64], shuffle[16], planes[64][16];
} ms_geometry_fixture;
_Static_assert(sizeof(ms_geometry_fixture)==3216,"geometry fixture layout");

typedef struct ms_geometry_corners {
    u128 corner[8];
    uint32x4_t x[2], y[2], z[2];
} ms_geometry_corners;

static inline uint32x4_t ms_geometry_vec(u128 v) { return vld1q_u32(v._u32); }
static inline u128 ms_geometry_u128(uint32x4_t v) { return spu__nv_u128(v); }
static inline uint32x4_t ms_geometry_zero(uint32x4_t v)
{ return vandq_u32(v, vcgtq_u32(vshlq_n_u32(v, 1), vdupq_n_u32(0))); }

/* Inputs are +0 or normal singles in [2^-30,2^30). Products and the
 * subsequent two FMAs and add stay below 2^62 on a 2^-106 lattice. Thus no
 * intermediate can overflow or become subnormal. Keep the original operation
 * order and hardware RTZ; signed intermediate zeros cannot affect nonzero
 * results, and visible zero results are canonicalized to SPU +0.
 */
static inline int ms_geometry_in_range(uint32x4_t v)
{
    uint32x4_t a = vandq_u32(v, vdupq_n_u32(0x7fffffffu));
    uint32x4_t valid = vorrq_u32(vceqq_u32(v, vdupq_n_u32(0)),
        vandq_u32(vcgeq_u32(a, vdupq_n_u32(0x30800000u)),
                  vcltq_u32(a, vdupq_n_u32(0x4e800000u))));
    return vminvq_u32(valid) != 0;
}
static inline __attribute__((always_inline)) uint32x4_t ms_geometry_mul(uint32x4_t a, uint32x4_t b)
{ uint32x4_t r; __asm__ volatile("fmul %0.4s, %1.4s, %2.4s":"=w"(r):"w"(a),"w"(b)); return r; }
static inline __attribute__((always_inline)) uint32x4_t ms_geometry_madd(uint32x4_t a, uint32x4_t b, uint32x4_t c)
{ __asm__ volatile("fmla %0.4s, %1.4s, %2.4s":"+w"(c):"w"(a),"w"(b)); return c; }
static inline __attribute__((always_inline)) uint32x4_t ms_geometry_add(uint32x4_t a, uint32x4_t b)
{ uint32x4_t r; __asm__ volatile("fadd %0.4s, %1.4s, %2.4s":"=w"(r):"w"(a),"w"(b)); return r; }
static inline uint32x4_t ms_geometry_dot(uint32x4_t x, uint32x4_t y, uint32x4_t z,
                                        uint32x4_t nx, uint32x4_t ny, uint32x4_t nz, uint32x4_t d)
{
    return ms_geometry_add(ms_geometry_madd(nz, z,
        ms_geometry_madd(ny, y, ms_geometry_mul(nx, x))), d);
}
static inline uint32x4_t ms_geometry_side(uint32x4_t d, int any_negative)
{
    float32x4_t v = vreinterpretq_f32_u32(d), zero = vdupq_n_f32(0);
    return any_negative ? vcltq_f32(v, zero) : vcgtq_f32(v, zero);
}

/* Two NEON vectors test eight different corners, rather than calculating four
 * rotated dot products for each corner. Only the final plane needs those
 * scratch results; the adapter reconstructs them once at the call boundary.
 */
static inline int ms_geometry_plane(const ms_geometry_corners* b, u128 plane, int any_negative)
{
    uint32x4_t nx=vdupq_n_u32(plane._u32[0]), ny=vdupq_n_u32(plane._u32[1]);
    uint32x4_t nz=vdupq_n_u32(plane._u32[2]), d=vdupq_n_u32(plane._u32[3]);
    uint32x4_t p = ms_geometry_side(ms_geometry_dot(b->x[0],b->y[0],b->z[0],nx,ny,nz,d),any_negative);
    uint32x4_t q = ms_geometry_side(ms_geometry_dot(b->x[1],b->y[1],b->z[1],nx,ny,nz,d),any_negative);
    return any_negative ? vmaxvq_u32(vorrq_u32(p,q)) != 0 : vminvq_u32(vandq_u32(p,q)) != 0;
}

static inline unsigned ms_geometry_mode(void)
{
#ifdef MOTORSTORM_GEOMETRY_TEST
    extern int ms_geometry_test_enabled;
    return ms_geometry_test_enabled;
#else
    static atomic_int cached = -1;
    int value=atomic_load_explicit(&cached,memory_order_relaxed);
    if(value<0) {
        const char* s=getenv("MOTORSTORM_NATIVE_GEOMETRY");
        value=s && s[0]=='1' && s[1]=='\0';
        if(getenv("MOTORSTORM_CULL_PROFILE") || getenv("SPU_PCHIST") ||
           getenv("SPU_HOST_DEPTH_MAX")) value=0;
        if(getenv("MOTORSTORM_GEOMETRY_CAPTURE")) value|=2;
        atomic_store_explicit(&cached,value,memory_order_relaxed);
    }
    return value;
#endif
}

static inline int ms_geometry_image(const spu_context* ctx)
{
    /* WWS keeps image_id=900 while dispatching the signature-matched overlay
     * through resident_ovl. The base image is not the current code owner.
     * Retain direct-image support for isolated calls without another overlay.
     */
    return (ctx->image_id==900 && ctx->resident_ovl==205) ||
        (ctx->image_id==205 && (!ctx->resident_ovl || ctx->resident_ovl==205));
}

static void ms_geometry_capture(const spu_context* ctx, unsigned kind)
{
    static atomic_uint calls[2], saved[2];
    if(kind>1 || !ms_geometry_image(ctx) || !ctx->gpr[5]._u32[0] || ctx->gpr[5]._u32[0]>64) return;
    if(atomic_load_explicit(&saved[kind],memory_order_relaxed)>=32) return;
    unsigned call=atomic_fetch_add_explicit(&calls[kind],1,memory_order_relaxed);
    if(call>=4 && (call&1023u)) return;
    unsigned slot=atomic_fetch_add_explicit(&saved[kind],1,memory_order_relaxed);
    if(slot>=32) return;
    const char* dir=getenv("MOTORSTORM_GEOMETRY_CAPTURE");
    if(!dir || !*dir) return;
    ms_geometry_fixture f={0};
    uint64_t mode;__asm__ volatile("mrs %0, fpcr":"=r"(mode));
    const uint32_t h[16]={0x4d534731,2,kind,ctx->gpr[5]._u32[0],(uint32_t)ctx->pc,ctx->steps,
        ctx->image_id,ctx->policy_mode,ctx->host_depth,ctx->int_enable,ctx->event_mask,
        ctx->status,ctx->event_status,(uint32_t)mode,(uint32_t)(mode>>32),(uint32_t)ctx->resident_ovl};
    memcpy(f.header,h,sizeof(h));memcpy(f.gpr,ctx->gpr,sizeof(f.gpr));
    for(unsigned i=0;i<4;++i) memcpy(f.box+16*i,ctx->ls+((ctx->gpr[3]._u32[0]+16*i)&0x3fff0),16);
    memcpy(f.shuffle,ctx->ls+0x16370,16);
    for(unsigned i=0;i<h[3];++i)memcpy(f.planes[i],ctx->ls+((ctx->gpr[4]._u32[0]+16*i)&0x3fff0),16);
    char path[4096];
    int length=snprintf(path,sizeof(path),"%s/geometry-%u-%02u.bin",dir,kind,slot);
    if(length<0 || (size_t)length>=sizeof(path)) return;
    mkdir(dir,0700);
    FILE* out=fopen(path,"wb");
    if(out) { if(fwrite(&f,sizeof(f),1,out)!=1)perror("geometry capture");fclose(out); }
}

static inline int ms_geometry_safe(const spu_context* ctx)
{
#if SPU_LS_DIAGNOSTICS
    (void)ctx; return 0;
#else
    uint64_t mode; __asm__ volatile("mrs %0, fpcr":"=r"(mode));
    /* No intermediate drain may be skipped when a transfer is pending or
     * the default recursion guard would halt. Custom limits/tracing disable
     * the adapter in ms_geometry_mode so the original drains handle them.
     */
    return ms_geometry_image(ctx) && ctx->policy_mode && ctx->host_depth<2000 &&
        !g_spu_trampoline_fn && spu_hop_release_ok(ctx) &&
        !(ctx->int_enable && (ctx->event_status & ctx->event_mask)) &&
        (mode & 0xc00000ull)==0xc00000ull && !(mode & 0x9f02ull);
#endif
}

static __attribute__((noinline)) int ms_geometry_try(spu_context* ctx, int any_negative)
{
    const unsigned count=ctx->gpr[5]._u32[0];
    if(!ms_geometry_safe(ctx) || !count || count>64) return 0;
    /* This read-only leaf has no DMA, channel, guest call or LS writes. Keep
     * every guest register private until all range and observer checks pass.
     * LS addresses retain the original quadword alignment and wrap semantics.
     */
    u128 r[42];
    r[25]=ctx->gpr[5]; r[13]=ctx->gpr[4];
    r[6]=spu_ls_read128(ctx,ctx->gpr[3]._u32[0]+32);
    r[9]=spu_ls_read128(ctx,ctx->gpr[3]._u32[0]+16);
    r[7]=spu_ls_read128(ctx,ctx->gpr[3]._u32[0]+48);
    r[8]=spu_ls_read128(ctx,ctx->gpr[3]._u32[0]);
    r[10]=spu_ls_read128(ctx,0x16370);
    const u128 shuffle=spu_ila(0x10203), zero=spu_il(0), one=spu_ilhu(0x3f80);
    u128 sx=spu_shufb(spu_rotqbyi(r[6],12),spu_rotqbyi(r[6],12),shuffle);
    u128 sy=spu_shufb(spu_rotqbyi(r[9],12),spu_rotqbyi(r[9],12),shuffle);
    u128 sz=spu_shufb(spu_rotqbyi(r[7],12),spu_rotqbyi(r[7],12),shuffle);
    u128 centre=spu_shufb(r[8],one,r[10]);
    u128 x=spu_shufb(ms_thread_fm(r[6],sx),zero,r[10]);
    u128 y=spu_shufb(ms_thread_fm(r[9],sy),zero,r[10]);
    u128 z=spu_shufb(ms_thread_fm(r[7],sz),zero,r[10]);
    u128 difference=ms_thread_fa(z,spu_xor(x,spu_ilhu(0x8000)));
    u128 sum=ms_thread_fa(x,z), low=ms_thread_fs(centre,y), high=ms_thread_fa(centre,y);
    r[22]=ms_thread_fs(low,difference); r[21]=ms_thread_fs(low,sum);
    r[20]=ms_thread_fs(high,sum); r[19]=ms_thread_fa(high,sum);
    r[18]=ms_thread_fa(low,sum); r[17]=ms_thread_fa(high,difference);
    r[16]=ms_thread_fa(low,difference); r[15]=ms_thread_fs(high,difference);

    ms_geometry_corners box;
    const unsigned order[8]={19,18,17,16,15,22,20,21};
    uint32_t packed[3][8];
    for(unsigned i=0;i<8;++i) {
        box.corner[i]=r[order[i]];
        if(!ms_geometry_in_range(ms_geometry_vec(box.corner[i]))) return 0;
        for(unsigned a=0;a<3;++a) packed[a][i]=box.corner[i]._u32[a];
    }
    for(unsigned i=0;i<2;++i) {
        box.x[i]=vld1q_u32(packed[0]+4*i); box.y[i]=vld1q_u32(packed[1]+4*i);
        box.z[i]=vld1q_u32(packed[2]+4*i);
    }
    u128 plane;
    unsigned used=0; int hit=0;
    for(;used<count;) {
        plane=spu_ls_read128(ctx,r[13]._u32[0]+16*used);
        if(!ms_geometry_in_range(ms_geometry_vec(plane))) return 0;
        ++used;
        if((hit=ms_geometry_plane(&box,plane,any_negative))) break;
    }
    /* Reconstruct the final original iteration's full scratch register file.
     * Earlier iterations only changed these scratch values, the plane pointer
     * and count. No ABI dead-register assumption is needed at this boundary.
     */
    uint32x4_t px=ms_geometry_vec(plane),py=vextq_u32(px,px,1),pz=vextq_u32(px,px,2),pd=vextq_u32(px,px,3);
    uint32x4_t predicate[8], final_product=vdupq_n_u32(0), final_partial=final_product;
    for(unsigned i=0;i<8;++i) {
        uint32x4_t cx=ms_geometry_vec(box.corner[i]);
        uint32x4_t cy=vextq_u32(cx,cx,1),cz=vextq_u32(cx,cx,2);
        uint32x4_t product=ms_geometry_mul(px,cx),partial=ms_geometry_madd(py,cy,product);
        uint32x4_t dot=ms_geometry_add(ms_geometry_madd(pz,cz,partial),pd);
        predicate[i]=vandq_u32(ms_geometry_side(dot,any_negative),vdupq_n_u32(1));
        r[41-2*i]=ms_geometry_u128(cy); r[40-2*i]=ms_geometry_u128(cz);
        if(i==7) { final_product=product; final_partial=partial; }
    }
    uint32x4_t combined=predicate[0];
    for(unsigned i=1;i<8;++i) combined=any_negative ? vorrq_u32(combined,predicate[i]) : vandq_u32(combined,predicate[i]);
    r[2]=ms_geometry_u128(combined); r[3]=spu_il(hit);
    r[4]=ms_geometry_u128(predicate[2]); r[5]=ms_geometry_u128(predicate[7]);
    r[6]=ms_geometry_u128(predicate[3]); r[7]=ms_geometry_u128(predicate[4]);
    r[8]=ms_geometry_u128(predicate[6]); r[9]=ms_geometry_u128(ms_geometry_zero(final_product));
    r[10]=ms_geometry_u128(predicate[5]); r[11]=ms_geometry_u128(ms_geometry_zero(final_partial));
    r[12]=ms_geometry_u128(pd); r[14]=spu_ai(r[13],16*used); r[13]=zero;
    r[23]=spu_il(used); r[24]=spu_ceq(r[25],r[23]);
    if(!ms_geometry_safe(ctx)) return 0;
    memcpy(ctx->gpr+2,r+2,40*sizeof(u128));
    ctx->steps += 2 + 2*(used-1) + (hit ? 0 : 2);
#ifdef MOTORSTORM_GEOMETRY_TEST
    extern unsigned ms_geometry_test_hits;
    ++ms_geometry_test_hits;
#else
    static atomic_uint reported[2];
    if(!atomic_load_explicit(&reported[any_negative],memory_order_relaxed) &&
       !atomic_exchange_explicit(&reported[any_negative],1,memory_order_relaxed))
        fprintf(stderr,"[native-geometry] box/planes active: kind=%d planes=%u tested=%u image=%d overlay=%d\n",any_negative,count,used,ctx->image_id,ctx->resident_ovl);
#endif
    return 1;
}
