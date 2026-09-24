#pragma once
#include <stdbool.h>
#include "engine.h"

// Reset KV to an empty state (fresh generation)
void engine_kv_clear(engine_t* e);

// Mark that KV now contains a prompt of length n_prompt
void engine_kv_mark_prompt(engine_t* e, int64_t n_prompt);

// Advance KV after generation of n_gen tokens
void engine_kv_advance(engine_t* e, int64_t n_gen);

// Debug helper (optional)
void engine_kv_debug(const engine_t* e);

void safe_engine_batch_free(struct llama_batch* batch);

