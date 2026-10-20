/* Actual GetData output with an authored clock and no physical controller.
 * Empty drain calls must not consume a requested hold. Inspired by upstream
 * MotorStorm issue #138; this fixture exercises our production implementation. */
#include <time.h>
#include <SDL2/SDL.h>
#include "../../vendor/ps3recomp/runtime/platform/win32_compat.h"
static unsigned long long test_ms = 1000;
static int test_clock_gettime(clockid_t clock, struct timespec* ts) {
    (void)clock; ts->tv_sec = test_ms / 1000; ts->tv_nsec = (test_ms % 1000) * 1000000;
    return 0;
}
#define clock_gettime test_clock_gettime
#include "../../vendor/ps3recomp/libs/input/cellPad.c"
#undef clock_gettime
#include <unistd.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
u32 cellGcm_flip_request_count(void) { return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void SDL_GameControllerUpdate(void) {}
/* No real controller or window service is needed for packet/probe semantics.
 * Avoid loading SDL's dylib constructor (which can open a modal error dialog
 * under sanitizer interposition before main runs). */
SDL_GameController* SDL_GameControllerOpen(int i) { (void)i; return NULL; }
SDL_bool SDL_GameControllerGetAttached(SDL_GameController* c) { (void)c; return SDL_FALSE; }
void SDL_GameControllerClose(SDL_GameController* c) { (void)c; }
Uint8 SDL_GameControllerGetButton(SDL_GameController* c, SDL_GameControllerButton b)
{ (void)c; (void)b; return 0; }
Sint16 SDL_GameControllerGetAxis(SDL_GameController* c, SDL_GameControllerAxis a)
{ (void)c; (void)a; return 0; }
int SDL_GameControllerRumble(SDL_GameController* c, Uint16 lo, Uint16 hi, Uint32 ms)
{ (void)c; (void)lo; (void)hi; (void)ms; return 0; }
int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
Uint32 SDL_WasInit(Uint32 flags) { return flags; }
int SDL_NumJoysticks(void) { return 0; }
static unsigned keyboard_buttons;
unsigned rsx_metal_keyboard_buttons(void) { return keyboard_buttons; }
SDL_bool SDL_IsGameController(int i) { (void)i; return SDL_FALSE; }
static int failures;
static void check(int ok, const char* what) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what); failures += !ok;
}
static void get(u32 port) { check(cellPadGetData(port, (CellPadData*)(uintptr_t)0x100) == CELL_OK, "GetData succeeds"); }
static u16 button(u32 n) { return vm_read16(0x104 + n * 2); }
int main(int argc, char** argv) {
    char path[] = "/tmp/motorstorm-pad-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 2;
    FILE* command = fdopen(fd, "w"); fputs("0x0200 4\n", command); fclose(command);
    setenv("PAD_FILE", path, 1); setenv("PAD_NO_KEYBOARD", "1", 1);
    unsetenv("PAD_SCRIPT"); unsetenv("PAD_ALWAYS_CHANGE"); unsetenv("PAD_AUTOPRESS");
    unsetenv("PAD_SCRIPT_STOP_FILE");
    ppu_vm_size = 0x10000; vm_base = calloc(1, ppu_vm_size);
    s_pad_initialized = 1; s_max_connect = 2;
    s_port_setting[0] = CELL_PAD_SETTING_PRESS_ON;
    if (argc == 2 && !strcmp(argv[1], "--script-stop")) {
        unsetenv("PAD_NO_KEYBOARD");
        char stop_path[512], ack_path[520];
        snprintf(stop_path, sizeof stop_path, "%s.stop", path);
        snprintf(ack_path, sizeof ack_path, "%s.ack", stop_path);
        setenv("PAD_SCRIPT", "0:0x4000,1:0x0008,2:0x2000", 1);
        setenv("PAD_SCRIPT_STOP_FILE", stop_path, 1);
        test_ms += 4; get(0);
        check((button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 0x42) == 0x42,
              "script Cross and independent file throttle both arrive");
        command = fopen(stop_path, "w"); fputs("1\n", command); fclose(command);
        test_ms += 4; get(0);
        check(!(button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 0x40),
              "cancellation releases the currently held scripted Cross");
        check((button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 2) &&
              button(CELL_PAD_BTN_OFFSET_PRESS_R2) == 255,
              "cancellation preserves independent throttle and pressure");
        command = fopen(ack_path, "r");
        check(command && fgetc(command) == '1', "cancellation is acknowledged");
        if (command) fclose(command);
        unlink(stop_path); /* Removing the request must not resume the script. */
        test_ms += 3000; get(0);
        check(!(button(CELL_PAD_BTN_OFFSET_DIGITAL1) & 8) &&
              !(button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 0x60),
              "future Start/Circle and old Cross stay cancelled");
        keyboard_buttons = CELL_PAD_CTRL_CIRCLE;
        test_ms += 4; get(0);
        check(button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 0x20,
              "real keyboard input still works after script cancellation");
        keyboard_buttons = 0;
        test_ms += 4; get(0);
        check(!(button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 0x62),
              "keyboard and file controls release normally");
        unlink(ack_path); unlink(path); free(vm_base);
        return failures ? 1 : 0;
    }
    int held_reports = 0, pressure_reports = 0, empty = 0;
    for (int report = 0; report < 4; ++report) {
        test_ms += 4;
        get(1);
        check(!(button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 2), "R2 injection stays on port zero");
        get(0);
        check(vm_read32(0x100) > 0, "fresh report is delivered");
        held_reports += !!(button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 2);
        pressure_reports += button(CELL_PAD_BTN_OFFSET_PRESS_R2) == 255;
        for (int drain = 0; drain < 20; ++drain) {
            cellPadGetData(0, (CellPadData*)(uintptr_t)0x100);
            empty += vm_read32(0x100) == 0;
        }
    }
    check(held_reports == 4, "four requested reports remain pressed across eighty empty drain calls");
    check(pressure_reports == 4, "every held R2 report carries full trigger pressure");
    check(empty == 80, "all same-clock drain calls return len zero");
    test_ms += 4; get(0);
    check(vm_read32(0x100) > 0 && !(button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 2) &&
          button(CELL_PAD_BTN_OFFSET_PRESS_R2) == 0, "next report releases R2 and pressure");
    command = fopen(path, "r");
    check(command && fgetc(command) == EOF, "input file was acknowledged once");
    if (command) fclose(command);
    command = fopen(path, "w"); fputs("0x0200 2 104 120 144 140\n", command); fclose(command);
    for (int report = 0; report < 2; ++report) {
        test_ms += 4; get(1);
        check(button(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X) != 104, "injected stick stays on port zero");
        get(0);
        check(button(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X) == 104 &&
              button(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y) == 120 &&
              button(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X) == 144 &&
              button(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y) == 140 &&
              button(CELL_PAD_BTN_OFFSET_PRESS_R2) == 255, "steering and throttle arrive together");
    }
    command = fopen(path, "w"); fputs("0 1 128 128 128 128\n", command); fclose(command);
    test_ms += 4; get(0);
    check(button(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X) == 128 &&
          button(CELL_PAD_BTN_OFFSET_PRESS_R2) == 0, "neutral report releases steering and throttle");
    unlink(path); free(vm_base);
    return failures ? 1 : 0;
}
