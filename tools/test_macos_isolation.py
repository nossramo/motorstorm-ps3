#!/usr/bin/env python3
"""Check the macOS snapshot guard and its real compiler dependency boundary."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

import build_macos_runtime as mac


class SnapshotGuard(unittest.TestCase):
    def test_mutated_added_and_linked_source_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'snapshot'
            port = source / 'port'
            port.mkdir(parents=True)
            file = port / 'main.cpp'
            file.write_text('original macOS source')
            manifest = {'commit': 'test', 'files': mac.source_hashes(source)}
            (source / 'macos-source.json').write_text(json.dumps(manifest))
            mac.verify(source, 'test')
            file.write_text('iOS experiment')
            with self.assertRaises(RuntimeError):
                mac.verify(source, 'test')
            file.write_text('original macOS source')
            extra = port / 'extra.cpp'
            extra.write_text('an extra globbed translation unit')
            with self.assertRaises(RuntimeError):
                mac.verify(source, 'test')
            extra.unlink()
            extra.symlink_to(file)
            with self.assertRaises(RuntimeError):
                mac.verify(source, 'test')

    def test_working_tree_edits_cannot_enter_archive(self):
        with tempfile.TemporaryDirectory() as folder:
            repo = Path(folder) / 'development'
            repo.mkdir()
            def git(*args):
                return subprocess.check_output(['git', '-C', str(repo), *args], text=True)
            git('init', '--quiet')
            (repo / 'engine.c').write_text('known macOS version')
            git('add', 'engine.c')
            tree = git('write-tree').strip()
            (repo / 'engine.c').write_text('uncommitted iOS experiment')
            (repo / 'ios_only.c').write_text('must not be archived')
            destination = Path(folder) / 'macos'
            mac.archive(repo, tree, destination)
            self.assertEqual((destination / 'engine.c').read_text(), 'known macOS version')
            self.assertFalse((destination / 'ios_only.c').exists())
            self.assertEqual((repo / 'engine.c').read_text(), 'uncommitted iOS experiment')


def compiled_boundary():
    mac.verify(mac.SOURCE, mac.baseline_commit())
    build = mac.SOURCE / 'build/game'
    records = json.loads(subprocess.check_output(['ninja', '-t', 'compdb'], cwd=build))
    count = 0
    runtime_c = 0
    for record in records:
        tokens = shlex.split(record['command'])
        if '-c' not in tokens:
            continue
        count += 1
        paths = [tokens[tokens.index('-c') + 1]]
        is_runtime_c = ('ps3recomp_sdk/CMakeFiles/ps3recomp_runtime.dir/' in record['output']
                        and Path(paths[0]).suffix == '.c')
        if is_runtime_c:
            runtime_c += 1
            assert '-fno-common' in tokens, f'Missing macOS alignment fix: {paths[0]}'
        else:
            assert '-fno-common' not in tokens, f'Alignment fix escaped the runtime C target: {paths[0]}'
        for i, token in enumerate(tokens):
            if token in ('-I', '-include', '-isystem'):
                paths.append(tokens[i + 1])
            elif token.startswith('-I'):
                paths.append(token[2:])
        for value in paths:
            path = (Path(record['directory']) / value).resolve()
            if path.is_relative_to(mac.ROOT) and not path.is_relative_to(mac.SOURCE):
                raise AssertionError(f'macOS compile consumes live development source: {path}')
    if count < 200:
        raise AssertionError(f'Incomplete compilation database ({count} entries)')
    assert runtime_c > 0, 'No runtime C compilation entries checked'
    response = (mac.CACHE / 'xcode-link.rsp').read_text()
    assert str(build / 'libMotorStormRecomp.a') in response
    assert str(build / 'ps3recomp_sdk/libps3recomp_runtime.a') in response
    assert str(mac.ROOT / 'build/ios-game') not in response
    assert str(mac.ROOT / 'build/game/') not in response
    print(f'PASS: {count} compile commands and Xcode link dependencies use isolated macOS source.')
    print(f'PASS: -fno-common is restricted to the {runtime_c} runtime C compile commands.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiled', action='store_true')
    args = parser.parse_args()
    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(SnapshotGuard))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.compiled:
        compiled_boundary()
