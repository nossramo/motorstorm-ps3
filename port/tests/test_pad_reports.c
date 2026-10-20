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
#include "../../vendor/ps3recomp/libs/input/macos_keyboard_map.h"
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
static uint64_t keyboard_keys[2];
uint64_t rsx_metal_keyboard_keys(unsigned word)
{ return word < 2 ? keyboard_keys[word] : 0; }
static MacKeyboardContext keyboard_context = MAC_KEYBOARD_CONTEXT_UNKNOWN;
int motorstorm_keyboard_context(void) { return keyboard_context; }
SDL_bool SDL_IsGameController(int i) { (void)i; return SDL_FALSE; }
static int failures;
static void check(int ok, const char* what) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what); failures += !ok;
}
static void get(u32 port) { check(cellPadGetData(port, (CellPadData*)(uintptr_t)0x100) == CELL_OK, "GetData succeeds"); }
static u16 button(u32 n) { return vm_read16(0x104 + n * 2); }
static uint64_t key(unsigned code) { return UINT64_C(1) << (code % 64); }
int main(int argc, char** argv) {
    MacKeyboardPadState mapped = mac_keyboard_pad_state(key(13) | key(40) | key(37) |
        key(49) | key(56) | key(8) | key(48) |
        key(15) | key(3), key(126) | key(125) | key(124),
        MAC_KEYBOARD_CONTEXT_GAMEPLAY);
    check((mapped.buttons & 0x0320) == 0x0320 && mapped.left_x == 255 &&
          mapped.left_y == 0 && mapped.right_x == 255 && mapped.right_y == 255,
          "arrows steer and trigger without replacing independent stick lean");
    check((mapped.buttons & 0xF005) == 0xF005,
          "Space, left Shift, C, Tab, R, and F map to their PS3 controls");
    mapped = mac_keyboard_pad_state(0, key(126), MAC_KEYBOARD_CONTEXT_GAMEPLAY);
    check((mapped.buttons & 0x0200) && mapped.left_y == 128 &&
          !(mapped.buttons & 0x0050), "up throttle does not also lean or navigate");
    mapped = mac_keyboard_pad_state(key(13), 0, MAC_KEYBOARD_CONTEXT_GAMEPLAY);
    check(mapped.left_y == 0 && !(mapped.buttons & 0x0210),
          "W controls lean without throttle or menu navigation");
    mapped = mac_keyboard_pad_state(0, 0, MAC_KEYBOARD_CONTEXT_GAMEPLAY);
    check(!mapped.buttons && mapped.left_x == 128 && mapped.left_y == 128 &&
          mapped.right_x == 128 && mapped.right_y == 128,
          "released keys return buttons and both sticks to neutral");
    mapped = mac_keyboard_pad_state(key(13) | key(1), key(123) | key(124),
                                    MAC_KEYBOARD_CONTEXT_GAMEPLAY);
    check((mapped.buttons & 0x00A0) == 0x00A0 &&
          mapped.left_x == 128 && mapped.left_y == 128,
          "simultaneous opposing keys cancel on their analog axes");
    uint64_t event_keys[2] = {0, 0};
    mac_keyboard_apply_event(event_keys, 13, MAC_KEY_EVENT_DOWN);
    mac_keyboard_apply_event(event_keys, 13, MAC_KEY_EVENT_DOWN); /* key repeat */
    check(mac_keyboard_held(event_keys[0], event_keys[1], 13),
          "repeated key-down remains one held key");
    mac_keyboard_apply_event(event_keys, 13, MAC_KEY_EVENT_UP);
    check(!mac_keyboard_held(event_keys[0], event_keys[1], 13),
          "key-up releases its mapped input");
    mac_keyboard_apply_modifiers(event_keys, 1);
    check(mac_keyboard_held(event_keys[0], event_keys[1], 56),
          "left Shift flagsChanged presses handbrake");
    mac_keyboard_apply_modifiers(event_keys, 0);
    check(!mac_keyboard_held(event_keys[0], event_keys[1], 56),
          "left Shift flagsChanged release clears handbrake");
    event_keys[0] = key(56);
    mac_keyboard_clear(event_keys);
    check(!event_keys[0] && !event_keys[1], "focus loss clears every held key");
    mac_keyboard_apply_modifiers(event_keys, 0);
    check(!event_keys[0], "Shift release after focus loss stays released");
    mac_keyboard_apply_modifiers(event_keys, 1);
    mac_keyboard_apply_modifiers(event_keys, 1);
    check(event_keys[0] == key(56), "repeated modifier state is idempotent");
    mapped = mac_keyboard_pad_state(key(36) | key(53), 0, MAC_KEYBOARD_CONTEXT_BOOT);
    check((mapped.buttons & 0x2008) == 0x2008,
          "Enter starts from the boot screen and Escape backs out of menus");
    mapped = mac_keyboard_pad_state(key(36) | key(53), 0, MAC_KEYBOARD_CONTEXT_FRONTEND);
    check((mapped.buttons & 0x6000) == 0x6000,
          "Enter confirms and Escape goes back in front-end menus");
    mapped = mac_keyboard_pad_state(key(36) | key(53), 0, MAC_KEYBOARD_CONTEXT_GAMEPLAY);
    check((mapped.buttons & 0x0008) && !(mapped.buttons & 0x4000),
          "Escape pauses from gameplay while Enter has no gameplay action");
    mapped = mac_keyboard_pad_state(key(36) | key(53), 0, MAC_KEYBOARD_CONTEXT_PAUSE);
    check((mapped.buttons & 0x4008) == 0x4008,
          "Enter confirms and Escape resumes from pause");
    MacKeyboardContext previous = MAC_KEYBOARD_CONTEXT_FRONTEND;
    uint64_t transition_keys[2] = {0, key(126)}, blocked[2] = {0, 0};
    mac_keyboard_context_transition(MAC_KEYBOARD_CONTEXT_GAMEPLAY, &previous,
                                    transition_keys, blocked);
    mapped = mac_keyboard_pad_state(transition_keys[0] & ~blocked[0],
        transition_keys[1] & ~blocked[1], MAC_KEYBOARD_CONTEXT_GAMEPLAY);
    check(!mapped.buttons, "menu key held during transition cannot become throttle");
    transition_keys[1] = 0;
    mac_keyboard_context_transition(MAC_KEYBOARD_CONTEXT_GAMEPLAY, &previous,
                                    transition_keys, blocked);
    transition_keys[1] = key(126);
    mac_keyboard_context_transition(MAC_KEYBOARD_CONTEXT_GAMEPLAY, &previous,
                                    transition_keys, blocked);
    mapped = mac_keyboard_pad_state(transition_keys[0] & ~blocked[0],
        transition_keys[1] & ~blocked[1], MAC_KEYBOARD_CONTEXT_GAMEPLAY);
    check(mapped.buttons & 0x0200, "throttle works after the transition key is released");
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
    if (argc == 2 && !strcmp(argv[1], "--keyboard")) {
        unsetenv("PAD_NO_KEYBOARD"); unsetenv("PAD_FILE");
        keyboard_context = MAC_KEYBOARD_CONTEXT_FRONTEND;
        test_ms += 4; get(0);
        keyboard_keys[1] = key(126);
        test_ms += 4; get(0);
        check((button(CELL_PAD_BTN_OFFSET_DIGITAL1) & 0x10) &&
              !(button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 2), "menu Up navigates without throttle in guest packet");
        keyboard_context = MAC_KEYBOARD_CONTEXT_GAMEPLAY;
        test_ms += 4; get(0);
        check(!(button(CELL_PAD_BTN_OFFSET_DIGITAL1) & 0x10) &&
              !(button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 2), "context change blocks a held menu key in guest packet");
        keyboard_keys[1] = 0; test_ms += 4; get(0);
        keyboard_keys[1] = key(126); test_ms += 4; get(0);
        check((button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 2) &&
              button(CELL_PAD_BTN_OFFSET_PRESS_R2) == 255 &&
              button(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y) == 128,
              "race Up reaches guest as full throttle with neutral lean");
        keyboard_keys[0] = key(13); test_ms += 4; get(0);
        check(button(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y) == 0 &&
              button(CELL_PAD_BTN_OFFSET_PRESS_R2) == 255, "lean and throttle coexist in guest packet");
        keyboard_keys[0] = keyboard_keys[1] = 0; test_ms += 4; get(0);
        keyboard_context = MAC_KEYBOARD_CONTEXT_PAUSE; test_ms += 4; get(0);
        keyboard_keys[0] = key(36); keyboard_keys[1] = key(125); test_ms += 4; get(0);
        check((button(CELL_PAD_BTN_OFFSET_DIGITAL1) & 0x40) &&
              (button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 0x40) &&
              button(CELL_PAD_BTN_OFFSET_PRESS_L2) == 0, "pause menu Down and Enter navigate and confirm without braking");
        keyboard_keys[0] = key(53); keyboard_keys[1] = 0; test_ms += 4; get(0);
        check((button(CELL_PAD_BTN_OFFSET_DIGITAL1) & 8) &&
              !(button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 0x40), "Escape resumes without a simultaneous confirmation");
        unlink(path); free(vm_base); return failures ? 1 : 0;
    }
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
        keyboard_keys[0] = key(56); /* left Shift */
        test_ms += 4; get(0);
        check(button(CELL_PAD_BTN_OFFSET_DIGITAL2) & 0x20,
              "real keyboard input still works after script cancellation");
        keyboard_keys[0] = 0;
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
