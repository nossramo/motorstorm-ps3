#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include "../../vendor/ps3recomp/libs/input/macos_keyboard_map.h"
extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size;
int motorstorm_keyboard_context();
}
static void put(uint32_t a, uint32_t v) {
    v=__builtin_bswap32(v); std::memcpy(vm_base+a,&v,4);
}
int main() {
    assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_UNKNOWN);
    vm_base=static_cast<uint8_t*>(mmap(nullptr, UINT64_C(1)<<32,
        PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0));
    assert(vm_base!=MAP_FAILED);
    constexpr uint32_t server=0x0feff9bc, wrapper=0x46f31d00, machine=0x46f45460;
    put(0x1013b5e4,server); put(server+0x38,wrapper); put(wrapper,machine);
    put(machine+0x40,machine); put(machine+0x44,0x100e7d00); put(machine+0x110,0x100e8118);
    put(machine,8); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_FRONTEND);
    put(machine,5); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_GAMEPLAY);
    constexpr uint32_t osd=0x46ef6400;
    put(0x1013b584,osd); put(osd,0x100e7640);
    put(osd+0x6a8,0); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_GAMEPLAY);
    put(osd+0x6a8,2); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_PAUSE);
    put(osd+0x6a8,0); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_GAMEPLAY);
    put(osd+0x6a8,2); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_PAUSE);
    put(osd+0x6a8,4); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_FRONTEND);
    put(machine,8); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_FRONTEND);
    put(machine,5); put(osd,0); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_UNKNOWN);
    put(osd,0x100e7640); put(0x1013b584,0xfffffff0);
    assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_UNKNOWN);
    put(0x1013b584,0);
    put(machine,1); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_BOOT);
    put(machine,72); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_UNKNOWN);
    put(machine,5); put(machine+0x44,0); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_UNKNOWN);
    put(machine+0x44,0x100e7d00); put(machine+0x40,0); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_UNKNOWN);
    put(machine+0x40,machine); put(wrapper,0xfffffff0); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_UNKNOWN);
    put(wrapper,machine); put(0x1013b5e4,0xfffffff0); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_UNKNOWN);
    put(0x1013b5e4,server); ppu_vm_size=0x10000; assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_UNKNOWN);
    munmap(vm_base,UINT64_C(1)<<32);
    std::puts("keyboard context: race/pause/resume/confirmation, high guest addresses and bounds guards passed");
}
