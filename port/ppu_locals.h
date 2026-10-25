#pragma once
#include <cstdlib>
#include <cstdio>

// One immutable process-wide choice. Restart after changing the launch
// environment; no per-instruction flag loads, allocation or logging.
inline bool ms_ppu_locals_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOTORSTORM_PPU_LOCALS");
        const bool active = value && value[0] == '1';
        std::fprintf(stderr, "[ppu-locals] four-function pilot %s\n",
                     active ? "enabled" : "original");
        return active;
    }();
    return enabled;
}
