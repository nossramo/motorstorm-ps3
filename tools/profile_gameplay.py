#!/usr/bin/env python3
import os,subprocess,time,pathlib
import argparse
parser=argparse.ArgumentParser(description='Bounded gameplay trace followed by a separate CPU sample.')
parser.add_argument('--out',type=pathlib.Path,required=True)
parser.add_argument('--capture',action='store_true',help='Save two Metal frames for Xcode replay instead of CPU timing/sampling')
parser.add_argument('--capture-frames',type=int,default=2,help='Number of consecutive complete Metal frames (1–120)')
parser.add_argument('--capture-trigger',type=pathlib.Path,help='Wait for this file to contain 1 before capturing; observe current.ppm and use pad.txt to reach gameplay first')
parser.add_argument('--seconds',type=int,default=185,help='Bounded run length; allow more time for manually triggered capture')
args=parser.parse_args()
if not 1 <= args.capture_frames <= 120: parser.error('--capture-frames must be 1–120')
if args.capture_trigger and not args.capture: parser.error('--capture-trigger requires --capture')
root=pathlib.Path(__file__).resolve().parents[1];out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
pad=out/'pad.txt';pad.write_text('0x0000 0\n')
env=dict(os.environ,MOTORSTORM_NATIVE_JOBS='1',RSX_ENGINE_CPU_READBACK='demand',GCM_SEMA_ACQUIRE='1',PS3RECOMP_METAL_FPS='1',PS3_FTRACE=f'140:25:{out}/trace.txt',PS3_FTRACE_MIN_US='0',PAD_FILE=str(pad),PAD_SCRIPT='5:0x4000,15:0x4000,25:0x4000,35:0x4000,45:0x4000,49:0x4000,53:0x4000,57:0x20,61:0x20,65:0x4000,80:0x4000')
if args.capture:
 if (out/'race.gputrace').exists(): parser.error('Capture output already exists; choose a fresh --out directory')
 env.update(MTL_CAPTURE_ENABLED='1',PS3RECOMP_METAL_GPU_TRACE=str(out/'race.gputrace'),PS3RECOMP_METAL_GPU_TRACE_AFTER='140',PS3RECOMP_METAL_GPU_TRACE_FRAMES=str(args.capture_frames))
 env.pop('PS3_FTRACE',None)
 env['MOTORSTORM_GPU_READ_TIMEOUT_SECONDS']='120'
 if args.capture_trigger:
  trigger=args.capture_trigger.resolve()
  if trigger.exists(): parser.error('Use a fresh trigger path to avoid capturing a stale request')
  env.update(PS3RECOMP_METAL_GPU_TRACE_TRIGGER=str(trigger),PS3RECOMP_METAL_GPU_TRACE_AFTER='0',PS3RECOMP_METAL_FRAME_DUMP=str(out/'current.ppm'))
  print(f'Observe {out}/current.ppm; once gameplay is visible, write 1 to {trigger}',flush=True)
p=subprocess.Popen(['python3','tools/run_probe.py','--seconds',str(args.seconds),'--log',str(out/'native.log')],cwd=root,env=env)
t=time.monotonic();gas=False;sample=False
while p.poll() is None:
 elapsed=time.monotonic()-t
 if elapsed>=130 and not gas and not args.capture_trigger:pad.write_text('0x0200 2000\n');gas=True
 if elapsed>=168 and not sample and not args.capture:
  pid=(out/'native.pid').read_text().strip();subprocess.Popen(['sample',pid,'5','1','-file',str(out/'stacks.txt')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);sample=True
 time.sleep(.5)
