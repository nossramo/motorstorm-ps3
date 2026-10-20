#!/usr/bin/env python3
"""Check actual lifted barrier plus production host wait with concurrent writers."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
source = (root/'port/recompiled/ppu_recomp_090.cpp').read_text()
body = source[source.index('static void profiled_job_wait_impl(ppu_context* ctx);'):source.index('void func_00CF73AC')]
reference = body[body.index('static void profiled_job_wait_impl(ppu_context* ctx) {'):]
reference = reference.replace('profiled_job_wait_impl', 'original_job_wait')
reference = reference.replace('        unsigned host_spins = 0;\n', '')
start = reference.index('            if (++host_spins == 256)')
end = reference.index('            goto loc_00CF7358;', start)
reference = reference[:start] + reference[end:]
with tempfile.TemporaryDirectory(prefix='motorstorm-job-wait-') as d:
    path = Path(d)
    helpers = source[source.index('static inline uint64_t ppc_rlw64'):source.index('void func_00CD67B0')]
    (path/'job_wait_lift.inc').write_text(helpers + body + reference)
    command = ['c++', '-std=c++20', '-O2', '-fwrapv', '-fno-strict-aliasing', '-pthread',
        '-I'+str(path), '-I'+str(root/'port/recompiled'),
        '-I'+str(root/'vendor/ps3recomp/include'),
        str(root/'port/tests/test_job_wait.cpp'), '-o', str(path/'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(path/'test')], check=True, timeout=10)
