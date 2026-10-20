/* Real Metal regression: physical raster size, guest WPOS, scissor, byte
 * aliases and depth ROI must agree at all three experimental resolutions. */
#include "../rsx_metal_backend.m"
#include <assert.h>
/* No guest or window is used by this fixture. The included backend also
 * defines its Cocoa view class; fail if its legacy path is ever invoked. */
u8* vm_base;
float cellAvconfExt_display_gamma(void) { return 1; }
int cellGcmOffsetIsDisplay(u32 off) { (void)off; assert(0); return 0; }
u32 cellGcmResolveOffset(u32 off) { (void)off; assert(0); return 0; }
u32 cellGcmResolveLocated(int loc,u32 off) { (void)loc;(void)off;assert(0);return 0; }
void rsx_fetch_attrib(const rsx_state* st,int idx,u32 vi,float out[4]) { (void)st;(void)idx;(void)vi;(void)out;assert(0); }
static const char* fp = "struct PSInput {float4 position:SV_POSITION;}; float4 main(PSInput input):SV_TARGET {return float4(input.position.xy/float2(160,90),0,1);}";
int main(void) { @autoreleasepool {
    s_dev=MTLCreateSystemDefaultDevice();assert(s_dev);s_queue=[s_dev newCommandQueue];
    assert(create_depth()==0);
    NSError* error=nil;
    id<MTLLibrary> lib=[s_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; vertex float4 vs(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],.375,1); }" options:nil error:&error];assert(lib);
    for (int mode=0;mode<3;++mode) {
        const char* heights[]={"720","900","1080"};setenv("PS3RECOMP_METAL_INTERNAL_HEIGHT",heights[mode],1);
        assert(eng_init(NULL,1280,720)==0);
        assert(s_depth.width==(mode==0?1280:mode==1?1600:1920));
        assert(s_depth.height==(mode==0?720:mode==1?900:1080));
        u32 color=eng_color_target_create(NULL,RSX_BE_FMT_R32G32B32A32F,160,90,NULL,0);
        u32 depth=eng_depth_target_create(NULL,160,90);
        assert(eng_obj(color).width==(mode==0?160:mode==1?200:240));
        assert(eng_obj(depth).height==(mode==0?90:mode==1?113:135));
        u32 shadow=eng_depth_target_create(NULL,1024,1024);assert(eng_obj(shadow).width==1024);
        MTLRenderPipelineDescriptor* pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[lib newFunctionWithName:@"vs"];
        pd.fragmentFunction=eng_function(fp,RSX_SHADER_STAGE_FRAGMENT,"resolution-test");assert(pd.fragmentFunction);
        pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Float;pd.depthAttachmentPixelFormat=MTL_DEPTH_FORMAT;pd.stencilAttachmentPixelFormat=MTL_DEPTH_FORMAT;
        s_eng_pipe[0].pso=[s_dev newRenderPipelineStateWithDescriptor:pd error:&error];assert(s_eng_pipe[0].pso);s_eng_pipe_count=1;
        MTLDepthStencilDescriptor* ds=[MTLDepthStencilDescriptor new];ds.depthCompareFunction=MTLCompareFunctionAlways;ds.depthWriteEnabled=YES;s_eng_pipe[0].ds=[s_dev newDepthStencilStateWithDescriptor:ds];
        MTLRenderPassDescriptor* rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=eng_obj(color);rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;
        rp.depthAttachment.texture=eng_obj(depth);rp.depthAttachment.loadAction=MTLLoadActionClear;rp.depthAttachment.clearDepth=1;rp.depthAttachment.storeAction=MTLStoreActionStore;
        rp.stencilAttachment.texture=eng_obj(depth);rp.stencilAttachment.loadAction=MTLLoadActionClear;rp.stencilAttachment.storeAction=MTLStoreActionStore;
        id<MTLCommandBuffer> cb=[s_queue commandBuffer];id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:rp];assert(enc);
        EngRecord r={0};r.pipeline=1;r.rt[0]=color;r.nrt=1;r.depth=depth;r.topology=MTLPrimitiveTypeTriangle;r.vertex_count=3;r.vp[2]=160;r.vp[3]=90;
        r.sc[0]=8;r.sc[1]=4;r.sc[2]=144;r.sc[3]=80;
        for(unsigned i=0;i<RSX_BE_MAX_TEXTURES;i++)r.samp[i]=-1;for(unsigned i=0;i<RSX_BE_MAX_VERTEX_TEXTURES;i++)r.vsamp[i]=-1;
        eng_encode_draw(enc,&r,nil);[enc endEncoding];[cb commit];[cb waitUntilCompleted];assert(cb.status==MTLCommandBufferStatusCompleted);
        float pixels[160*90*4];eng_readback(NULL,color,0,0,160,90,pixels,160*16);
        for(int y=0;y<90;++y)for(int x=0;x<160;++x){float* p=pixels+(y*160+x)*4;
            if(x>=8&&x<152&&y>=4&&y<84){assert(fabsf(p[0]-(x+.5f)/160)<.006f);assert(fabsf(p[1]-(y+.5f)/90)<.009f);assert(p[3]==1);}
            if(x<7||x>152||y<3||y>84)assert(p[3]==0);
        }
        float roi[3*5];for(int i=0;i<15;i++)roi[i]=-1;
        assert(eng_depth_readback(NULL,depth,20,10,3,3,roi,5*4)==0);
        for(int y=0;y<3;y++)for(int x=0;x<5;x++)assert(roi[y*5+x]==(x<3?.375f:-1));
        u8 seed[160*90*4],back[160*90*4];for(int i=0;i<sizeof seed;i++)seed[i]=(u8)(i*13);
        u32 seeded=eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,160,90,seed,160*4);
        eng_readback(NULL,seeded,0,0,160,90,back,160*4);assert(!memcmp(seed,back,sizeof seed));
        u32 view=eng_surface_view(NULL,seeded,0xAAE4,0xA5);assert(view && s_eng_guest_w[view-1]==160);
        u32 snap=eng_color_snapshot(NULL,seeded);assert(snap && s_eng_guest_w[snap-1]==160);eng_submit_and_wait(NULL,0);
        assert(s_eng_checked_completed);printf("PASS %s: physical target, WPOS, scissor, depth ROI, packed bytes, view and snapshot\n",heights[mode]);
        /* Real recorded path without a guest zeta must cover the entire
         * larger colour surface, including its bottom-right physical pixel. */
        u32 display=eng_color_target_create(NULL,RSX_BE_FMT_R32G32B32A32F,1280,720,NULL,0);
        const float zero[4]={0};eng_clear_color(NULL,display,zero);
        EngRecord full=r;full.rt[0]=display;full.depth=0;full.vp[2]=1280;full.vp[3]=720;
        full.sc[0]=full.sc[1]=0;full.sc[2]=1280;full.sc[3]=720;
        s_eng_rec[s_eng_rec_count++]=full;eng_submit_and_wait(NULL,0);assert(s_eng_checked_completed);
        float corner[4]={0};eng_readback(NULL,display,1279,719,1,1,corner,16);
        assert(corner[3]==1);printf("PASS %s: full display coverage with fallback depth\n",heights[mode]);
        /* The engine grows the same zeta for landscape and portrait passes.
         * Its resulting 1280-square allocation is scene depth, not a shadow
         * map. It must not clip a scaled 1280x720 colour attachment. */
        u32 padded_depth=eng_depth_target_create(NULL,1280,1280);
        eng_clear_color(NULL,display,zero);full.depth=padded_depth;
        s_eng_rec[s_eng_rec_count++]=full;eng_submit_and_wait(NULL,0);
        assert(s_eng_checked_completed);memset(corner,0,sizeof corner);
        eng_readback(NULL,display,1279,719,1,1,corner,16);
        assert(corner[3]==1);printf("PASS %s: full display coverage with padded scene depth\n",heights[mode]);
        eng_shutdown(NULL);
    }
    return 0;
}}
