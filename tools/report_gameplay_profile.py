#!/usr/bin/env python3
"""Analyze measured gameplay intervals; overlapping lanes are not additive.
CPU counters spanning a frame boundary are apportioned by wall overlap. GPU
intervals are intersected directly. Unattributed CPU is not labelled physics.
"""
import argparse, collections, json, math, re, statistics
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('trace',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
spans=[];header=''
for line in a.trace.read_text().splitlines():
 if line.startswith('#'):header=line;continue
 if line.startswith('T '):continue
 k,w,x,y,t0,t1=line.split();spans.append((int(k),int(w),int(x,16),int(y,16),float(t0)/1000,float(t1)/1000))
spans.sort(key=lambda s:s[4])
# Convert cumulative Havok-worker CPU samples to measured intervals.
workers=collections.defaultdict(list)
for s in spans:
 if s[0]==21:workers[s[1]].append(s)
for records in workers.values():
 for l,r in zip(records,records[1:]):
  delta=(r[3]<<32|r[2])-(l[3]<<32|l[2])
  if delta>=0:spans.append((23,l[1],delta,0,l[4],r[4]))
by_kind=collections.defaultdict(list)
# Caller CPU is sampled at exactly the same flip boundaries as process CPU.
# Keep per-thread counters separate; callbacks may run on another PPU thread.
flip_callers=collections.defaultdict(list)
for s in spans:
 if s[0]==40:flip_callers[s[1]].append(s)
for tid,records in flip_callers.items():
 for l,r in zip(records,records[1:]):
  delta=(r[3]<<32|r[2])-(l[3]<<32|l[2])
  if delta>=0 and tid==1:spans.append((140,tid,delta,0,l[4],r[4]))
for s in spans:by_kind[s[0]].append(s)
flips=[s for s in spans if s[0]==18]
window = re.search(r'start_us=([\d.]+) end_us=([\d.]+)', header)
window_start, window_end = (float(x)/1000 for x in window.groups()) if window else (flips[0][4], flips[-1][4])
def overlap(s,lo,hi):return max(0,min(s[5],hi)-max(s[4],lo))
def union_ms(intervals):
 merged=[]
 for lo,hi in sorted(intervals):
  if hi<=lo:continue
  if merged and lo<=merged[-1][1]:merged[-1][1]=max(hi,merged[-1][1])
  else:merged.append([lo,hi])
 return sum(hi-lo for lo,hi in merged)
def cpu(kind,lo,hi):
 return sum(s[2]/1e6*(overlap(s,lo,hi)/(s[5]-s[4]) if s[5]>s[4] else int(lo<=s[4]<hi)) for s in by_kind[kind] if s[4]<=hi and s[5]>=lo)
def wall(kinds,lo,hi):return union_ms([(max(lo,s[4]),min(hi,s[5])) for k in kinds for s in by_kind[k] if s[4]<=hi and s[5]>=lo])
frames=[]
for left,right in zip(flips,flips[1:]):
 lo,hi=left[4],right[4];total=((right[3]<<32|right[2])-(left[3]<<32|left[2]))/1e6
 rsx=cpu(17,lo,hi);enc=cpu(19,lo,hi);interp=cpu(16,lo,hi);spin=cpu(20,lo,hi);policy=cpu(22,lo,hi);havok=cpu(23,lo,hi)
 frames.append(dict(start_ms=lo,end_ms=hi,frame_ms=hi-lo,process_cpu_ms=total,
  rsx_total_cpu_ms=rsx,metal_encode_cpu_ms=enc,
  spu_interp_cpu_ms=interp,spu_interp_wall_ms=wall({15},lo,hi),job_poll_cpu_ms=spin,
  job_poll_wall_ms=wall({20},lo,hi),spu_policy_cpu_ms=policy,havok_worker_cpu_ms=havok,
  other_cpu_ms=total-rsx-spin-havok-(policy if policy else interp),
  gpu_execution_ms=wall({14},lo,hi),render_gpu_wait_ms=wall({12},lo,hi),
  render_readback_wall_ms=wall({13},lo,hi),rsx_acquire_wait_ms=wall({5,6},lo,hi),
  backend_present_wall_ms=wall({10},lo,hi),
  spu_semaphore_delay_cpu_ms=cpu(39,lo,hi),
  cpu_gpu_read_request_wait_ms=wall({25},lo,hi),
  flip_wait_ms=wall({26},lo,hi),
  shader_build_wall_ms=wall({30},lo,hi),shader_build_cpu_ms=cpu(30,lo,hi),
  shader_translate_wall_ms=wall({35},lo,hi),shader_translate_cpu_ms=cpu(35,lo,hi),
  shader_metal_wall_ms=wall({36},lo,hi),shader_metal_cpu_ms=cpu(36,lo,hi),
  pipeline_build_wall_ms=wall({31},lo,hi),pipeline_build_cpu_ms=cpu(31,lo,hi),
  main_condition_wait_ms=union_ms([(max(lo,s[4]),min(hi,s[5])) for s in by_kind[1] if s[1]==1 and s[2]==107 and s[4]<=hi and s[5]>=lo]),
  rsx_drain_wall_ms=wall({7},lo,hi),metal_encode_wall_ms=wall({11},lo,hi)))
 if by_kind[140]:
  frames[-1].update(main_ppu_cpu_ms=cpu(140,lo,hi),
   main_not_on_cpu_ms=hi-lo-cpu(140,lo,hi),
   main_syscalls_wall_ms=union_ms([(max(lo,s[4]),min(hi,s[5])) for s in by_kind[1] if s[1]==1]),
   main_usleep_merged_wall_ms=union_ms([(max(lo,s[4]),min(hi,s[5])) for s in by_kind[2] if s[1]==1]))
if not frames:
 result=dict(header=header,valid_gameplay_timing=False,frames=0,flips=len(flips),
             average_fps=None,measurement_window_ms=window_end-window_start,
             reason='Fewer than two guest flips in the measurement window; no valid gameplay FPS or frame-time breakdown. Inspect native.log and stacks.txt.')
 a.out.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
 raise SystemExit(2)
keys=[k for k in frames[0] if k not in ('start_ms','end_ms')]
means={k:statistics.mean(f[k] for f in frames) for k in keys};d=sorted(f['frame_ms'] for f in frames)
representative=min(frames,key=lambda f:abs(f['frame_ms']-statistics.median(d)))
by_interp=collections.defaultdict(lambda:dict(cpu_ms=0,wall_ms=0,calls=0))
for s in spans:
 if s[0]==16:
  z=by_interp[f'image={s[1]} entry=0x{s[3]:05X}'];z['cpu_ms']+=s[2]/1e6;z['calls']+=1;z['wall_ms']+=s[5]-s[4]
result=dict(header=header,frames=len(frames),average_fps=1000/means['frame_ms'],median_ms=statistics.median(d),p95_ms=d[math.ceil(len(d)*.95)-1],min_ms=d[0],max_ms=d[-1],mean=means,representative_frame=representative,interpreter=sorted(by_interp.items(),key=lambda x:-x[1]['cpu_ms']),per_frame=frames,
 caveats=['CPU ms sum across threads and can exceed elapsed frame ms.','GPU and CPU overlap; do not add lanes.', 'RSX FIFO CPU includes parsing, state, resource conversion and some Metal encoding; the separately measured encoding value overlaps it.', 'SPU policy CPU includes native jobs, interpreter fallback and policy-host work; do not add interpreter CPU again.', 'Havok worker CPU includes worker overhead and is not the complete game physics cost.','CPU spans crossing frame boundaries are apportioned by wall overlap.','Other CPU includes native guest code, physics, driver threads and uninstrumented host work; physics is not independently identified.','Interpreter calls that escape through longjmp do not reach the return probe.','SPU channel busy spans are wall activity, not CPU utilization.','GPU timestamps mapped from host uptime to CLOCK_MONOTONIC at completion.'])
# A final stall has no closing flip and therefore is absent from interval
# statistics. Include the entire requested window in the headline throughput.
result['frame_interval_fps'] = result['average_fps']
result['average_fps'] = len(flips) * 1000 / (window_end-window_start)
result['measurement_window_ms'] = window_end-window_start
result['fifo_completed_frames'] = len(by_kind[8])
result['fifo_completed_fps'] = len(by_kind[8]) * 1000 / (window_end-window_start)
result['backend_present_calls'] = len(by_kind[10])
result['backend_present_rate'] = len(by_kind[10]) * 1000 / (window_end-window_start)
result['spu_semaphore_delay']={}
for mode, label in ((0,'original_or_guard_fallback'),(1,'already_invalid'),
                    (2,'notified_or_spurious'),(3,'timeout')):
 records=[s for s in by_kind[39] if s[3]==mode]
 result['spu_semaphore_delay'][label]=dict(calls=len(records),
  cpu_ms=sum(s[2] for s in records)/1e6,
  aggregate_wall_ms=sum(s[5]-s[4] for s in records))
result['caveats'].append('SPU semaphore delay CPU measures image 507917221 at 0xAAE8; it is nested in other existing CPU totals, not an additive lane. Wakeup always retries the guest semaphore; it does not authorize progress.')
result['first_flip_delay_ms'] = flips[0][4]-window_start
result['last_flip_to_window_end_ms'] = window_end-flips[-1][4]
pending_callbacks={}; callback_delays=collections.defaultdict(list); coalesced=collections.Counter()
for s in sorted(by_kind[28]+by_kind[29], key=lambda s:s[4]):
 for bit in (1,2):
  if not s[2]&bit:continue
  if s[0]==28:
   if bit in pending_callbacks:coalesced[bit]+=1
   else:pending_callbacks[bit]=s[4]
  elif bit in pending_callbacks:callback_delays[bit].append(s[4]-pending_callbacks.pop(bit))
result['callback_delivery']={}
result['vblank_notifications_queued']=sum(bool(s[2]&1) for s in by_kind[28])
vblank_times=sorted(s[4] for s in by_kind[28] if s[2]&1)
vblank_intervals=sorted(r-l for l,r in zip(vblank_times,vblank_times[1:]))
if vblank_intervals:
 result['vblank_publish_intervals_ms']=dict(samples=len(vblank_intervals),
  mean=statistics.mean(vblank_intervals),median=statistics.median(vblank_intervals),
  p95=vblank_intervals[math.ceil(len(vblank_intervals)*.95)-1],max=vblank_intervals[-1],
  over_25_ms=sum(d>25 for d in vblank_intervals),below_8_ms=sum(d<8 for d in vblank_intervals))
result['vblank_callbacks_returned']=len(by_kind[42])
result['vblank_callback_rate']=len(by_kind[42])*1000/(window_end-window_start)
result['flip_cpu_callers']={str(tid):len(records) for tid,records in flip_callers.items()}
result['caveats'].append('Main PPU CPU is a caller-thread counter at flip requests, including its guest and host work. Main not-on-CPU time includes waiting and preemption; it is not all synchronization. Merged usleep spans include inter-poll gaps, and all these lanes overlap other totals. Older captures lack caller CPU and returned-callback probes.')
result['caveats'].append('Coalesced notifications count publications sharing one pending bit, not necessarily lost callbacks: counted-vblank mode delivers every counted tick. Returned callbacks and queued notifications can differ at recording boundaries. Delivery latency measures the pending-bit claim, not each individual callback, and older captures lack returned-callback probes.')
for bit,delays in callback_delays.items():
 delays.sort()
 result['callback_delivery']['vblank' if bit==1 else 'flip']=dict(
  samples=len(delays),mean_ms=statistics.mean(delays),p95_ms=delays[math.ceil(len(delays)*.95)-1],
  max_ms=max(delays),coalesced_notifications=coalesced[bit])
result['shader_build_events']=len(by_kind[30])
result['pipeline_build_events']=len(by_kind[31])
result['shader_translation_cache']=dict(requests=len(by_kind[37]),
 disk_hits=sum(s[2]==1 for s in by_kind[37]),misses=sum(s[2]==0 for s in by_kind[37]))
phase_names={1:'geometry',2:'targets',3:'pipeline_lookup',4:'textures',5:'staging_submit',
 6:'geometry_layout',7:'geometry_references',8:'geometry_remap',9:'geometry_fetch_plan',
 10:'geometry_pack',11:'geometry_cpu_decode',12:'stage_indices_constants',
 13:'stage_prepare_flush',14:'stage_bindings_constants',15:'stage_geometry_record',
 16:'pipeline_vertex_hash',17:'surface_seed',18:'surface_create'}
phase_records=collections.defaultdict(list)
for s in by_kind[33]:phase_records[s[1]].append(s)
result['draw_phases']={}
for phase,records in phase_records.items():
 result['draw_phases'][phase_names.get(phase,str(phase))]=dict(
  frame_records=len(records),cpu_ms_per_record=statistics.mean(s[2]/1e6 for s in records),
  wall_ms_per_record=statistics.mean(s[3]/1e6 for s in records),
  calls=sum(s[2] for s in by_kind[34] if s[1]==phase))
result['slow_draw_phases']=[dict(phase=phase_names.get(s[1],str(s[1])),
 start_ms=s[4],wall_ms=s[5]-s[4],cpu_ms=s[2]/1e6,guest_frame=s[3])
 for s in sorted(by_kind[32],key=lambda s:s[4]-s[5])[:20]]
submit_names={1:'command_buffer_commit',2:'resource_retirement',3:'autorelease_pool_drain',
              4:'diagnostic_frame_dump',5:'capture_boundary',6:'drawable_acquisition'}
result['submission_stages']={}
for stage,name in submit_names.items():
 records=[s for s in by_kind[38] if s[1]==stage]
 if records:
  result['submission_stages'][name]=dict(calls=len(records),
   cpu_ms=sum(s[2]/1e6 for s in records),
   wall_ms=sum(s[5]-s[4] for s in records),
   max_wall_ms=max(s[5]-s[4] for s in records))
result['slow_submission_stages']=[dict(stage=submit_names.get(s[1],str(s[1])),
 start_ms=s[4],wall_ms=s[5]-s[4],cpu_ms=s[2]/1e6,present=bool(s[3]))
 for s in sorted(by_kind[38],key=lambda s:s[4]-s[5])[:12]]
result['caveats'].append('Interval averages/percentiles exclude unfinished stalls; average_fps includes the full measurement window. Inspect last_flip_to_window_end_ms for a final stall.')
result['caveats'].append('Shader/pipeline build spans are part of RSX time, not additional work. Earlier binaries did not emit these probes; absent events do not prove compilation was absent in an older capture.')
result['caveats'].append('Shader translation and Metal function creation are nested within shader-build time. Cache request/hit counts cover only newly requested backend functions in the recording window, not all draws or startup.')
result['caveats'].append('Backend present and submission stages include host-side work outside GPU execution. Their intervals overlap existing RSX/encode/wait lanes; do not add them. Stage probes are absent from older captures.')
if phase_records:result['caveats'].append('Opt-in draw-phase diagnostics add CPU-clock overhead. Aggregates cover completed phases and can omit rejected draws; kind 32 retains only phases >=100 microseconds. Do not add these nested phases to RSX time.')
if any(k>=6 for k in phase_records):result['caveats'].append('Detail phases 6-11 are nested in geometry, 12-15 in staging_submit, and 16 in pipeline_lookup. They include extra CPU-clock overhead; prepare_flush may include Metal encoding/submission. CPU decode is fallback-only. Do not sum details with their enclosing phases.')
hash_reasons={0:'cached_seed',1:'rebuilt_metadata',2:'changed_seed',3:'no_metadata'}
if by_kind[1004]:
 result['fragment_hash_probe']={}
 for reason,label in hash_reasons.items():
  counts=[s for s in by_kind[1004] if s[1]==reason]
  timing=[s for s in by_kind[1005] if s[1]==reason]
  result['fragment_hash_probe'][label]=dict(calls=sum(s[2] for s in counts),
   source_bytes=sum(s[3] for s in counts),cpu_ms=sum(s[2] for s in timing)/1e6,
   cpu_timed=bool(reason))
 result['caveats'].append('Fragment-hash probes time only recomputation, include CPU-clock overhead, and are nested in RSX CPU. Cached-seed CPU is unmeasured. No-metadata source bytes include inline constants; other byte counts exclude them. Per-present aggregates may include a partial boundary frame.')
metadata_reasons={0:'new_address_reusable',1:'changed_address_reusable',
                  2:'new_address_unmatched',3:'changed_address_unmatched'}
if by_kind[1006]:
 result['fragment_metadata_probe']={}
 for reason,label in metadata_reasons.items():
  counts=[s for s in by_kind[1006] if s[1]==reason]
  timing=[s for s in by_kind[1007] if s[1]==reason]
  result['fragment_metadata_probe'][label]=dict(calls=sum(s[2] for s in counts),
   source_bytes=sum(s[3] for s in counts),search_cpu_ms=sum(s[2] for s in timing)/1e6)
 result['caveats'].append('Metadata reuse probes search exact resident instruction contents. The probe alone retains the old policy; RSX_FP_RELOCATE_CACHE separately enables reuse. New address means not currently resident, not necessarily never seen. Unmatched content may be new or evicted. Search CPU includes diagnostic clocks and is nested in RSX CPU.')
a.out.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='per_frame'},indent=2))
