#!/usr/bin/env python3
"""Copy bounded offline diagnostics from an iPhone, or report a previous copy.
Never launches, stops, mirrors, or attaches a debugger to the game.
"""
import argparse
import json
from pathlib import Path
import subprocess


def summarize(session):
    metadata = json.loads((session / 'session.json').read_text())
    rows, incomplete = [], 0
    for path in session.glob('samples-*.jsonl'):
        for line in path.read_text(errors='replace').splitlines():
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                incomplete += 1  # abrupt shutdown may leave the final row partial
    rows.sort(key=lambda row: row['seq'])
    samples = [r for r in rows if r['type'] == 'sample' and r['active']]
    visible = [r for r in samples if r['display_fps'] is not None]
    cpu = [r for r in samples if r['cpu_percent'] is not None]
    def weighted(items, key):
        duration = sum(r['window_s'] for r in items)
        return sum(r[key] * r['window_s'] for r in items) / duration if duration else None
    audio_blocks = sum(r['audio_blocks'] for r in samples)
    role_names = sorted({name for r in samples for name in r.get('cpu_by_role_ms', {})})
    duration = sum(r['window_s'] for r in samples)
    roles = {name: sum(r.get('cpu_by_role_ms', {}).get(name, 0) for r in samples) for name in role_names}
    async_samples = [r for r in samples if 'async_dispatches' in r]
    async_count = sum(r['async_dispatches'] for r in async_samples)
    return dict(
        session=metadata['session'], device=metadata['device'], samples=len(samples),
        diagnostics_schema=metadata.get('schema', 1), cpu_role_metrics_available=bool(role_names),
        runtime_options=metadata.get('runtime_options', {}),
        build_options=metadata.get('build_options', {}),
        async_metrics_available=bool(async_samples),
        async_dispatches=async_count if async_samples else None,
        async_reuse_percent=100*sum(r['async_reused'] for r in async_samples)/async_count if async_count else None,
        async_threads_created=sum(r['async_threads_created'] for r in async_samples) if async_samples else None,
        async_launch_failures=sum(r['async_launch_failures'] for r in async_samples) if async_samples else None,
        async_launch_cpu_total_ms=sum(r['async_launch_cpu_ms'] for r in async_samples) if async_samples else None,
        async_launch_wall_total_ms=sum(r['async_launch_wall_ms'] for r in async_samples) if async_samples else None,
        measured_active_seconds=sum(r['window_s'] for r in samples),
        average_display_fps=weighted(visible, 'display_fps'),
        average_guest_fps=weighted(visible, 'guest_fps'),
        average_cpu_percent=weighted(cpu, 'cpu_percent'),
        cpu_by_role_total_ms=roles,
        cpu_by_role_percent={name: ms/(duration*10) for name, ms in roles.items()} if duration else {},
        thread_clock_errors=sum(r.get('thread_clock_errors', 0) for r in samples),
        thread_registration_drops=sum(r.get('thread_registration_drops', 0) for r in samples),
        audio_sleep_overshoot_total_ms=sum(r.get('audio_sleep_overshoot_ms', 0) for r in samples),
        audio_sleep_overshoot_max_ms=max((r.get('audio_sleep_overshoot_max_ms', 0) for r in samples), default=None),
        audio_sleep_late_5ms=sum(r.get('audio_sleep_late_5ms', 0) for r in samples),
        audio_sleep_late_20ms=sum(r.get('audio_sleep_late_20ms', 0) for r in samples),
        largest_present_interval_ms=max((r['frame_max_ms'] for r in visible), default=None),
        largest_present_age_s=max((r['last_present_age_s'] for r in visible), default=None),
        maximum_memory_mb=max((r['memory_footprint_mb'] for r in samples if r['memory_footprint_mb'] is not None), default=None),
        worst_thermal_state=max((r['thermal_state'] for r in samples), default=None),
        audio_blocks=audio_blocks,
        silent_audio_block_percent=100*sum(r['audio_silent_blocks'] for r in samples)/audio_blocks if audio_blocks else None,
        audio_empty_queue_observations=sum(r['audio_empty_queue_observations'] for r in samples),
        maximum_audio_submit_gap_ms=max((r['audio_submit_max_gap_ms'] for r in samples), default=None),
        gpu_completed_ms=sum(r['gpu_completed_ms'] for r in samples),
        gpu_errors=sum(r['gpu_errors'] for r in samples),
        gpu_wait_wall_ms=sum(r['gpu_wait_wall_ms'] for r in samples),
        shader_build_wall_ms=sum(r['shader_build_wall_ms'] for r in samples),
        pipeline_build_wall_ms=sum(r['pipeline_build_wall_ms'] for r in samples),
        logger_cpu_ms_per_sample=weighted(samples, 'logger_cpu_ms_previous_sample'),
        incomplete_rows=incomplete,
        events=[r for r in rows if r['type'] == 'event'],
        caveats=metadata['notes'] + [
            'Summary includes menus, loading and races; it is not a gameplay-only benchmark.',
            'Rotating files retain the most recent portion of a long session.',
            'Read the one-second sample rows to locate stalls; aggregate means can hide them.'
        ])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', help='Required for a fresh copy; omit with --report-only')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--report-only', action='store_true')
    args = parser.parse_args()
    if not args.report_only:
        if not args.device:
            parser.error('--device is required to copy logs')
        if args.out.exists():
            parser.error('Use a fresh output directory to preserve earlier evidence')
        args.out.mkdir(parents=True)
        subprocess.run(['xcrun', 'devicectl', 'device', 'copy', 'from', '--device', args.device,
                        '--source', 'Documents/logs/performance', '--destination', str(args.out / 'performance'),
                        '--domain-type', 'appDataContainer', '--domain-identifier', 'com.lithium.motorstorm'], check=True)
    summaries = []
    for path in sorted((args.out / 'performance').glob('session-*/session.json')):
        report = summarize(path.parent)
        (path.parent / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
        summaries.append(report)
    if not summaries:
        parser.error('No offline sessions found; launch the updated app and press Start game first')
    (args.out / 'summary.json').write_text(json.dumps(summaries, indent=2) + '\n')
    print(f'Read {len(summaries)} session(s); summaries saved in {args.out}. No game process was started or stopped.')


if __name__ == '__main__':
    main()
