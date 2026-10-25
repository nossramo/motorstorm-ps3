#!/usr/bin/env python3
"""Differential replay of the production filter loop; optional sanitizers."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import specialize_audio_filter as filter


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--thinlto', action='store_true', help='Match the iOS ThinLTO compile/link policy')
    parser.add_argument('--capture', type=Path, help='Private DSP capture directory with 32770-{before,after,channels}.bin')
    args = parser.parse_args()
    os.environ.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    source = filter.checked_source()
    generated = filter.generate(source)
    injection = '        if (ms_audio_filter_enabled()) ms_audio_filter_prefix(ctx);\n'
    assert filter.generate_legacy(source).removeprefix('#include "spu_audio_filter.h"\n').replace(injection, '') == source
    with tempfile.TemporaryDirectory(prefix='motorstorm-audio-filter-') as folder:
        work = Path(folder)
        (work / 'audio_filter_reference.inc').write_text(
            'static __attribute__((noinline)) void reference_loop(spu_context* ctx) {\n' +
            filter.loop_body(source) + '}\n')
        command = ['clang', '-O2', '-std=gnu17', '-UNDEBUG', '-fno-strict-aliasing',
            '-fwrapv', '-ffp-contract=off', '-DSPU_LS_NEON', '-DSPU_LS_DIAGNOSTICS=0',
            '-DMOTORSTORM_SPU_THREAD_RTZ', '-include', filter.ROOT / 'port/spu_thread_rtz.h',
            '-include', filter.ROOT / 'port/spu_shufb_neon.h',
            '-I' + str(work), '-I' + str(filter.ROOT / 'port'),
            '-I' + str(filter.ROOT / 'vendor/ps3recomp/runtime/spu'),
            '-I' + str(filter.ROOT / 'vendor/ps3recomp/include'),
            filter.ROOT / 'port/tests/test_audio_filter.c',
            filter.ROOT / 'port/spu_thread_rtz.c', '-o', work / 'replay']
        if args.sanitize:
            command[1:1] = ['-g', '-fsanitize=address,undefined']
        if args.thinlto:
            command[1:1] = ['-flto=thin']
        def run(argv, env=None):
            subprocess.run(list(map(str, argv)), cwd=filter.ROOT, check=True, env=env)
        run(command)
        run([work / 'replay'])
        if args.capture:
            # Instrument only the test copy, saving the actual loop-entry
            # state after the captured DMA. The production transform and
            # native helper are otherwise identical.
            instrument = '''        if (ms_audio_filter_enabled()) {
            static int saved;
            if (!saved++) {
                FILE* f = fopen(getenv("MS_FILTER_TEST_STATE"), "wb");
                if (!f || fwrite(ctx, sizeof *ctx, 1, f) != 1 || fclose(f)) abort();
            }
            ms_audio_filter_prefix(ctx);
        }
'''
            (work / 'candidate.c').write_text(generated.replace(injection, instrument))
            driver = (filter.ROOT / 'tests/audio_dsp_replay.c').read_text()
            driver = '#include "spu_float_scope.h"\n' + driver.replace(
                'assert(argc==5);', 'ms_rtz_enter(); assert(argc==5);')
            driver = driver.replace('void spu_stop(spu_context*c,unsigned code)', 'void spu_stop(spu_context*c)')
            (work / 'capture.c').write_text(driver)
            flags = command[:command.index(filter.ROOT / 'port/tests/test_audio_filter.c')]
            files = [work / 'capture.c', work / 'candidate.c',
                filter.ROOT / 'port/spu_thread_rtz.c',
                filter.ROOT / 'port/spu/motorstorm_audio_dsp/spu_recomp.c',
                filter.ROOT / 'port/spu/motorstorm_audio_2FEF0/spu_recomp.c',
                filter.ROOT / 'port/spu/motorstorm_audio_register.c',
                filter.ROOT / 'vendor/ps3recomp/runtime/spu/spu_interp.c']
            run([*flags, '-I' + str(filter.SOURCE.parent), '-include',
                 filter.ROOT / 'port/spu_audio_conversions.h', *files, '-lm', '-o', work / 'capture'])
            state = work / 'loop-state.bin'
            for enabled, lifted in [('0', '1'), ('1', '1'), ('0', '0')]:
                env = dict(os.environ, MOTORSTORM_AUDIO_FILTER=enabled, MOTORSTORM_AUDIO_BLOCK='0',
                           MOTORSTORM_NATIVE_AUDIO='1', MS_FILTER_TEST_STATE=str(state))
                print(f'Full DSP replay: filter={enabled}, lifted={lifted}', flush=True)
                run([work / 'capture', *[args.capture / f'32770-{part}.bin'
                    for part in ('before', 'after', 'channels')], lifted], env=env)
            run([work / 'replay', state])


if __name__ == '__main__':
    main()
