#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
xcrun --sdk macosx clang -std=gnu17 -O2 -fsanitize=address,undefined \
    -c port/tests/offline_cpu_worker.c -o "$test_dir/cpu-worker.o"
xcrun --sdk macosx clang++ -std=c++20 -O2 -fobjc-arc -framework Foundation -DMOTORSTORM_IOS_DIAGNOSTICS=1 \
    -fsanitize=address,undefined \
    'MotorStorm/MotorStorm iOS/OfflineDiagnostics.mm' \
    port/tests/test_offline_diagnostics.mm "$test_dir/cpu-worker.o" -o "$test_dir/offline-test"
"$test_dir/offline-test" "$test_dir/documents"
python3 tools/collect_ios_diagnostics.py --archive "$test_dir/diagnostics.zip" --out "$test_dir/imported"
python3 - "$test_dir" <<'PY'
import json,sys,zipfile
from pathlib import Path
sys.path.insert(0,'tools')
from collect_ios_diagnostics import import_archive
root=Path(sys.argv[1])
with zipfile.ZipFile(root/'diagnostics.zip') as archive:
    names=archive.namelist()
    assert any(name.endswith('/session.json') for name in names)
    assert not any('secret.dat' in name or '/game/' in name or 'session-user-folder' in name for name in names)
report=json.loads((root/'imported/summary.json').read_text())
assert len(report)==1 and report[0]['executable_uuid']
with zipfile.ZipFile(root/'unsafe.zip','w') as archive:
    archive.writestr('../session-escape/session.json','{}')
try:
    import_archive(root/'unsafe.zip',root/'unsafe-output')
    raise AssertionError('Unsafe path accepted')
except ValueError:
    pass
assert not (root/'session-escape').exists()
PY
printf '%s\n' 'Offline diagnostics: counters, threads, timer, JSON, rotation, retention, console cap and ZIP export/import passed.'
