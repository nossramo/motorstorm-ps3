#!/usr/bin/env python3
"""Check real generated WWS registration with early and late launch settings.

Native job bodies are replaced by counters; the generated image IDs, signatures,
constructor and idempotency gate run unchanged. No game data enters this test.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
registry = root / 'port/spu/jobmods_register.c'
source = registry.read_text()
names = re.findall(r'extern void (jobmod_\w+_spu_recomp_register)\(void\);', source)
assert len(names) == 10 and len(set(names)) == 10
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned bodies[10], signatures[10];
static unsigned total_bodies, total_signatures;
static int image;
void port_jobmods_register_all(void);
void spu_begin_image(int id) { image = id; }
void spu_overlay_register_sig(const uint8_t sig[16], int id) {
    assert(id == image && id >= 200 && id < 210);
    assert(sig[0] == 0x42 || sig[0] == 0x43);
    ++signatures[id-200]; ++total_signatures;
}
static void body(unsigned i) {
    assert(image == (int)i+200);
    ++bodies[i]; ++total_bodies;
}
'''
harness += '\n'.join(f'void {name}(void) {{ body({i}); }}' for i, name in enumerate(names))
harness += r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "early")) {
        assert(total_bodies == 10 && total_signatures == 10);
    } else {
        assert(total_bodies == 0 && total_signatures == 0);
        if (!strcmp(argv[1], "disabled")) {
            port_jobmods_register_all(); port_jobmods_register_all();
            assert(total_bodies == 0 && total_signatures == 0);
            puts("disabled: no modules registered"); return 0;
        }
        /* Reproduce UIKit: constructors ran before bundle settings were read. */
        setenv("MOTORSTORM_NATIVE_JOBS", "1", 1);
    }
    port_jobmods_register_all(); port_jobmods_register_all();
    assert(total_bodies == 10 && total_signatures == 10 && image == 0);
    for (unsigned i=0; i<10; ++i) assert(bodies[i] == 1 && signatures[i] == 1);
    printf("%s: all 10 modules registered exactly once\n", argv[1]);
}
'''
with tempfile.TemporaryDirectory(prefix='motorstorm-native-startup-') as temp:
    path = Path(temp)
    (path / 'harness.c').write_text(harness)
    binary = path / 'test'
    subprocess.run(['clang', '-std=gnu17', '-O2', '-UNDEBUG',
                    '-fsanitize=address,undefined', str(registry),
                    str(path / 'harness.c'), '-o', str(binary)], check=True)
    env = dict(os.environ)
    env.pop('MOTORSTORM_NATIVE_JOBS', None)
    for mode in ('disabled', 'late', 'early'):
        subprocess.run([str(binary), mode],
                       env=env if mode != 'early' else dict(env, MOTORSTORM_NATIVE_JOBS='1'),
                       check=True, timeout=10)
