/* Guest-visible ABI checks, independent of the host struct definitions.
 * Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellAudioOut.h. */
#include "../../vendor/ps3recomp/libs/misc/cellAvconfExt.c"
#include <assert.h>

uint8_t* vm_base;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t a) { (void)a; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
void ps3_hle_register_ctx(uint32_t n, const char* s, void (*f)(ppu_context*))
{ (void)n; (void)s; (void)f; }

int main(void)
{
    static uint8_t memory[1024];
    vm_base = memory;
    memset(memory, 0xcd, sizeof(memory));
    assert(cellAudioOutGetDeviceInfo(0, 0, (void*)128) == 0);
    assert(memory[128] == 0 && memory[129] == 3 && memory[130] == 1);
    const uint8_t channels[] = {2, 6, 8};
    const uint32_t layouts[] = {1, 0x10000, 0x40000000};
    for (unsigned i = 0; i < 3; ++i) {
        unsigned ea = 128 + 8 + i * 8;
        assert(memory[ea] == 0 && memory[ea+1] == channels[i]);
        assert(memory[ea+2] == 4 && vm_read32(ea+4) == layouts[i]);
        assert(cellAudioOutGetSoundAvailability2(0, 0, 4, channels[i], 0) == channels[i]);
    }
    assert(memory[127] == 0xcd && memory[128+136] == 0xcd);
    assert(cellAudioOutGetSoundAvailability(0, 0, 4, 0) == 8);
    assert(cellAudioOutGetSoundAvailability(0, 0, 1, 0) == 0);
    assert(cellAudioOutGetSoundAvailability(0, 1, 4, 0) == 0);
    assert(cellAudioOutGetState(0, 0, (void*)512) == 0);
    assert(memory[512+13] == 2 && memory[512+14] == 4);
    assert(vm_read32(512+16) == 1 && memory[512+20] == 0xcd);
    memset(memory+768, 0, 16);
    memory[768] = 8;
    vm_write32(768+12, 1);
    assert(cellAudioOutConfigure(0, (void*)768, NULL, 1) == 0);
    assert(cellAudioOutGetState(0, 0, (void*)512) == 0);
    assert(memory[512+13] == 8 && vm_read32(512+8) == 1);
    assert(vm_read32(512+16) == 0x40000000);
    assert(cellAudioOutGetConfiguration(0, (void*)800, NULL, 0) == 0);
    assert(memory[800] == 8 && vm_read32(800+12) == 1);
    memory[768] = 3;
    assert(cellAudioOutConfigure(0, (void*)768, NULL, 0) != 0);
    assert(cellAudioOutGetState(0, 0, (void*)512) == 0 && memory[525] == 8);
    puts("Audio output: guest layout, modes, 48 kHz and configure/readback passed.");
}
