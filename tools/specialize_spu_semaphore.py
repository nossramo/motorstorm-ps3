#!/usr/bin/env python3
"""Specialize one checked delay loop; preserve the original generated source."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = "spu_e4633a5354beb2ec6e89ced78ef31ef3076c119b62ac8e7b0302a1f0b540e559"
SOURCE = ROOT / "port/spu" / IMAGE / "spu_recomp.c"
NAME = IMAGE + "_spu_func_0000AAE8"

def loop_bodies():
    source = SOURCE.read_text()
    start = source.index("loc_0000AAE8:\n", source.index("void " + NAME + "("))
    end = source.index("        ctx->gpr[2] = spu_a(ctx->gpr[80]", start)
    original = source[start:end]
    assert original.count("spu_ls_write128(") == 2
    assert original.count("spu_ls_read128(") == 3
    assert "if (ctx->gpr[2]._u32[0] == 0) goto loc_0000AAE8;" in original
    # Fail closed if the lifted block changes, before skipping any instruction.
    import hashlib
    assert hashlib.sha256(original.encode()).hexdigest() == EXPECTED_LOOP_SHA256
    prefix = """        const uint64_t delay_t0 = PS3_FT_ON() ? ps3_ft_now() : 0;
        const uint64_t delay_cpu0 = delay_t0 ? ps3_ft_cpu_ns(0) : 0;
        const int delay_mode = motorstorm_spu_semaphore_delay(ctx);
"""
    suffix = """        if (delay_t0)
            ps3_ft_span(39, ctx->spu_id, (uint32_t)(ps3_ft_cpu_ns(0) - delay_cpu0),
                        delay_mode, delay_t0, ps3_ft_now());
"""
    return original, prefix + original + suffix

EXPECTED_LOOP_SHA256 = "3d9545bb8548efb6370fdbc4664940261089de0cdaaaaf46cd02e29c3af7de4f"

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", type=Path, required=True)
    args = parser.parse_args()
    original, optimized = loop_bodies()
    source = SOURCE.read_text()
    assert source.count(original) == 1
    args.generate.write_text('#include "' + str(ROOT / "port/spu_semaphore_wait.h") +
                             '"\n' + source.replace(original, optimized))
