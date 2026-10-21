/* Measurement-only switch for spu_float_probe.h.
 * MOTORSTORM_SPU_FLOAT_PROBE=1 keeps the probe on, =alt turns it on during
 * odd ten-second UTC windows ((utc_seconds / 10) & 1), anything else leaves
 * it off. Analysis assigns samples to windows from their UTC time alone. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

volatile int g_ms_spu_float_probe;

static void* alternate(void* unused)
{
    (void)unused;
    for (;;) {
        g_ms_spu_float_probe = (int)((time(NULL) / 10) & 1);
        usleep(50000);
    }
    return NULL;
}

__attribute__((constructor)) static void ms_spu_float_probe_start(void)
{
    const char* mode = getenv("MOTORSTORM_SPU_FLOAT_PROBE");
    if (mode && strcmp(mode, "1") == 0) {
        g_ms_spu_float_probe = 1;
    } else if (mode && strcmp(mode, "alt") == 0) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, alternate, NULL) == 0) pthread_detach(thread);
    }
    fprintf(stderr, "[spu-float-probe] mode=%s\n", mode ? mode : "off");
}
