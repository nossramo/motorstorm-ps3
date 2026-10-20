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
    // The race's outer FSM remains in state 5 at the results menu.
    // A held driving key must not select an item as that menu appears.
    MacKeyboardContext previous=MAC_KEYBOARD_CONTEXT_GAMEPLAY;
    uint64_t keys[2]={0,UINT64_C(1)<<(125-64)}, blocked[2]={0,0};
    put(osd+0x6a8,7);
    auto context=static_cast<MacKeyboardContext>(motorstorm_keyboard_context());
    assert(context==MAC_KEYBOARD_CONTEXT_FRONTEND);
    mac_keyboard_context_transition(context,&previous,keys,blocked);
    auto pad=mac_keyboard_pad_state(keys[0]&~blocked[0],keys[1]&~blocked[1],context);
    assert(pad.buttons==0);
    keys[1]=0; mac_keyboard_context_transition(context,&previous,keys,blocked);
    keys[0]=UINT64_C(1)<<36; keys[1]=UINT64_C(1)<<(125-64);
    mac_keyboard_context_transition(context,&previous,keys,blocked);
    pad=mac_keyboard_pad_state(keys[0]&~blocked[0],keys[1]&~blocked[1],context);
    assert((pad.buttons&0x4040)==0x4040); // Down navigates, Enter confirms.
    assert(!(pad.buttons&0x0300));       // Neither driving trigger is sent.
    put(osd+0x6a8,0); assert(motorstorm_keyboard_context()==MAC_KEYBOARD_CONTEXT_GAMEPLAY);
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
    std::puts("keyboard context: race/pause/results/retry, held keys, confirmation and bounds guards passed");
}
