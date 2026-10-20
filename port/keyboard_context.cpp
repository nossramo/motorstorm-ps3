// Read-only keyboard context for the pinned BCES00006 03.10 executable.
// The front-end FSM is constructed at 0x006056C8 and dispatched at 0x00603468.
// State 5 runs the race via 0x005FEFAC; all other normal states are menus.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../vendor/ps3recomp/libs/input/macos_keyboard_map.h"

extern "C" uint8_t* vm_base;
extern "C" uint32_t ppu_vm_size;

namespace {
bool readable(uint64_t address, uint64_t size) {
    const uint64_t limit = ppu_vm_size ? ppu_vm_size : (UINT64_C(1) << 32);
    return vm_base && address >= 0x10000 && address <= limit && size <= limit-address;
}
uint32_t read_word(uint64_t address) {
    if ((address & 3) || !readable(address, 4)) return 0;
    uint32_t value;
    std::memcpy(&value, vm_base + address, sizeof value);
    return __builtin_bswap32(value);
}
}

extern "C" int motorstorm_keyboard_context() {
    const uint32_t server = read_word(0x1013b5e4);
    const uint32_t wrapper = server ? read_word(uint64_t(server) + 0x38) : 0;
    const uint32_t machine = read_word(wrapper);
    if (!readable(machine, 0x3e70) || (machine & 3) ||
        read_word(uint64_t(machine) + 0x40) != machine ||
        read_word(uint64_t(machine) + 0x44) != 0x100e7d00 ||
        read_word(uint64_t(machine) + 0x110) != 0x100e8118)
        return MAC_KEYBOARD_CONTEXT_UNKNOWN;
    const uint32_t state = read_word(machine);
    if (state > 71) return MAC_KEYBOARD_CONTEXT_UNKNOWN;
    int context = state == 5 ? MAC_KEYBOARD_CONTEXT_GAMEPLAY :
                  state == 1 ? MAC_KEYBOARD_CONTEXT_BOOT : MAC_KEYBOARD_CONTEXT_FRONTEND;
    if (state == 5) {
        // OsdSystemComponent singleton (TOC + 0x2E38). Its own FSM is
        // dispatched by 0x0055FAFC: 2 is the pause menu, 4 its confirmation
        // dialog. Captured race/pause/resume/pause states were 0/2/0/2.
        const uint32_t osd = read_word(0x1013b584);
        if (osd) {
            if (!readable(osd, 0x6ac) || read_word(osd) != 0x100e7640)
                return MAC_KEYBOARD_CONTEXT_UNKNOWN;
            const uint32_t osd_state = read_word(uint64_t(osd) + 0x6a8);
            if (osd_state == 2) context = MAC_KEYBOARD_CONTEXT_PAUSE;
            else if (osd_state == 4) context = MAC_KEYBOARD_CONTEXT_FRONTEND;
            if (read_word(uint64_t(osd) + 0x6a8) != osd_state ||
                read_word(0x1013b584) != osd)
                return MAC_KEYBOARD_CONTEXT_UNKNOWN;
        }
    }
    // Recheck in case the guest moved to a different state while we sampled it.
    if (read_word(machine) != state) return MAC_KEYBOARD_CONTEXT_UNKNOWN;
    static const bool trace = std::getenv("MOTORSTORM_KEYBOARD_TRACE") != nullptr;
    static thread_local int previous = -1;
    if (trace && context != previous) {
        std::fprintf(stderr, "[keyboard-context] state=%u context=%d\n", state, context);
        previous = context;
    }
    return context;
}
