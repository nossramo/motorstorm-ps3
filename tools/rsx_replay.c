/* Offline replay of tools/rsx_capture.py's MSRX0001 stream. No PPU or SPU.
 * Include the draw engine only to seed its initial state; the implementation
 * under test is identical to the game's. No production initialization changes.
 * This first runner supports the observed MotorStorm NV3062/NV308A transfers.
 * Unsupported active transfer engines fail explicitly instead of being skipped.
 */
#include "vertex_pack_bench.h"
#define rsx_vertex_pack_ex replay_vertex_pack
#include "rsx_draw_engine.c"
#undef rsx_vertex_pack_ex
#include "rsx_metal_backend.h"
#include "rsx_commands.h"

#define BANK_SIZE (256u << 20)
#define ID_LIMIT (1u << 20)
typedef struct { u8 *data; u32 size; } replay_blob;
typedef struct { u32 offset, location; u64 blob; int valid; } replay_memory;
static replay_blob *blobs, *displays, *tiles;
static replay_memory *memories;
static FILE *input;
u8 *vm_base;
u32 ppu_vm_size = 2 * BANK_SIZE;
static u32 display_offsets[8];
static rsx_draw_backend sampler_audit_backend;
static const rsx_draw_backend* sampler_audit_original;
static u64 sampler_audit_counts[17], sampler_audit_bias;
static void audit_bind_textures(void* user, const u32* textures,
                               const rsx_be_sampler_desc* samplers, u32 mask)
{
    for (u32 unit=0; unit<16; ++unit) if (mask & (1u<<unit)) {
        const u32 level=samplers[unit].max_anisotropy;
        if (level<=16) sampler_audit_counts[level]++;
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx,unit,&t);
        sampler_audit_bias += (t.filter & 0x1fffu) != 0;
        if (t.cubemap && getenv("RSX_REPLAY_CUBE_AUDIT")) {
            static u32 seen[64], count;
            u32 i=0; while(i<count && seen[i]!=t.offset) ++i;
            if (i==count && count<64) {
                seen[count++]=t.offset;
                u32 stride=rsx_texture_cube_face_stride(t.format,t.width,t.height,t.mipmaps,t.pitch);
                fprintf(stderr,"[cube audit] unit=%u offset=%08x loc=%u size=%ux%u format=%02x pitch=%u levels=%u stride=%u remap=%04x\n",
                    unit,t.offset,t.location,t.width,t.height,t.format,t.pitch,t.mipmaps,stride,t.remap);
                for(u32 face=0;face<6;++face) for(u32 j=0;j<g.n_surfaces;++j) {
                    const eng_surface* s=&g.surfaces[j];
                    if(s->location==t.location && s->offset==t.offset+face*stride)
                        fprintf(stderr,"[cube audit] face=%u slot=%u size=%ux%u fmt=%u pitch=%u raster=%u serial=%llu handle=%u\n",
                            face,j,s->w,s->h,s->fmt,s->pitch,s->raster_type,
                            (unsigned long long)s->color_serial,s->handle);
                }
            }
        }
    }
    sampler_audit_original->bind_textures(user,textures,samplers,mask);
}

static void fail(const char *why) { fprintf(stderr,"[replay] %s\n",why); exit(2); }
static void read_exact(void *p, size_t n) { if (fread(p,1,n,input)!=n) fail("truncated stream"); }
static u32 u32read(void) { u32 v; read_exact(&v,4); return v; }
static u64 u64read(void) { u64 v; read_exact(&v,8); return v; }
static u32 state_id(void) { u64 v=u64read(); if(v>=ID_LIMIT) fail("state ID exceeds limit"); return (u32)v; }
static void load_map(replay_blob *map, u32 fixed) {
    u32 n=u32read(); if(n>=ID_LIMIT) fail("too many states");
    while(n--) { u32 id=state_id(),size=u32read();
        if(map[id].data || size>BANK_SIZE || (fixed && size!=fixed)) fail("invalid state size/ID");
        map[id].size=size; map[id].data=malloc(size?size:1);
        if(!map[id].data) fail("allocation failure"); read_exact(map[id].data,size);
    }
}
static const u8 *memory_ptr(void *unused,u32 loc,u32 offset,u32 size) {
    (void)unused; if(loc>1 || offset>BANK_SIZE || size>BANK_SIZE-offset) return NULL;
    return vm_base + (loc==0 ? BANK_SIZE : 0) + offset;
}
u32 cellGcmResolveLocated(int local,u32 offset) { return (local?BANK_SIZE:0)+offset; }
u32 cellGcmResolveIO(u32 offset) { return offset; }
u32 cellGcmResolveOffset(u32 offset) { return BANK_SIZE+offset; }
int cellGcmOffsetIsDisplay(u32 offset) { for(int i=0;i<8;i++) if(display_offsets[i]==offset) return 1; return 0; }
void cellGcm_replay_display_buffers_to_engine(void) {}
float cellAvconfExt_display_gamma(void) { return 1.0f; }
void rsx_set_backend(rsx_backend *unused) { (void)unused; }
void rsx_fetch_attrib(const rsx_state *s,u32 a,u32 v,float *out) {
    (void)s;(void)a;(void)v;(void)out;fail("legacy renderer unexpectedly used");
}

/* libgcm's actual object handles in this capture, not guessed subchannels. */
static u32 objects[8], dst_dma, dst_offset, dst_pitch, inline_point, inline_size;
static void transfer(u32 method,u32 arg) {
    u32 sub=method>>13, m=method&0x1ffc;
    if(m==0) { objects[sub]=arg; return; }
    if(objects[sub]==0x313371c3u) { /* NV3062 */
        if(m==0x188) dst_dma=arg;
        else if(m==0x304) dst_pitch=arg>>16;
        else if(m==0x30c) dst_offset=arg;
        return;
    }
    if(objects[sub]==0x31337808u) { /* NV308A: 0x300 is COLOR_FORMAT, not a swizzle marker. */
        if(m==0x304) inline_point=arg;
        else if(m==0x308) inline_size=arg;
        else if(m>=0x400 && m<=0x7fc) {
            u64 offset=(u64)dst_offset+(inline_point>>16)*dst_pitch+
                ((inline_point&65535)+(m-0x400)/4)*4ull;
            if(offset+4>BANK_SIZE || !inline_size) fail("inline transfer outside memory");
            u8 *p=(u8*)memory_ptr(NULL,dst_dma==0xfeed0001u?1:0,(u32)offset,4);
            p[0]=arg>>24;p[1]=arg>>16;p[2]=arg>>8;p[3]=arg;
        }
        return;
    }
    if(m>=0x300) fail("unsupported active 2D transfer engine");
}

int main(int argc,char **argv) {
    if(argc!=3) { fprintf(stderr,"usage: %s frame.msrx output.ppm\n",argv[0]); return 2; }
    input=fopen(argv[1],"rb"); if(!input) fail("cannot open input");
    char magic[8]; read_exact(magic,8); if(memcmp(magic,"MSRX0001",8)) fail("bad stream version");
    u32 vp[544*4],regs[0x4000]; read_exact(vp,sizeof vp);read_exact(regs,sizeof regs);
    blobs=calloc(ID_LIMIT,sizeof *blobs);displays=calloc(ID_LIMIT,sizeof *displays);
    tiles=calloc(ID_LIMIT,sizeof *tiles);memories=calloc(ID_LIMIT,sizeof *memories);
    vm_base=calloc(1,2ull*BANK_SIZE);
    if(!blobs||!displays||!tiles||!memories||!vm_base) fail("allocation failure");
    load_map(tiles,432);load_map(displays,132);load_map(blobs,0);
    u32 n=u32read();if(n>=ID_LIMIT) fail("too many memory states");
    while(n--) { u32 id=state_id();replay_memory *m=&memories[id];
        m->offset=u32read();m->location=u32read();m->blob=state_id();m->valid=1;
        if(!blobs[m->blob].data || !memory_ptr(NULL,m->location,m->offset,blobs[m->blob].size)) fail("invalid memory state");
    }
    setenv("PS3RECOMP_METAL_HEADLESS","1",1);
    setenv("PS3RECOMP_METAL_FRAME_DUMP",argv[2],1);
    setenv("PS3RECOMP_METAL_FRAME_DUMP_EVERY","1",1);
    rsx_draw_engine_set_guest_memory(memory_ptr,NULL);
    if(rsx_metal_backend_init(1280,720,"MotorStorm RSX replay")) fail("Metal init failed");
    if (getenv("RSX_REPLAY_SAMPLER_AUDIT") || getenv("RSX_REPLAY_CUBE_AUDIT")) {
        sampler_audit_original=g.be; sampler_audit_backend=*g.be;
        sampler_audit_backend.bind_textures=audit_bind_textures;
        rsx_draw_engine_set_backend(&sampler_audit_backend);
    }
    rsx_dispatch_seed_registers(&g.rsx,regs,0x4000);
    rsx_dispatch_seed_transform_program(&g.rsx,vp,544*4);
    n=u32read(); u32 draws=0,flips=0;
    const char *pass_min_width=getenv("RSX_REPLAY_PASS_MIN_WIDTH");
    const u32 min_width=pass_min_width?(u32)strtoul(pass_min_width,NULL,10):0;
    for(u32 i=0;i<n;i++) {
        u32 method=u32read(),arg=u32read(),nm=u32read(),tile=state_id(),display=state_id();
        if(nm>=ID_LIMIT || (tile&&!tiles[tile].data) || (display&&!displays[display].data)) fail("invalid command state");
        while(nm--) { u32 id=state_id();replay_memory *m=&memories[id];
            if(!m->valid) fail("missing memory state"); replay_blob *b=&blobs[m->blob];
            memcpy((void*)memory_ptr(NULL,m->location,m->offset,b->size),b->data,b->size);
        }
        if(display) { u32 *d=(u32*)displays[display].data;
            if(d[32]>8) fail("invalid display count");
            for(u32 j=0;j<d[32];j++) { display_offsets[j]=d[j*4+3];
                rsx_draw_engine_set_display_buffer(j,0,d[j*4+3],d[j*4+2],d[j*4],d[j*4+1]); }
        }
        if(method==0xffffffff) continue;
        if(method>=0x2000 && method<0xe000) transfer(method,arg);
        else {
            /* Start diagnostics at the scene, past the hundreds of small
             * particle-atlas targets at the beginning of a gameplay frame. */
            if(min_width && method==0x1808 && arg==0) {
                rsx_dsp_surface sf;rsx_dsp_get_surface(&g.rsx,&sf);
                pd_capturing();
                if(s_pd.state==1 && sf.clip_w>=min_width && pd_open_index())
                    fprintf(s_pd.index,"replay command %u, draw %u\n",i,draws);
            }
            rsx_draw_engine_method(method,arg);
            draws+=(method==0x1808 && arg==0); flips+=(method==0xe944); }
    }
    if(fgetc(input)!=EOF) fail("trailing stream data"); fclose(input);
    if(!flips) rsx_draw_engine_present_buffer(1);
    rsx_draw_engine_flush();
    fprintf(stderr,"[replay] %u commands, %u draw ends, %u flips, %u guest draws\n",n,draws,flips,rsx_draw_engine_guest_draws());
    replay_vertex_bench_report();
    if (sampler_audit_original) {
        for (u32 i=0;i<=16;++i) if (sampler_audit_counts[i])
            fprintf(stderr,"[sampler audit] sampled anisotropy=%u bindings=%llu\n",i,(unsigned long long)sampler_audit_counts[i]);
        fprintf(stderr,"[sampler audit] sampled nonzero LOD bias bindings=%llu\n",(unsigned long long)sampler_audit_bias);
    }
    rsx_metal_backend_shutdown();
    return 0;
}
