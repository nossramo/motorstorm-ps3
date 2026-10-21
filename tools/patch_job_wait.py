#!/usr/bin/env python3
"""Restore the authored wait hook after lifting, without storing game code."""
import argparse
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL_SHA256 = "ec8ce3d2b9058e1be50be587dfb5b7e4678d34d2bbf63098e831cf01d1e39e13"
SIGNATURE = "void func_00CF7358(ppu_context* ctx) {"
IMPLEMENTATION = "static void profiled_job_wait_impl(ppu_context* ctx) {"
INCLUDE = '#include "ps3emu/ps3_ftrace.h"\n'
COUNTER = "        unsigned host_spins = 0;\n"
BACKEDGE = "        if (((ctx->cr >> 2) & 1)) goto loc_00CF7358;"
WAIT = """        if (((ctx->cr >> 2) & 1)) {
            if (++host_spins == 256) {
                host_spins = 0;
                motorstorm_job_wait_pause((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[8],
                                          (uint32_t)ctx->gpr[4]);
            }
            goto loc_00CF7358;
        }"""
WRAPPER = """static void profiled_job_wait_impl(ppu_context* ctx);
extern "C" void motorstorm_job_wait_pause(uint32_t, uint32_t, uint32_t);
void func_00CF7358(ppu_context* ctx) {
    uint64_t t0 = PS3_FT_ON() ? ps3_ft_now() : 0;
    const uint64_t c0 = t0 ? ps3_ft_cpu_ns(0) : 0;
    profiled_job_wait_impl(ctx);
    if (t0) ps3_ft_span(PS3_FT_JOB_WAIT, (uint32_t)ctx->gpr[8], (uint32_t)(ps3_ft_cpu_ns(0)-c0),
                        0xCF7358, t0, ps3_ft_now());
}
"""
OLD_WRAPPER = WRAPPER
WRAPPER = WRAPPER.replace('    profiled_job_wait_impl(ctx);', '''    const uint64_t passive_start = ps3_metric_begin();
    profiled_job_wait_impl(ctx);
    ps3_wait_record(PW_JOB_TOTAL, passive_start, 0);''')
PASSIVE_INCLUDE = '#include "ps3emu/passive_waits.h"\n'


def patch(source):
    if IMPLEMENTATION in source:
        start = source.index(IMPLEMENTATION)
        end = source.index("\n}", start) + 2
        block = source[start:end]
        if block.count(COUNTER) != 1 or block.count(WAIT) != 1:
            raise ValueError("Unrecognized existing wait hook")
        original = block.replace(IMPLEMENTATION, SIGNATURE, 1)
        original = original.replace(COUNTER, "", 1).replace(WAIT, BACKEDGE, 1)
        if not any(w + block in source for w in (WRAPPER, OLD_WRAPPER)) or INCLUDE not in source:
            raise ValueError("Incomplete wait wrapper")
        already_patched = True
    else:
        if source.count(SIGNATURE) != 1:
            raise ValueError("Expected one generated wait function")
        start = source.index(SIGNATURE)
        end = source.index("\n}", start) + 2
        original = source[start:end]
        already_patched = False
    if hashlib.sha256(original.encode()).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("Generated wait function changed; review before patching")
    if already_patched:
        result = source.replace(OLD_WRAPPER + block, WRAPPER + block, 1)
        return result if PASSIVE_INCLUDE in result else PASSIVE_INCLUDE + result
    block = original.replace(SIGNATURE, IMPLEMENTATION + "\n" + COUNTER.rstrip("\n"), 1)
    block = block.replace(BACKEDGE, WAIT, 1)
    result = source[:start] + WRAPPER + block + source[end:]
    if INCLUDE not in result: result = INCLUDE + result
    return result if PASSIVE_INCLUDE in result else PASSIVE_INCLUDE + result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=ROOT / "port/recompiled/ppu_recomp_090.cpp")
    parser.add_argument("--check", action="store_true", help="Verify without writing")
    args = parser.parse_args()
    before = args.source.read_text()
    after = patch(before)
    if args.check:
        if before != after:
            raise SystemExit("Wait hook needs to be applied")
        print("Wait hook and original function fingerprint verified.")
    elif before != after:
        args.source.write_text(after)
        print("Restored authored job-wait hook.")
    else:
        print("Wait hook already present and verified.")


if __name__ == "__main__":
    main()
