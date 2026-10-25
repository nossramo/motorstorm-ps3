#!/usr/bin/env python3
"""Differential-check the actual lifted loop plus production reservation wait."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from specialize_spu_semaphore import ROOT, loop_bodies

original, optimized = loop_bodies()
with tempfile.TemporaryDirectory(prefix="motorstorm-spu-semaphore-") as d:
    path = Path(d)
    (path/"semaphore_loops.inc").write_text(
        "static void reference_loop(spu_context* ctx) {\n"+original+"}\n"+
        "static int optimized_loop(spu_context* ctx) {\n"+optimized+"return delay_mode;\n}\n")
    sdk = ROOT/"vendor/ps3recomp"
    for diagnostics in ("0", "1"):
        subprocess.run(["clang", "-std=gnu17", "-O2", "-UNDEBUG", "-pthread",
            "-fno-strict-aliasing", "-fwrapv", *shlex.split(os.environ.get("CFLAGS","")),
            "-DSPU_LS_DIAGNOSTICS="+diagnostics,
            "-I"+str(path), "-I"+str(ROOT/"port"), "-I"+str(sdk/"runtime/spu"),
            "-I"+str(sdk/"include"), str(ROOT/"port/tests/test_spu_semaphore_wait.c"),
            "-o", str(path/"test")], check=True)
        for mode in ("0","1"):
            print("SPU LS diagnostics:", diagnostics, "coherence lock mode:", mode, flush=True)
            subprocess.run([str(path/"test")], env=dict(os.environ,SPU_LOCKLINE_HOST=mode),
                           check=True, timeout=40)
