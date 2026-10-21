#!/usr/bin/env python3
"""Build pinned, static iOS dependencies without using macOS Homebrew binaries."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', choices=['iphoneos', 'iphonesimulator'], default='iphoneos')
    parser.add_argument('--jobs', default='4')
    args = parser.parse_args()
    os.environ.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    source = ROOT / 'vendor/ios-deps'
    source.mkdir(parents=True, exist_ok=True)
    dependencies = json.loads((ROOT / 'port/ios/dependencies.json').read_text())
    for name, spec in dependencies.items():
        dest = source / name
        stamp = dest / '.motorstorm-revision'
        if stamp.exists() and stamp.read_text().strip() == spec['commit']:
            continue
        if dest.exists() and any(dest.iterdir()):
            raise SystemExit(f'Refusing to overwrite unrecognized dependency: {dest}')
        url = f"https://codeload.github.com/{spec['repo']}/tar.gz/{spec['commit']}"
        print(f'Downloading {name} at {spec["commit"]}', flush=True)
        with tempfile.TemporaryDirectory(dir=source) as temp:
            archive = Path(temp) / 'source.tar.gz'
            urllib.request.urlretrieve(url, archive)
            with tarfile.open(archive) as tar:
                tar.extractall(Path(temp) / 'unpacked', filter='data')
            unpacked, = (Path(temp) / 'unpacked').iterdir()
            dest.parent.mkdir(parents=True, exist_ok=True)
            if dest.exists(): dest.rmdir()  # upstream empty dependency placeholder
            shutil.move(unpacked, dest)
            stamp.write_text(spec['commit'] + '\n')
    build = ROOT / 'build/ios-deps' / args.sdk
    prefix = build / 'install'
    sdk = subprocess.check_output(['xcrun', '--sdk', args.sdk, '--show-sdk-path'], text=True).strip()
    run(['cmake', '-S', ROOT / 'port/ios/deps', '-B', build / 'cmake', '-G', 'Ninja',
         '-DCMAKE_SYSTEM_NAME=iOS', f'-DCMAKE_OSX_SYSROOT={sdk}', '-DCMAKE_OSX_ARCHITECTURES=arm64',
         '-DCMAKE_OSX_DEPLOYMENT_TARGET=18.0', '-DCMAKE_BUILD_TYPE=Release',
         f'-DCMAKE_INSTALL_PREFIX={prefix}', f'-DIOS_SOURCE_DIR={source}'])
    run(['cmake', '--build', build / 'cmake', '--target', 'install', '-j', args.jobs])
    ffbuild = build / 'ffmpeg'
    ffbuild.mkdir(parents=True, exist_ok=True)
    target = 'arm64-apple-ios18.0' + ('-simulator' if args.sdk == 'iphonesimulator' else '')
    flags = f'-target {target} -isysroot {sdk}'
    if not (ffbuild / 'config.mak').exists() and not (ffbuild / 'ffbuild/config.mak').exists():
        run([source / 'ffmpeg/configure', f'--prefix={prefix}', '--target-os=darwin',
             '--arch=aarch64', '--enable-cross-compile', f'--sysroot={sdk}', '--cc=clang',
             f'--extra-cflags={flags}', f'--extra-ldflags={flags}', '--disable-everything',
             '--disable-programs', '--disable-doc', '--disable-debug', '--disable-autodetect',
             '--disable-shared', '--enable-static', '--enable-pic', '--enable-avcodec', '--enable-avutil',
             '--enable-decoder=h264,mpeg2video,mp3,mp3float,aac,atrac3,atrac3p,pcm_s16be,pcm_s16le',
             '--enable-parser=h264,mpegvideo,mpegaudio,aac', '--enable-pthreads'], cwd=ffbuild)
    run(['make', '-j', args.jobs], cwd=ffbuild)
    run(['make', 'install'], cwd=ffbuild)
    (build / 'ready.json').write_text(json.dumps({'sdk': args.sdk, 'dependencies': dependencies}, indent=2))
    print(f'iOS dependencies ready: {prefix}')


if __name__ == '__main__':
    main()
