#!/usr/bin/env python3
"""Exercise the production texture mutation detector and time its two paths."""
from pathlib import Path
import subprocess, tempfile

root = Path(__file__).resolve().parents[1]
src = (root/'vendor/ps3recomp/libs/video/rsx_draw_engine.c').read_text()
start = src.index('static int s_texhash_xxh3;')
end = src.index('/* The cheap per-frame change check:', start)
code = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>
#define XXH_INLINE_ALL
#include "vendor/ps3recomp/third_party/xxhash/xxhash.h"
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
static u8 *bytes; static u32 length;
static const u8* eng_guest_ptr(void* user,u32 loc,u32 offset,u32 span) {
    (void)user;return !loc && offset<=length && span<=length-offset?bytes+offset:NULL;
}
''' + src[start:end] + r'''
static double now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9; }
int main(void) {
    length=16*1024*1024+64;bytes=malloc(length);assert(bytes);
    for(u32 i=0;i<length;i++) bytes[i]=(u8)(i*117u+(i>>7));
    for(int mode=0;mode<2;mode++) {
        s_texhash_xxh3=mode;
        for(u32 span=1;span<=8192;span=span*2+1) for(u32 offset=0;offset<16;offset++) {
            int valid=0;u64 original=eng_texture_content_hash(0,offset,span,&valid);assert(valid);
            for(u32 j=0;j<span;j++) {
                bytes[offset+j]^=128;
                assert(eng_texture_content_hash(0,offset,span,&valid)!=original && valid);
                bytes[offset+j]^=128;
            }
            assert(eng_texture_content_hash(0,offset,span,&valid)==original && valid);
        }
        int valid=1;assert(!eng_texture_content_hash(0,length-3,4,&valid) && !valid);
        valid=1;assert(!eng_texture_content_hash(1,0,4,&valid) && !valid);
        valid=1;assert(!eng_texture_content_hash(0,0,0,&valid) && !valid);
        volatile u64 result=0;double begin=now();
        for(int i=0;i<128;i++) result^=eng_texture_content_hash(0,i&15,16*1024*1024,&valid);
        double elapsed=now()-begin;
        printf("%s: %.2f GB/s (%llu)\n",mode?"XXH3":"FNV",128.*16*1024*1024/elapsed/1e9,(unsigned long long)result);
    }
    free(bytes);puts("texture hash: mutation, unaligned spans, bounds and reuse PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='texture-hash-') as tmp:
    source=Path(tmp)/'test.c';binary=Path(tmp)/'test';source.write_text(code)
    subprocess.run(['clang','-std=gnu17','-O3','-fno-strict-aliasing','-I',str(root),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
