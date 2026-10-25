#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for probes in 0 1; do
    clang -O2 -std=gnu17 -DSPU_LS_NEON -DSPU_LS_DIAGNOSTICS=$probes -ffp-contract=off \
        -Ivendor/ps3recomp/runtime/spu -Ivendor/ps3recomp/include \
        ${TEST_SANITIZE:+-fsanitize=$TEST_SANITIZE} \
        port/tests/test_spu_ls_neon.c -o "$work/test"
    "$work/test"
done

# Inspect the actual ARM64 load/store helper bodies, excluding their diagnostic
# data declarations. This detects gates accidentally left in the hot path.
cat > "$work/access.c" <<'EOF'
#include "spu_context.h"
u128 checked_load(spu_context *ctx, uint32_t address) {
    return spu_ls_read128(ctx, address);
}
void checked_store(spu_context *ctx, uint32_t address, u128 value) {
    spu_ls_write128(ctx, address, value);
}
EOF
for probes in 0 1; do
    clang -O2 -std=gnu17 -DSPU_LS_NEON -DSPU_LS_DIAGNOSTICS=$probes \
        -Ivendor/ps3recomp/runtime/spu -Ivendor/ps3recomp/include \
        -S "$work/access.c" -o "$work/access-$probes.s"
done
python3 - "$work" <<'PY'
from pathlib import Path
import re
import sys
work = Path(sys.argv[1])
for mode in (0, 1):
    text = (work / f'access-{mode}.s').read_text()
    for name in ('checked_load', 'checked_store'):
        body = text.split(f'_{name}:', 1)[1].split('.cfi_endproc', 1)[0]
        probes = re.findall(r'(?:g_spu_ls_|g_spu_smc_|spu_ls_\w*probe|spu_ls_watch)\w*', body)
        assert bool(probes) == bool(mode), (mode, name, probes)
        assert 'rev32' in body, (mode, name, 'missing byte-order operation')
print('PASS ARM64 assembly: legacy probe references absent only in normal mode')
PY
