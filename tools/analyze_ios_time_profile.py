#!/usr/bin/env python3
"""Split an exported iPhone Time Profiler table by thread role, core type and cost kind.

Input is the XML written by
`xctrace export --input X.trace --xpath '/trace-toc/run[1]/data/table[@schema="time-profile"]'`.
Output separates P-core from E-core time and native job/DSP work from lock,
yield, condition-variable, dispatch, MFC and copy overhead. Unnamed system
frames are symbolicated with atos against matching-UUID Xcode DeviceSupport
libraries when available.

Sample weights are CPU time summed across threads, not frame latency. Only
Running samples are counted, so blocked time is invisible: use a System Trace
or wall-clock counters for critical-path waits.

Usage:
  python3 tools/analyze_ios_time_profile.py logs/X/time-profile.xml PID \
      [--e-core-ratio 0.35] [--json logs/X/core-split.json] \
      [--app-binary 'saved.app/MotorStorm iOS']
"""
import argparse
import collections
import glob
import json
import os
import subprocess
import xml.etree.ElementTree as ET

# Game-specific markers: the Multistream audio image hash and the Havok PPU
# worker entry. Matched against any frame of the sample's stack.
AUDIO_IMAGE_MARKER = '1136862334bb'
PHYSICS_ENTRY_MARKER = 'func_00B9FCD0'

ROLE_RULES = [
    ('spurs_worker', lambda n: 'spurs_pm_worker' in n),
    ('spu_audio', lambda n: any(AUDIO_IMAGE_MARKER in f for f in n)),
    ('spu_async_other', lambda n: 'spu_async_thread' in n),
    ('main_game', lambda n: 'runGame(void*)' in n or 'motorstorm_main' in n),
    ('render', lambda n: 'frame_clock(void*)' in n),
    ('physics', lambda n: any(f.startswith(PHYSICS_ENTRY_MARKER) for f in n)),
    ('spurs_coordinator', lambda n: 'spurs_kernel_thread' in n),
    ('ppu_other', lambda n: 'ppu_host_thread_proc' in n),
    ('system_dispatch', lambda n: any(f.startswith('_dispatch_') for f in n)),
]

LOCK_WAIT = {'__ulock_wait2', '__ulock_wait', '__ulock_wake', '_os_unfair_lock_lock_slow',
             'os_unfair_lock_lock', 'os_unfair_lock_unlock', '__psynch_mutexwait',
             '__psynch_mutexdrop', '__psynch_rw_wrlock', '__psynch_rw_rdlock',
             '__psynch_rw_unlock', 'DYLD-STUB$$os_unfair_lock_lock',
             'DYLD-STUB$$os_unfair_lock_unlock', 'spu_lockline_lock'}
LOCK_WAIT.update({'spu_lockline_acquire_mask', 'spu_lockline_release_mask',
                  'spu_lockline_lock_range', 'spu_lockline_lock_pair'})
YIELD_SPIN = {'swtch_pri', 'sched_yield', 'thread_switch', 'cellSyncMutexLock'}
COND_WAIT = {'__psynch_cvwait', '__psynch_cvbroad', '__psynch_cvsignal', '__semwait_signal',
             'semaphore_wait_trap', 'semaphore_timedwait_trap', 'semaphore_signal_trap'}
DISPATCH = {'spu_task_launch_check', 'spu_drain_call', 'spu_check_stack_reset',
            'spu_indirect_branch', 'spu_lookup', 'spu_depth_guard', 'yz_lockstep_tick',
            '_tlv_get_addr', 'spu_img_restore', 'spu_run_with_halt', 'ps3_indirect_call_impl(ppu_context*)'}
MFC = {'mfc_for', 'mfc_do_transfer', 'mfc_channel_write', 'spu_wrch', 'spu_rdch',
       'gcm_note_spu_put', 'spu_coh_notify_write', 'spu_coh_notify_write_except'}
GUEST_MEMORY = {'vm_write8', 'vm_write16', 'vm_write32', 'vm_write64', 'vm_read8', 'vm_read16',
                'vm_read32', 'vm_read64', 'spu_coh_is_reserved'}
COPY = {'_platform_memmove', '_platform_memset', '__bzero', '_platform_memcmp'}


def classify(leaf, binary, names):
    if leaf in LOCK_WAIT:
        return 'lock_wait'
    if leaf in YIELD_SPIN:
        return 'yield_spin'
    if leaf in COND_WAIT:
        return 'cond_wait'
    if leaf in DISPATCH:
        return 'spu_dispatch'
    if leaf in MFC:
        return 'mfc_channel'
    if leaf in GUEST_MEMORY:
        return 'guest_memory'
    if leaf in COPY:
        return 'copy'
    if leaf.startswith('vmx_') or leaf.startswith('_ZL') and 'vmx_' in leaf:
        return 'vmx_helper'
    if binary and (binary.startswith('AGX') or binary in ('Metal', 'IOKit', 'IOGPU')) \
            or leaf == 'iokit_user_client_trap':
        return 'metal_driver'
    if any(f.startswith('jobmod_') or f.startswith('ms_cull') for f in names):
        return 'native_job'
    if leaf.startswith('ms_audio_') or AUDIO_IMAGE_MARKER in leaf or leaf.startswith('spu__'):
        return 'spu_arith'
    if leaf.startswith('func_') or leaf.startswith('_Z13func_'):
        return 'ppu_guest_code'
    if binary == 'MotorStorm iOS':
        return 'other_app'
    return 'other_system'


def device_support_roots():
    base = os.path.expanduser('~/Library/Developer/Xcode/iOS DeviceSupport')
    return sorted(glob.glob(os.path.join(base, '*', '*', 'Symbols')) +
                  glob.glob(os.path.join(base, '*', 'Symbols')))


def file_uuid(path):
    out = subprocess.run(['dwarfdump', '--uuid', path], capture_output=True, text=True).stdout
    return {line.split()[1] for line in out.splitlines() if line.startswith('UUID:')}


def symbolicate(unresolved, app_binary=None):
    """unresolved: {(path, uuid, load): set(addr)} -> {addr: name}"""
    names, roots = {}, device_support_roots()
    for (path, uuid, load), addrs in unresolved.items():
        is_app = path and path.endswith('/MotorStorm iOS')
        if is_app and app_binary:
            if uuid not in file_uuid(app_binary):
                raise SystemExit(f'App binary UUID does not match trace UUID {uuid}')
            candidates = [app_binary]
        elif path:
            candidates = [path] if path.startswith('/Users') else [r + path for r in roots]
        else:
            candidates = []
        local = next((c for c in candidates if os.path.exists(c) and uuid in file_uuid(c)), None)
        if not local:
            continue
        addrs = sorted(addrs)
        out = subprocess.run(['atos', '-arch', 'arm64' if is_app else 'arm64e', '-o', local, '-l', load, *addrs],
                             capture_output=True, text=True).stdout.splitlines()
        for addr, line in zip(addrs, out):
            if ' (in ' in line:
                names[addr] = line.split(' (in ')[0]
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('xml')
    ap.add_argument('pid', type=int)
    ap.add_argument('--e-core-ratio', type=float, default=0.35,
                    help='assumed E-core throughput relative to a P core, for P-equivalent totals')
    ap.add_argument('--json')
    ap.add_argument('--app-binary', help='matching unstripped MotorStorm iOS executable for unnamed app frames')
    ap.add_argument('--top', type=int, default=8)
    ap.add_argument('--guest-frames', type=int,
                    help='guest frames in exactly this trace window; omit if not measured')
    args = ap.parse_args()
    if args.guest_frames is not None and args.guest_frames <= 0:
        ap.error('--guest-frames must be positive and match the trace window')

    root = ET.parse(args.xml).getroot()
    ids = {e.attrib['id']: e for e in root.iter() if 'id' in e.attrib}

    def res(e):
        while e is not None and 'ref' in e.attrib:
            e = ids[e.attrib['ref']]
        return e

    samples, unresolved = [], collections.defaultdict(set)
    first, last = float('inf'), 0.0
    core_ms = collections.Counter()
    for node in root.findall('node'):
        schema = node.find('schema')
        if schema is None or schema.get('name') != 'time-profile':
            continue
        cols = [c.findtext('mnemonic') for c in schema]
        for row in node.findall('row'):
            c = dict(zip(cols, row))
            if int(res(res(c['process']).find('pid')).text) != args.pid:
                continue
            if res(c['thread-state']).text != 'Running':
                continue
            ms = float(res(c['weight']).text) / 1e6
            t = float(res(c['time']).text) / 1e9
            first, last = min(first, t), max(last, t)
            core_node = res(c.get('core'))
            core = core_node.get('fmt', '') if core_node is not None else ''
            if 'P Core' not in core and 'E Core' not in core:
                ap.error(f'Running sample has no P/E core label ({core!r}); '
                         'cannot report core placement from this export')
            core_ms[core] += ms
            kind = 'P' if 'P Core' in core else 'E'
            tid = res(res(c['thread']).find('tid')).text
            stack = res(c['stack'])
            frames = [res(f) for f in (stack.findall('frame') if stack is not None else [])]
            names, bins = [], []
            for f in frames:
                b = res(f.find('binary'))
                names.append(f.get('name', f.get('addr')))
                bins.append(b.get('name') if b is not None else None)
                unnamed = 'name' not in f.attrib or f.get('name').startswith('0x')
                if unnamed and b is not None and (b.get('name') != 'MotorStorm iOS' or args.app_binary):
                    unresolved[(b.get('path'), b.get('UUID'), b.get('load-addr'))].add(f.get('addr'))
            samples.append((tid, kind, ms, names, bins))

    if not samples:
        ap.error(f'No Running samples for PID {args.pid}; check the export and PID')
    sym = symbolicate(unresolved, args.app_binary)
    duration = last - first if samples else 0.0
    k = args.e_core_ratio
    roles = {}
    vmx_callers = collections.defaultdict(collections.Counter)
    memory_callers = collections.defaultdict(collections.Counter)
    threads = collections.defaultdict(lambda: collections.Counter())
    for tid, kind, ms, names, bins in samples:
        names = [sym.get(n, n) for n in names]
        role = next((r for r, rule in ROLE_RULES if rule(names)), 'other') if names else 'no_stack'
        leaf = names[0] if names else '<no stack>'
        binary = bins[0] if bins else None
        cat = classify(leaf, binary, names) if names else 'no_stack'
        if cat in ('vmx_helper', 'guest_memory'):
            # Static VMX helpers have the same name in many generated units,
            # and out-of-line guest memory calls come from every unit.
            # Preserve the guest caller so a hot helper or store path can be
            # optimized in the units actually used by this trace.
            caller = next((n for n in names[1:] if n.startswith(('func_', '_Z13func_'))), '<unknown>')
            (vmx_callers if cat == 'vmx_helper' else memory_callers)[(role, leaf, caller)][kind] += ms
        app = next((n for n, b in zip(names, bins) if b == 'MotorStorm iOS'), None)
        r = roles.setdefault(role, {'ms': 0.0, 'P_ms': 0.0, 'E_ms': 0.0,
                                    'categories': collections.Counter(),
                                    'P_categories': collections.Counter(),
                                    'E_categories': collections.Counter(),
                                    'P_leaves': collections.Counter(),
                                    'leaves': collections.Counter(), 'threads': set()})
        r['ms'] += ms
        r[kind + '_ms'] += ms
        r['categories'][cat] += ms
        r[kind + '_categories'][cat] += ms
        if kind == 'P':
            r['P_leaves'][leaf if binary == 'MotorStorm iOS' or not app else f'{leaf} <- {app}'] += ms
        r['leaves'][leaf if binary == 'MotorStorm iOS' or not app else f'{leaf} <- {app}'] += ms
        r['threads'].add(tid)
        threads[tid][kind] += ms

    total = sum(r['ms'] for r in roles.values())
    p_total = sum(r['P_ms'] for r in roles.values())
    e_total = sum(r['E_ms'] for r in roles.values())
    p_cores = sum(1 for c in core_ms if 'P Core' in c)
    e_cores = sum(1 for c in core_ms if 'E Core' in c)
    print(f'pid {args.pid}: {duration:.2f} s, {total:.0f} ms running CPU, '
          f'{len(threads)} threads, {len(sym)} frames symbolicated')
    if duration:
        print(f'P cores: {p_total / duration / 1000:.2f} busy of {p_cores}; '
              f'E cores: {e_total / duration / 1000:.2f} busy of {e_cores}')
        for core, ms in sorted(core_ms.items()):
            print(f'  {core}: {100 * ms / duration / 1000:.0f}% busy')
    print(f'P-equivalent assumes one E core = {k:.2f} P core.')
    report = {'pid': args.pid, 'duration_s': duration, 'running_ms': total,
              'e_core_ratio_assumed': k, 'cores_ms': dict(core_ms), 'roles': {},
              'guest_frames': args.guest_frames,
              'caveats': ['Running samples only; blocked time is not visible.',
                          'CPU time summed across threads, not frame latency.',
                          'P-equivalent depends on the assumed E-core ratio.']}
    report['vmx_callers'] = [
        {'role': key[0], 'helper': key[1], 'caller': key[2], 'P_ms': value['P'], 'E_ms': value['E']}
        for key, value in sorted(vmx_callers.items(), key=lambda kv: -kv[1]['P'])]
    report['guest_memory_callers'] = [
        {'role': key[0], 'helper': key[1], 'caller': key[2], 'P_ms': value['P'], 'E_ms': value['E']}
        for key, value in sorted(memory_callers.items(), key=lambda kv: -kv[1]['P'])]
    if duration < 5:
        warning = 'Less than five seconds of samples: diagnostic fragment, not a stable performance baseline.'
        print('WARNING: ' + warning)
        report['caveats'].append(warning)
    for role, r in sorted(roles.items(), key=lambda kv: -kv[1]['P_ms']):
        p_eq = r['P_ms'] + k * r['E_ms']
        print(f"\n== {role}: {r['ms']:.0f} ms ({100 * r['ms'] / total:.1f}%), "
              f"{len(r['threads'])} threads, P {100 * r['P_ms'] / r['ms']:.0f}% / E "
              f"{100 * r['E_ms'] / r['ms']:.0f}%, {p_eq / duration / 1000 if duration else 0:.2f} P-eq cores")
        for cat, ms in sorted(r['categories'].items(), key=lambda kv: -r['P_categories'][kv[0]]):
            print(f'   {ms:8.0f} ms {100 * ms / r["ms"]:5.1f}%  '
                  f'P {r["P_categories"][cat]:7.0f} ms / E {r["E_categories"][cat]:7.0f} ms  {cat}')
        print('   top leaves: ' + '; '.join(f'{n[:60]} {v:.0f}' for n, v in r['leaves'].most_common(args.top)))
        report['roles'][role] = {
            'ms': r['ms'], 'P_ms': r['P_ms'], 'E_ms': r['E_ms'], 'P_equivalent_ms': p_eq,
            'threads': len(r['threads']), 'categories': dict(r['categories']),
            'P_categories_ms': dict(r['P_categories']), 'E_categories_ms': dict(r['E_categories']),
            'P_top_leaves': r['P_leaves'].most_common(40),
            'P_ms_per_guest_frame': r['P_ms']/args.guest_frames if args.guest_frames else None,
            'top_leaves': r['leaves'].most_common(40)}
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(report, f, indent=1)
        print(f'\nwrote {args.json}')


if __name__ == '__main__':
    main()
