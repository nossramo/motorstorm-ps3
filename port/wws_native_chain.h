#pragma once
/* Private, pointer-free replay of a complete WWS object-classification batch.
 * The generated adapter is installed only in the hash-checked image 205.
 * Captures contain game data and remain in ignored local diagnostics folders. */
#include "spu_context.h"

typedef struct ms_wws_chain_fixture {
    uint32_t header[32];
    u128 gpr[128];
    uint8_t ls[SPU_LS_SIZE];
} ms_wws_chain_fixture;

void ms_wws_chain_capture(const spu_context* ctx, unsigned entry);
int ms_wws_chain_try(spu_context* ctx);
int ms_wws_sort_try(spu_context* ctx);
/* Semantic batch entry: native objects/planes/list operations; no guest GPRs,
 * instruction dispatch, stack frames or channel callbacks inside the kernel. */
int ms_wws_classify_batch(uint8_t* memory, unsigned count);
int ms_wws_sort_occluders(uint8_t* memory, unsigned count);
int ms_wws_select_occluders(uint8_t* memory, unsigned count, unsigned* selected);
