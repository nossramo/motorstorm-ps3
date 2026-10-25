/* Replays captured DSP input/channel data against all GPR and LS output bytes.
 * Captures contain local game data and stay under ignored logs/. This fixture
 * rejects every unexpected channel operation rather than approximating DMA. */
#include "spu_interp.h"
#include <assert.h>
#include <time.h>
int g_spu_ls_watch_n,g_spu_ls_probe,g_wws_read_probe,g_wws_code_probe,g_spu_smc_watch,g_spu_ls_dbg;
int (*g_spu_lv2_stop_hook)(spu_context*);
void spu_ls_watch_slow(uint32_t l,int w,const uint8_t*p,uint32_t pc,uint32_t lr) { abort(); }
spu_lifted_fn spu_lifted_lookup(const spu_context*c,uint32_t a) { return 0; }
void spu_spurs_taskset_syscall(spu_context*c) { abort(); }
static unsigned char* channel_data;static size_t channel_pos,channel_size;
static u128 channel(unsigned kind,unsigned ch) {
 assert(channel_pos+24<=channel_size);unsigned h[2];u128 v;
 memcpy(h,channel_data+channel_pos,8);memcpy(&v,channel_data+channel_pos+8,16);channel_pos+=24;
 assert(h[0]==kind&&h[1]==ch);return v;
}
u128 spu_rdch(spu_context*c,uint32_t ch) {return channel(1,ch);}
void spu_wrch(spu_context*c,uint32_t ch,u128 v) {
 u128 want=channel(3,ch);assert(!memcmp(&v,&want,16));
 if(ch==21) {assert(channel_pos+2*SPU_LS_SIZE<=channel_size);
  assert(!memcmp(c->ls,channel_data+channel_pos,SPU_LS_SIZE));channel_pos+=SPU_LS_SIZE;
  memcpy(c->ls,channel_data+channel_pos,SPU_LS_SIZE);channel_pos+=SPU_LS_SIZE;}
}
uint32_t spu_rchcnt(spu_context*c,uint32_t ch) {return channel(2,ch)._u32[0];}
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
static spu_lifted_fn lifted[65536];
void spu_register_function(unsigned pc,spu_lifted_fn fn) {lifted[pc/4]=fn;}
void spu_indirect_branch(spu_context*c) {assert(lifted[c->pc/4]);g_spu_trampoline_fn=lifted[c->pc/4];}
void spu_indirect_branch_mt(spu_context*c) {spu_indirect_branch(c);}
void spu_drain_call(spu_context*c,unsigned pc) {
 unsigned budget=100000;
 while(g_spu_trampoline_fn) {
  assert(budget--);spu_lifted_fn fn=g_spu_trampoline_fn;g_spu_trampoline_fn=NULL;fn(c);
 }
 assert(c->pc==pc);
}
void spu_img_restore(spu_context*c,int image) {assert(c->image_id==image);}
void spu_halt(spu_context*c) {fprintf(stderr,"unexpected halt at %X\n",c->pc);abort();}
void spu_stop(spu_context*c,unsigned code) {abort();}
extern void ms_audio_30720_spu_recomp_register(void);
extern void ms_audio_2FEF0_spu_recomp_register(void);
extern void ms_audio_32770_spu_recomp_register(void);
extern spu_lifted_fn motorstorm_audio_lookup(const spu_context*, uint32_t);
spu_lifted_fn (*g_spu_port_overlay_lookup)(const spu_context*, uint32_t);
static spu_context before,after,ctx;
static void check_overlay_guard(void) {
 ctx=before;ctx.ls=ctx.ls_store;
 assert(ctx.image_id==286484578 && ctx.policy_mode==0);
 unsigned end=ctx.pc==0x30720?0x326bf:ctx.pc==0x2fef0?0x3038f:0x32dbf;
 assert(motorstorm_audio_lookup(&ctx,ctx.pc)==lifted[ctx.pc/4]);
 assert(motorstorm_audio_lookup(&ctx,ctx.pc+4)==NULL);
 ctx.image_id++;assert(motorstorm_audio_lookup(&ctx,ctx.pc)==NULL);ctx.image_id--;
 ctx.policy_mode=1;assert(motorstorm_audio_lookup(&ctx,ctx.pc)==NULL);ctx.policy_mode=0;
 ctx.ls[end]^=1;assert(motorstorm_audio_lookup(&ctx,ctx.pc)==NULL);
 ctx.ls[end]^=1;assert(motorstorm_audio_lookup(&ctx,ctx.pc)!=NULL);
 puts("PASS overlay identity, entry, policy and full-code mutation guards");
}
int main(int argc,char**argv) {
 assert(argc==5);ms_audio_30720_spu_recomp_register();ms_audio_2FEF0_spu_recomp_register();ms_audio_32770_spu_recomp_register();FILE*f=fopen(argv[1],"rb");assert(f&&fread(&before,sizeof before,1,f)==1);fclose(f);
 f=fopen(argv[2],"rb");assert(f&&fread(&after,sizeof after,1,f)==1);fclose(f);
 check_overlay_guard();
 printf("entry=%X expected=%X image=%d size=%zu\n",before.pc,after.pc,before.image_id,sizeof ctx);fflush(stdout);
 f=fopen(argv[3],"rb");assert(f);fseek(f,0,SEEK_END);channel_size=ftell(f);rewind(f);
 channel_data=malloc(channel_size?channel_size:1);assert(fread(channel_data,1,channel_size,f)==channel_size);fclose(f);
 unsigned repeat=100;double elapsed=0;
 for(unsigned i=0;i<repeat;i++) {
  ctx=before;ctx.ls=ctx.ls_store;channel_pos=0;g_spu_trampoline_fn=NULL;
  struct timespec t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);
#if defined(MOTORSTORM_SPU_THREAD_RTZ)
  ms_spu_exec_enter();
#endif
  if(atoi(argv[4])) {
   assert(lifted[before.pc/4]);lifted[before.pc/4](&ctx);
   while(g_spu_trampoline_fn) {spu_lifted_fn fn=g_spu_trampoline_fn;g_spu_trampoline_fn=NULL;fn(&ctx);}
  } else spu_interp_run_until(&ctx,before.pc,after.pc);
#if defined(MOTORSTORM_SPU_THREAD_RTZ)
  ms_spu_exec_leave();
#endif
  assert(channel_pos==channel_size);
  clock_gettime(CLOCK_MONOTONIC,&t1);elapsed+=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)*1e-9;
  assert(ctx.pc==after.pc&&ctx.status==after.status);
  assert(!memcmp(ctx.gpr,after.gpr,sizeof ctx.gpr));
  assert(!memcmp(ctx.ls_store,after.ls_store,sizeof ctx.ls_store));
 }
 printf("PASS all registers and local-store bytes, %.3f ms/call (%u runs)\n",elapsed*1000/repeat,repeat);
 free(channel_data);
}
