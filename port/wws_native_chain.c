#include "wws_native_chain.h"
#include "ps3emu/passive_metrics.h"

#if PS3_PASSIVE_METRICS
#define WWS_DIAGNOSTIC(...) __VA_ARGS__
#else
#define WWS_DIAGNOSTIC(...)
#endif
#include "spu_thread_rtz.h"
#include "spu_job_scope.h"
#include "native_geometry.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

void ms_wws_chain_capture(const spu_context* ctx, unsigned entry)
{
#if PS3_PASSIVE_METRICS
    static atomic_int enabled = -1;
    static atomic_uint calls[2], saved[2];
    unsigned kind=entry==0xDCBC;
    int active = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (active < 0) {
        const char* dir = getenv("MOTORSTORM_WWS_CAPTURE");
        active = dir && *dir;
        atomic_store_explicit(&enabled, active, memory_order_relaxed);
    }
    if (!active || !ctx->gpr[kind?18:83]._u32[0] || atomic_load_explicit(&saved[kind], memory_order_relaxed) >= 32) return;
    unsigned call = atomic_fetch_add_explicit(&calls[kind], 1, memory_order_relaxed);
    if (call >= 8 && call % 512) return;
    unsigned slot = atomic_fetch_add_explicit(&saved[kind], 1, memory_order_relaxed);
    if (slot >= 32) return;
    ms_wws_chain_fixture* f = calloc(1, sizeof(*f));
    if (!f) return;
    uint64_t fpcr = 0;
#if defined(__aarch64__)
    __asm__ volatile("mrs %0, fpcr":"=r"(fpcr));
#endif
    const uint32_t h[] = {0x4d535731, 1, sizeof(*f), entry, ctx->pc, ctx->steps,
        ctx->image_id, ctx->resident_ovl, ctx->policy_mode, ctx->host_depth,
        ctx->int_enable, ctx->event_mask, ctx->event_status, ctx->status,
        (uint32_t)fpcr, (uint32_t)(fpcr >> 32), call};
    memcpy(f->header, h, sizeof(h)); memcpy(f->gpr, ctx->gpr, sizeof(f->gpr));
    memcpy(f->ls, ctx->ls, sizeof(f->ls));
    const char* dir = getenv("MOTORSTORM_WWS_CAPTURE");
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/wws-%04x-%02u.bin", dir, entry, slot);
    if (n > 0 && (size_t)n < sizeof(path)) {
        mkdir(dir, 0700);
        FILE* out = fopen(path, "wb");
        if (out) { if (fwrite(f, sizeof(*f), 1, out) != 1) perror("WWS capture"); fclose(out); }
    }
    free(f);
#else
    (void)ctx; (void)entry;
#endif
}

/* Values retain the SPU format, including extended finite exponents. These
 * arithmetic primitives are shared with the independently tested exact ARM
 * implementation; the algorithm below operates on objects, not registers. */
typedef u128 Vec;
static inline Vec splat(uint32_t v) { Vec r={{0}};for(unsigned i=0;i<4;i++)r._u32[i]=v;return r; }
static inline Vec lane(Vec v,unsigned i) { return splat(v._u32[i]); }
static inline Vec with_w(Vec v,uint32_t w) { v._u32[3]=w;return v; }
static inline Vec mul(Vec a,Vec b) { return spu_fm(a,b); }
static inline Vec add(Vec a,Vec b) { return spu_fa(a,b); }
static inline Vec sub(Vec a,Vec b) { return spu_fs(a,b); }
static inline Vec madd(Vec a,Vec b,Vec c) { return spu_fma(a,b,c); }
static inline Vec nms(Vec a,Vec b,Vec c) { return spu_fnms(a,b,c); }
static inline int gt(Vec a,Vec b) { return spu_fcgt(a,b)._u32[0]!=0; }
static inline Vec negate(Vec v) { return spu_xor(v,splat(0x80000000)); }
static inline Vec zero(void) { return splat(0); }
static inline Vec one(void) { return splat(0x3f800000); }
static inline uint32_t word(const uint8_t* p,unsigned a) { uint32_t v;memcpy(&v,p+a,4);return SPU_BSWAP32(v); }
static inline void store_word(uint8_t* p,unsigned a,uint32_t v) { v=SPU_BSWAP32(v);memcpy(p+a,&v,4); }
static inline Vec quad(const uint8_t* p,unsigned a) {
    Vec v;for(unsigned i=0;i<4;i++)v._u32[i]=word(p,a+4*i);return v;
}
static inline void store_quad(uint8_t* p,unsigned a,Vec v) { for(unsigned i=0;i<4;i++)store_word(p,a+4*i,v._u32[i]); }
static inline void increment(uint8_t* p,unsigned a) { store_word(p,a,word(p,a)+1); }
static inline unsigned increment_counter(uint8_t* p,unsigned a) {
    Vec v=quad(p,a);unsigned before=v._u32[0];store_quad(p,a,spu_ai(v,1));return before;
}
static inline int flag(const uint8_t* p,unsigned a) { return p[a]!=0; }
static inline Vec scalar(const uint8_t* p,unsigned a) { return splat(word(p,a)); }
static Vec length3(Vec v) {
    Vec q=mul(v,v),y=spu_rotqbyi(v,4),z=spu_rotqbyi(v,8);
    Vec square=madd(z,z,madd(y,y,q));
    Vec reciprocal=ms_job_fi(square,spu_frsqest(square));
    Vec estimate=mul(reciprocal,square);
    Vec correction=nms(reciprocal,estimate,splat(0x3f800001));
    return lane(madd(correction,mul(estimate,splat(0x3f000000)),estimate),0);
}
static Vec reciprocal(Vec v) { return ms_job_fi(v,spu_frest(v)); }
static Vec transform(const Vec matrix[4],Vec point) {
    Vec a=mul(matrix[0],lane(point,0)),b=mul(matrix[1],lane(point,1));
    return add(madd(matrix[2],lane(point,2),a),madd(matrix[3],lane(point,3),b));
}
static Vec dot3(Vec a,Vec b) {
    return lane(madd(spu_rotqbyi(a,8),spu_rotqbyi(b,8),
        madd(spu_rotqbyi(a,4),spu_rotqbyi(b,4),mul(a,b))),0);
}
static Vec plane_distance(Vec plane,Vec position) { return add(dot3(plane,position),lane(plane,3)); }
static void read_matrix(const uint8_t* p,unsigned a,Vec out[4]) { for(unsigned i=0;i<4;i++)out[i]=quad(p,a+16*i); }

/* Oriented box transform: fold each original extent into its axis, transform
 * the centre/directions, then split transformed directions into length+unit
 * axis. Operation order and the 1+ulp refinement constants match 0x7B30. */
static void transform_box(Vec box[4],const Vec matrix[4]) {
    box[0]=transform(matrix,with_w(box[0],0x3f800000));
    for(unsigned i=1;i<4;i++) {
        Vec direction=transform(matrix,with_w(mul(box[i],lane(box[i],3)),0));
        Vec length=length3(direction), inv=reciprocal(length);
        Vec correction=nms(length,inv,splat(0x3f800001));
        Vec scaled=mul(direction,inv);
        box[i]=with_w(madd(correction,scaled,scaled),length._u32[0]);
    }
}
static void box_corners(const Vec box[4],Vec corners[8]) {
    Vec x=with_w(mul(box[2],lane(box[2],3)),0);
    Vec y=with_w(mul(box[1],lane(box[1],3)),0);
    Vec z=with_w(mul(box[3],lane(box[3],3)),0);
    Vec centre=with_w(box[0],0x3f800000),diff=add(z,negate(x)),sum=add(x,z);
    Vec low=sub(centre,y),high=add(centre,y);
    corners[0]=add(high,sum);corners[1]=add(low,sum);
    corners[2]=add(high,diff);corners[3]=add(low,diff);
    corners[4]=sub(high,diff);corners[5]=sub(low,diff);
    corners[6]=sub(high,sum);corners[7]=sub(low,sum);
}
static int box_plane_test(const Vec corners[8],const uint8_t* p,unsigned planes,unsigned count,int any_negative) {
    /* Across-corner SIMD: each lane is a different corner. The retained
     * geometry range proof permits plain ARM RTZ here; other inputs use the
     * exact extended-SPU path below. No scratch GPR reconstruction is needed. */
    ms_geometry_corners packed;uint32_t xyz[3][8];int vector_ok=1;
    for(unsigned c=0;c<8;c++) {
        vector_ok&=ms_geometry_in_range(ms_geometry_vec(corners[c]));
        for(unsigned axis=0;axis<3;axis++)xyz[axis][c]=corners[c]._u32[axis];
    }
    for(unsigned half=0;half<2;half++) {
        packed.x[half]=vld1q_u32(xyz[0]+4*half);packed.y[half]=vld1q_u32(xyz[1]+4*half);
        packed.z[half]=vld1q_u32(xyz[2]+4*half);
    }
    for(unsigned i=0;i<count;i++) {
        Vec plane=quad(p,planes+16*i);int hit=any_negative?0:1;
        if(vector_ok&&ms_geometry_in_range(ms_geometry_vec(plane))) {
            if(ms_geometry_plane(&packed,plane,any_negative))return 1;
            continue;
        }
        for(unsigned j=0;j<8;j++) {
            Vec distance=plane_distance(plane,corners[j]);
            if(any_negative)hit|=gt(zero(),distance);else hit&=gt(distance,zero());
        }
        if(hit)return 1;
    }
    return 0;
}
static int frustum_sphere_reject(const uint8_t* p,Vec sphere) {
    Vec radius=lane(sphere,3),axis=quad(p,0x16870),origin=quad(p,0x16860);
    Vec cone=quad(p,0x16880),cap=mul(radius,quad(p,0x16890));
    Vec offset=sub(sphere,nms(axis,lane(cap,0),origin));
    Vec projection=dot3(axis,offset),distance=dot3(offset,offset);
    if(!gt(projection,zero()) || gt(mul(mul(lane(cone,2),lane(cone,2)),distance),mul(projection,projection)))return 1;
    offset=sub(sphere,origin);projection=negate(dot3(axis,offset));distance=dot3(offset,offset);
    if(gt(projection,zero()) && !gt(mul(mul(lane(cone,3),lane(cone,3)),distance),mul(projection,projection)) &&
       gt(distance,mul(radius,radius)))return 1;
    sphere=with_w(sphere,0x3f800000);
    for(unsigned i=0;i<5;i++)if(!gt(zero(),sub(plane_distance(quad(p,0x168A0+16*i),sphere),radius)))return 1;
    return 0;
}
typedef struct LazyBox { Vec corners[8];int ready;const Vec* matrix;unsigned address; } LazyBox;
static const Vec* get_corners(LazyBox* box,const uint8_t* p) {
    if(!box->ready) {
        Vec local[4];read_matrix(p,box->address,local);transform_box(local,box->matrix);
        box_corners(local,box->corners);box->ready=1;
    }
    return box->corners;
}
static int occluded(const uint8_t* p,Vec sphere,LazyBox* box) {
    unsigned count=word(p,0x265E0);Vec radius=lane(sphere,3);
    for(unsigned i=0;i<count;i++) {
        unsigned planes=0x1BAE0+304*word(p,0x265F0+4*i),n=word(p,planes+288);
        int sphere_outside=0;
        for(unsigned j=0;j<n;j++)if(gt(zero(),sub(plane_distance(quad(p,planes+16*j),sphere),radius))) {
            sphere_outside=1;break;
        }
        if(!sphere_outside || !box_plane_test(get_corners(box,p),p,planes,n,1))return 1;
    }
    return 0;
}
static int projected_size_reject(const uint8_t* p,Vec sphere,Vec weight) {
    Vec view[4],projection[4];read_matrix(p,0x169B0,view);read_matrix(p,0x16970,projection);
    Vec centre=with_w(transform(view,with_w(sphere,0x3f800000)),0x3f800000);
    Vec radius=lane(sphere,3);
    Vec a=with_w(madd(radius,quad(p,0x163B0),centre),0x3f800000);
    Vec b=with_w(madd(radius,quad(p,0x163C0),centre),0x3f800000);
    a=transform(projection,a);b=transform(projection,b);
    Vec aw=lane(a,3),bw=lane(b,3),ai=reciprocal(aw),bi=reciprocal(bw);
    Vec ac=nms(aw,ai,splat(0x3f800001)),bc=nms(bw,bi,splat(0x3f800001));
    a=mul(a,ai);b=mul(b,bi);a=madd(ac,a,a);b=madd(bc,b,b);
    a._u32[2]=b._u32[2]=0;
    Vec delta=sub(a,b);weight=mul(weight,quad(p,0x16800));
    return gt(mul(weight,weight),dot3(delta,delta));
}

typedef struct Range { unsigned lo,size; } Range;
static int valid_range(Range r,unsigned alignment) { return r.lo%alignment==0 && r.lo<=SPU_LS_SIZE && r.size<=SPU_LS_SIZE-r.lo; }
static int overlap(Range a,Range b) { return a.size&&b.size&&a.lo<b.lo+b.size&&b.lo<a.lo+a.size; }

/* Reject unsupported layouts before any write. The original runs on rejection.
 * In particular a batch that might flush the 4096-entry DMA buffer must retain
 * its original publication point. Native code never changes that protocol. */
int ms_wws_classify_batch(uint8_t* p,unsigned count) {
    if(!count)return 1;
    if(count>256)return 0;
    unsigned records=word(p,0x16A30),matrices=word(p,0x16A60),spheres=word(p,0x16A50),boxes=word(p,0x16A40);
    unsigned stats=word(p,0x16780),primary=word(p,0x167B0),secondary=word(p,0x16790),auxiliary=word(p,0x167A0);
    unsigned main_count=word(p,0x16770),second_count=word(p,0x17AC0),aux_count=word(p,0x17AD0),special_count=word(p,0x16AB0);
    if(main_count>=4096 || count>=4096-main_count || second_count>2048 || count>2048-second_count ||
       aux_count>2048 || special_count>1024 || count>1024-special_count || word(p,0x265E0)>64)return 0;
    const uint32_t shuffle[4]={0x00010203,0x04050607,0x08090a0b,0x10111213};
    for(unsigned i=0;i<4;i++)if(word(p,0x16370+4*i)!=shuffle[i])return 0;
    Range inputs[]={{records,count*20},{matrices,count*64},{spheres,count*16},{boxes,count*64},{0x16370,0x16AC0-0x16370},{0x1BAE0,0x26F10-0x1BAE0}};
    Range outputs[]={{stats,32},{primary,16384},{secondary,secondary?8192:0},{auxiliary,auxiliary?8192:0},
        {0x16AC0,4096},{0x17AE0,8192},{0x19AE0,8192}};
    for(unsigned i=0;i<4;i++)if(!valid_range(inputs[i],16)||inputs[i].lo<0x27000||inputs[i].lo+inputs[i].size>0x3B000)return 0;
    for(unsigned i=0;i<sizeof(outputs)/sizeof(*outputs);i++) {
        if(!valid_range(outputs[i],4))return 0;
        if(i<4&&outputs[i].size&&(outputs[i].lo<0x27000||outputs[i].lo+outputs[i].size>0x3B000))return 0;
        for(unsigned j=0;j<sizeof(inputs)/sizeof(*inputs);j++)if(overlap(outputs[i],inputs[j]))return 0;
        for(unsigned j=0;j<i;j++)if(overlap(outputs[i],outputs[j]))return 0;
    }
    for(unsigned i=0;i<4;i++) {
        for(unsigned j=0;j<i;j++)if(overlap(inputs[i],inputs[j]))return 0;
        for(unsigned j=4;j<sizeof(inputs)/sizeof(*inputs);j++)if(overlap(inputs[i],inputs[j]))return 0;
    }
    for(unsigned i=0;i<count;i++)if((word(p,records+20*i+8)&65535)>11)return 0;
    for(unsigned i=0;i<word(p,0x265E0);i++) {
        unsigned index=word(p,0x265F0+4*i);
        if(index>=144 || word(p,0x1BAE0+304*index+288)>18)return 0;
    }
    for(unsigned i=0;i<count;i++) {
        unsigned record=records+20*i,flags=word(p,record+12),type=word(p,record+8);
        if(!(flags&1))continue;
        Vec matrix[4];read_matrix(p,matrices+64*i,matrix);
        Vec sphere=quad(p,spheres+16*i),scale=length3(matrix[2]);
        Vec world=transform(matrix,with_w(sphere,0x3f800000));
        world=with_w(world,mul(lane(sphere,3),scale)._u32[0]);
        Vec radius=mul(scalar(p,record),scale),weight=scalar(p,record+4),depth=zero();int omit_secondary=0;
        store_word(p,record,radius._u32[0]);
        unsigned mode=word(p,0x167C0),lod=mode<17 ? 1u<<(mode+15) : 0;
        if(!mode) {
            Vec z=plane_distance(quad(p,0x16840),world);lod=0x10000;
            if(gt(z,zero())) {
                Vec inv=reciprocal(z),estimate=mul(radius,inv);
                Vec size=mul(mul(madd(nms(estimate,z,radius),inv,estimate),quad(p,0x16850)),splat(0x44870000));
                unsigned kind=type&65535;
                if(!gt(size,scalar(p,0x164F0+4*kind)))lod=gt(size,scalar(p,0x164C0+4*kind))?0x20000:0x40000;
            }
        }
        if(!(type&lod)) { increment(p,stats+8);continue; }
        if(flag(p,0x16810)) {
            if(!flag(p,0x16920)) {
                if(projected_size_reject(p,world,weight)) { increment(p,stats+8);continue; }
            } else {
                depth=plane_distance(quad(p,0x168F0),world);
                Vec inv=reciprocal(depth),approx=mul(one(),inv);
                inv=madd(nms(approx,depth,one()),inv,approx);
                Vec size=mul(mul(mul(radius,inv),quad(p,0x16850)),splat(0x44870000));
                Vec threshold=scalar(p,0x163F0+4*(type&65535));int reject;
                if(gt(depth,zero())) {
                    Vec relative=mul(lane(world,3),inv);
                    omit_secondary=gt(threshold,size)||gt(quad(p,0x16900),relative);
                    reject=gt(mul(weight,quad(p,0x16910)),relative);
                } else {
                    reject=!gt(depth,negate(lane(world,3)));
                    omit_secondary=reject||gt(threshold,negate(size));
                }
                if(reject) { increment(p,stats+8);continue; }
            }
        }
        /* The old transform only writes private callee scratch. Delay it
         * until a sphere test actually needs the more expensive box test. */
        LazyBox box={.ready=0,.matrix=matrix,.address=boxes+64*i};
        if(flag(p,0x16820)&&(frustum_sphere_reject(p,world)||
           box_plane_test(get_corners(&box,p),p,0x168A0,5,0))) { increment(p,stats+4);continue; }
        if(flag(p,0x16830)&&word(p,0x265E0)&&occluded(p,world,&box)) { increment(p,stats);continue; }
        increment(p,stats+12);
        unsigned index=word(p,0x26F00)+i;
        if(auxiliary&&(flags>>1)) {
            unsigned n=word(p,0x17AD0);
            if(n<2048) { increment_counter(p,0x17AD0);store_word(p,auxiliary+4*n,index);store_word(p,0x19AE0+4*n,depth._u32[0]); }
            if((flags>>1)==2) { n=increment_counter(p,0x16AB0);store_word(p,0x16AC0+4*n,index); }
        } else {
            unsigned n=increment_counter(p,0x16770);store_word(p,primary+4*n,index);increment(p,stats+16);
        }
        if(secondary&&!omit_secondary) {
            unsigned n=increment_counter(p,0x17AC0);store_word(p,secondary+4*n,index);store_word(p,0x17AE0+4*n,depth._u32[0]);
        }
    }
    return 1;
}

static int chain_enabled(unsigned mask) {
    static atomic_int enabled=-1;
    int active=atomic_load_explicit(&enabled,memory_order_relaxed);
    if(active<0) {
        const char* s=getenv("MOTORSTORM_WWS_CHAIN");active=s&&s[0]>='0'&&s[0]<='7'&&!s[1]?s[0]-'0':0;
        if(getenv("SPU_HOST_DEPTH_MAX")||getenv("SPU_SN_DEFER")||getenv("SPU_PCHIST")||
           getenv("MOTORSTORM_CULL_PROFILE")||getenv("MOTORSTORM_GEOMETRY_CAPTURE"))active=0;
        atomic_store_explicit(&enabled,active,memory_order_relaxed);
    }
    return (active&mask)!=0;
}

int ms_wws_chain_try(spu_context* ctx) {
    if(!chain_enabled(1))return 0;
    WWS_DIAGNOSTIC(static SPU_THREAD_LOCAL uint64_t calls,accepted,objects,guard_rejects,layout_rejects;)
    WWS_DIAGNOSTIC(++calls;)int result=0;
    /* Only the checked caller's void subroutine is replaced. A pending IRQ,
     * diagnostic dispatch or unsupported stack always takes the original.
     * Channel/DMA operations remain outside this arithmetic phase. Runtime
     * steps count host transfers, not guest cycles, and need no emulation. */
    unsigned sp=ctx->gpr[1]._u32[0];
    if(!ms_geometry_safe(ctx)||ctx->status!=SPU_STATUS_RUNNING||ctx->irq_saved||
       sp<0x3B060||sp>SPU_LS_SIZE-336||(sp&15)){ WWS_DIAGNOSTIC(++guard_rejects;) }
    else {
        unsigned ret=word(ctx->ls,sp+320);
        if(ret!=0x9820&&ret!=0xAEF4&&ret!=0xAF98&&ret!=0xB028&&ret!=0xB0A0){ WWS_DIAGNOSTIC(++guard_rejects;) }
        else if((result=ms_wws_classify_batch(ctx->ls,ctx->gpr[83]._u32[0]))) {
            WWS_DIAGNOSTIC(++accepted;objects+=ctx->gpr[83]._u32[0];)
        } else { WWS_DIAGNOSTIC(++layout_rejects;) }
    }
#if PS3_PASSIVE_METRICS
    if(calls==1||!(calls&8191))fprintf(stderr,
        "[wws-chain] calls=%llu accepted=%llu objects=%llu guard=%llu layout=%llu\n",
        (unsigned long long)calls,(unsigned long long)accepted,(unsigned long long)objects,
        (unsigned long long)guard_rejects,(unsigned long long)layout_rejects);
#endif
    return result;
}

/* Equal-key partition order is observable: the following occlusion pass keeps
 * only a bounded prefix. This is the original Dinkumware-style algorithm, not
 * a replacement with libc qsort (whose tie order differs).
 *
 * The partition/median algorithm is adapted from Microsoft STL's algorithm:
 * Copyright (c) Microsoft Corporation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 * https://github.com/microsoft/STL/blob/main/stl/inc/algorithm
 * See wws_sort.LICENSE.txt. Modified here for C, fixed-capacity SPU records,
 * exact extended-float ordering and transactional fallback before LS writes.
 */
typedef struct SortItem { uint32_t index; int32_t key; } SortItem;
static void sort_swap(SortItem* a,SortItem* b) { SortItem t=*a;*a=*b;*b=t; }
static void sort_med3(SortItem* a,SortItem* b,SortItem* c) {
    if(b->key>a->key)sort_swap(a,b);
    if(c->key>b->key) {sort_swap(b,c);if(b->key>a->key)sort_swap(a,b);}
}
static int sort_range(SortItem* a,unsigned lo,unsigned hi,unsigned ideal) {
    while(hi-lo>32) {
        if(!ideal)return 0; /* Original heap path, with no partial publication. */
        unsigned mid=lo+(hi-lo)/2,last=hi-1;
        if(last-lo>40) {
            unsigned step=(hi-lo)/8;
            sort_med3(a+lo,a+lo+step,a+lo+2*step);
            sort_med3(a+mid-step,a+mid,a+mid+step);
            sort_med3(a+last-2*step,a+last-step,a+last);
            sort_med3(a+lo+step,a+mid,a+last-step);
        } else sort_med3(a+lo,a+mid,a+last);
        unsigned pf=mid,pl=mid+1;
        while(lo<pf&&a[pf-1].key==a[pf].key)--pf;
        while(pl<hi&&a[pl].key==a[pf].key)++pl;
        unsigned gf=pl,gl=pf;
        for(;;) {
            for(;gf<hi;++gf) {
                if(a[pf].key>a[gf].key)continue;
                if(a[gf].key>a[pf].key)break;
                if(pl!=gf)sort_swap(a+pl++,a+gf);else ++pl;
            }
            for(;lo<gl;--gl) {
                if(a[gl-1].key>a[pf].key)continue;
                if(a[pf].key>a[gl-1].key)break;
                if(--pf!=gl-1)sort_swap(a+pf,a+gl-1);
            }
            if(gl==lo&&gf==hi)break;
            if(gl==lo) {
                if(pl!=gf)sort_swap(a+pf,a+pl);
                ++pl;sort_swap(a+pf,a+gf);++pf;++gf;
            } else if(gf==hi) {
                if(--gl!=--pf)sort_swap(a+gl,a+pf);
                sort_swap(a+pf,a+--pl);
            } else {sort_swap(a+gf,a+--gl);++gf;}
        }
        ideal=ideal/2+ideal/4;
        if(pf-lo<hi-pl) {if(!sort_range(a,lo,pf,ideal))return 0;lo=pl;}
        else {if(!sort_range(a,pl,hi,ideal))return 0;hi=pf;}
    }
    for(unsigned i=lo+1;i<hi;i++) {
        SortItem value=a[i];unsigned j=i;
        while(j>lo&&value.key>a[j-1].key) {a[j]=a[j-1];--j;}
        a[j]=value;
    }
    return 1;
}
int ms_wws_sort_occluders(uint8_t* p,unsigned count) {
    if(count>144)return 0;
    SortItem a[144];
    for(unsigned i=0;i<count;i++) {
        unsigned index=word(p,0x26CC0+4*i);if(index>=144)return 0;
        uint32_t bits=word(p,0x26A80+4*index);
        /* SPU compares exponent-255 values as finite and flushes exponent-zero
         * values to signed zero. The monotonic signed key matches both. */
        int32_t mag=(bits&0x7f800000)?(int32_t)(bits&0x7fffffff):0;
        a[i]=(SortItem){index,(bits&0x80000000)?-mag:mag};
    }
    if(!sort_range(a,0,count,count))return 0;
    for(unsigned i=0;i<count;i++)store_word(p,0x26CC0+4*i,a[i].index);
    return 1;
}

static int select_layout(const uint8_t* p,unsigned count) {
    unsigned spheres=word(p,0x16A90);
    if(count>144||spheres<0x27000||spheres>0x3B000-144*16||(spheres&15))return 0;
    const uint32_t shuffle[4]={0x00010203,0x04050607,0x08090a0b,0x10111213};
    for(unsigned i=0;i<4;i++)if(word(p,0x16370+4*i)!=shuffle[i])return 0;
    for(unsigned i=0;i<count;i++) {
        unsigned index=word(p,0x26CC0+4*i);
        if(index>=144||word(p,0x1BAE0+304*index+288)>18)return 0;
    }
    return 1;
}
/* Structure-of-arrays SIMD: a lane is one sphere, rather than an unused
 * rotation of the same dot product. All operations still use exact SPU RTZ
 * helpers, so extended inputs need no approximation or range assumption. */
static Vec frustum_spheres4(const uint8_t* p,Vec x,Vec y,Vec z,Vec radius) {
    Vec axis=quad(p,0x16870),origin=quad(p,0x16860),cone=quad(p,0x16880);
    Vec ax=lane(axis,0),ay=lane(axis,1),az=lane(axis,2),cap=mul(radius,scalar(p,0x16890));
    Vec ox=sub(x,nms(ax,cap,lane(origin,0))),oy=sub(y,nms(ay,cap,lane(origin,1)));
    Vec oz=sub(z,nms(az,cap,lane(origin,2)));
    Vec projection=madd(az,oz,madd(ay,oy,mul(ax,ox)));
    Vec distance=madd(oz,oz,madd(oy,oy,mul(ox,ox)));
    Vec rejected=spu_nor(spu_fcgt(projection,zero()),spu_fcgt(projection,zero()));
    rejected=spu_or(rejected,spu_fcgt(mul(mul(lane(cone,2),lane(cone,2)),distance),mul(projection,projection)));
    if(spu__nv_all4(ms_geometry_vec(rejected)))return rejected;
    ox=sub(x,lane(origin,0));oy=sub(y,lane(origin,1));oz=sub(z,lane(origin,2));
    projection=negate(madd(az,oz,madd(ay,oy,mul(ax,ox))));
    distance=madd(oz,oz,madd(oy,oy,mul(ox,ox)));
    Vec back=spu_andc(spu_fcgt(projection,zero()),
        spu_fcgt(mul(mul(lane(cone,3),lane(cone,3)),distance),mul(projection,projection)));
    rejected=spu_or(rejected,spu_and(back,spu_fcgt(distance,mul(radius,radius))));
    for(unsigned j=0;j<5;j++) {
        Vec plane=quad(p,0x168A0+16*j);
        Vec d=add(madd(lane(plane,2),z,madd(lane(plane,1),y,mul(lane(plane,0),x))),lane(plane,3));
        Vec inside=spu_fcgt(zero(),sub(d,radius));rejected=spu_or(rejected,spu_nor(inside,inside));
        if(spu__nv_all4(ms_geometry_vec(rejected)))break;
    }
    return rejected;
}
int ms_wws_select_occluders(uint8_t* p,unsigned count,unsigned* selected) {
    if(!select_layout(p,count))return 0;
    unsigned n=0,spheres=word(p,0x16A90);uint32_t indices[144];
    for(unsigned i=0;i<count;i+=4) {
        unsigned ids[4],lanes=count-i<4?count-i:4;
        Vec x=zero(),y=zero(),z=zero(),radius=zero();
        for(unsigned k=0;k<lanes;k++) {
            ids[k]=word(p,0x26CC0+4*(i+k));Vec sphere=quad(p,spheres+16*ids[k]);
            x._u32[k]=sphere._u32[0];y._u32[k]=sphere._u32[1];z._u32[k]=sphere._u32[2];radius._u32[k]=sphere._u32[3];
        }
        Vec rejected=frustum_spheres4(p,x,y,z,radius);
        for(unsigned k=0;k<lanes;k++)if(!rejected._u32[k])indices[n++]=ids[k];
    }
    /* F044..F2E8 performs pairwise plane tests but has no memory/channel writes
     * and no live result. The source audit proves its only output registers
     * die in F2F8..F530; no inferred visual approximation is involved. */
    for(unsigned i=0;i<n;i++) {
        store_word(p,0x26840+4*i,indices[i]);store_word(p,0x265F0+4*i,indices[i]);
    }
    store_quad(p,0x26830,splat(n));*selected=n;return 1;
}
int ms_wws_sort_try(spu_context* ctx) {
    if(!chain_enabled(2))return 0;
    WWS_DIAGNOSTIC(static SPU_THREAD_LOCAL uint64_t calls,accepted,objects,guard_rejects,layout_rejects,selected_batches;)
    WWS_DIAGNOSTIC(++calls;)int result=0;unsigned n=ctx->gpr[18]._u32[0],sp=ctx->gpr[1]._u32[0];
    if(!ms_geometry_safe(ctx)||ctx->status!=SPU_STATUS_RUNNING||ctx->irq_saved||
       sp<0x3B200||sp>SPU_LS_SIZE-32||(sp&15)||ctx->gpr[87]._u32[0]!=0x26CC0||
       word(ctx->ls,0x16AA0)!=n){ WWS_DIAGNOSTIC(++guard_rejects;) }
    else if((result=ms_wws_sort_occluders(ctx->ls,n))) {
        ctx->gpr[87]=spu_ila(0x26CC0);WWS_DIAGNOSTIC(++accepted;objects+=n;)
        unsigned selected;
        if(chain_enabled(4)&&ms_wws_select_occluders(ctx->ls,n,&selected)) {
            ctx->gpr[10]=spu_il((int)selected);result=2;WWS_DIAGNOSTIC(++selected_batches;)
        }
    } else { WWS_DIAGNOSTIC(++layout_rejects;) }
#if PS3_PASSIVE_METRICS
    if(calls==1||!(calls&8191))fprintf(stderr,
        "[wws-sort] calls=%llu accepted=%llu objects=%llu guard=%llu layout=%llu selected=%llu\n",
        (unsigned long long)calls,(unsigned long long)accepted,(unsigned long long)objects,
        (unsigned long long)guard_rejects,(unsigned long long)layout_rejects,(unsigned long long)selected_batches);
#endif
    return result;
}
