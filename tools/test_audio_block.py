#!/usr/bin/env python3
"""Validate the native audio block against integer arithmetic and the full lift."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import specialize_audio_filter as audio


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--thinlto', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--capture', type=Path, action='append', default=[])
    args = parser.parse_args()
    for capture in args.capture:
        for part in ('before', 'after', 'channels'):
            if not (capture / f'32770-{part}.bin').is_file():
                parser.error(f'Incomplete DSP capture: {capture}/32770-{part}.bin is missing')
    os.environ.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    root = audio.ROOT
    source = audio.checked_source()
    with tempfile.TemporaryDirectory(prefix='motorstorm-audio-block-') as folder:
        work = Path(folder)
        ref = work / 'ref'; ref.mkdir()
        (ref / 'spu_recomp.c').write_text(source.replace('ms_audio_32770_', 'ref_audio_32770_'))
        (ref / 'spu_recomp.h').write_text((audio.SOURCE.parent / 'spu_recomp.h').read_text().replace(
            'ms_audio_32770_', 'ref_audio_32770_'))
        (work / 'candidate.c').write_text(audio.generate(source))
        (work / 'audio_block_reference.inc').write_text(
            'static __attribute__((noinline)) void reference_loop(spu_context* ctx) {\n' +
            audio.loop_body(source) + '}\n')
        flags = ['clang', '-O2', '-std=gnu17', '-UNDEBUG', '-fno-strict-aliasing',
            '-fwrapv', '-ffp-contract=off', '-DSPU_LS_NEON', '-DSPU_LS_DIAGNOSTICS=0',
            '-DMOTORSTORM_SPU_THREAD_RTZ', '-DMOTORSTORM_AUDIO_BLOCK_TEST',
            '-include', root / 'port/spu_thread_rtz.h',
            '-include', root / 'port/spu_shufb_neon.h', '-I' + str(work),
            '-I' + str(root / 'port'), '-I' + str(audio.SOURCE.parent),
            '-I' + str(root / 'vendor/ps3recomp/runtime/spu'),
            '-I' + str(root / 'vendor/ps3recomp/include')]
        if args.thinlto:
            flags += ['-flto=thin']
        if args.sanitize:
            flags += ['-g', '-fsanitize=address,undefined']
        def run(argv, **env):
            subprocess.run(list(map(str, argv)), cwd=root, check=True,
                env=os.environ | {'MOTORSTORM_AUDIO_BLOCK':'1', 'MOTORSTORM_AUDIO_FILTER':'1'} | env)
        run([*flags, root / 'port/tests/test_audio_block.c', ref / 'spu_recomp.c',
             work / 'candidate.c', root / 'port/spu_thread_rtz.c', '-o', work / 'test'])
        run([work / 'test'])
        if not args.capture:
            return
        # Save the actual recurrence boundary in a test-only generated copy.
        generated = audio.generate(source)
        injection = '        ms_audio_block_prepare(ctx, 0);\n'
        instrument = '''        {
            static int saved;
            if (!saved++) {
                FILE* f = fopen(getenv("MS_BLOCK_TEST_STATE"), "wb");
                if (!f || fwrite(ctx, sizeof *ctx, 1, f) != 1 || fclose(f)) abort();
            }
            ms_audio_block_prepare(ctx, 0);
        }
'''
        assert generated.count(injection) == 1
        (work / 'capture-candidate.c').write_text(generated.replace(injection, instrument))
        driver = (root / 'tests/audio_dsp_replay.c').read_text().replace(
            'void spu_stop(spu_context*c,unsigned code)', 'void spu_stop(spu_context*c)')
        (work / 'capture.c').write_text(driver)
        run([*flags, '-include', root / 'port/spu_audio_conversions.h',
             work / 'capture.c', work / 'capture-candidate.c', root / 'port/spu_thread_rtz.c',
             root / 'port/spu/motorstorm_audio_dsp/spu_recomp.c',
             root / 'port/spu/motorstorm_audio_2FEF0/spu_recomp.c',
             root / 'port/spu/motorstorm_audio_register.c',
             root / 'vendor/ps3recomp/runtime/spu/spu_interp.c', '-lm', '-o', work / 'capture'])
        for capture in args.capture:
            state = work / 'captured-state.bin'
            print(f'Full recorded DSP call: {capture}', flush=True)
            run([work / 'capture', *[capture / f'32770-{part}.bin'
                for part in ('before', 'after', 'channels')], '1'],
                MOTORSTORM_NATIVE_AUDIO='1', MS_BLOCK_TEST_STATE=str(state))
            run([work / 'test', state])


if __name__ == '__main__':
    main()
