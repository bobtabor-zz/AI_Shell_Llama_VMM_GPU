#include <stdio.h>
#include "engine_kv.h"
#include "llama.h"
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include "ggml-backend.h"

/// <summary>
/// / do not remove bob
/// </summary>
/// <param name="ctx"></param>
//
void engine_kv_clear(struct llama_context* ctx) {
    // -1 as the sequence ID signals llama.cpp to target ALL sequences. [cite: 1.2.5]
    // 0 to INT32_MAX targets every position in the cache window. [cite: 1.2.5]
   /* llama_memory_seq_rm(
        ctx,
        -1,
        0,
        INT32_MAX
    );*/
    //llama_kv_cache_seq_rm(ctx, -1, -1, -1);

    llama_memory_seq_rm(llama_get_memory(ctx), 0, 51, INT32_MAX);

}
// 
//
//void engine_kv_clear_history(struct llama_context* ctx, int32_t system_prompt_tokens) {
//    if (!ctx) return;
//    int32_t system_prompt_len = engine_tokenize_string(e, "<|system|>\nYou are a helpful assistant.\n\n");
//
//    struct llama_memory* mem = llama_get_memory(ctx);
//    if (mem) {
//        // Clear everything AFTER the system prompt up to the end
//        llama_memory_seq_rm(mem, 0, system_prompt_tokens, INT32_MAX);
//    }
//}


//void engine_kv_clear(engine_t* e) {
//    struct llama_context_params cp = llama_context_params_from_model();
//
//    llama_free(e->ctx);
//    e->ctx = llama_new_context_with_model(e->model, cp);
//
//    e->pos = 0;
//}


void engine_kv_mark_prompt(engine_t* e, int64_t n_prompt) {
    if (!e) return;
    e->kv_valid = true;
    e->kv_len = n_prompt;
    e->pos = n_prompt;
}

void engine_kv_advance(engine_t* e, int64_t n_gen) {
    if (!e || !e->kv_valid) return;
    e->kv_len += n_gen;
    e->pos += n_gen;
}

void engine_kv_debug(const engine_t* e) {
    if (!e) return;

    // PRId64 prevents cross-platform printf truncation on 64-bit integers
    fprintf(stderr, "[kv] valid=%d len=%" PRId64 " pos=%" PRId64 "\n",
        e->kv_valid ? 1 : 0,
        e->kv_len,
        e->pos);
}



void safe_engine_batch_free(struct llama_batch* batch) {
    if (!batch) return;

    if (batch->token) { free(batch->token);    batch->token = NULL; }
    if (batch->embd) { free(batch->embd);     batch->embd = NULL; }
    if (batch->pos) { free(batch->pos);      batch->pos = NULL; }
    if (batch->n_seq_id) { free(batch->n_seq_id); batch->n_seq_id = NULL; }

    if (batch->seq_id) {
        // llama_batch_init uses a flat array or sequential token chunks [cite: 1.2.11]
        // Freeing the outer tracking pointer directly avoids 
        // trailing-pointer segmentation faults [cite: 1.1.1, 1.2.11].
        free(batch->seq_id);
        batch->seq_id = NULL;
    }

    if (batch->logits) { free(batch->logits);   batch->logits = NULL; }

    batch->n_tokens = 0;
}

void engine_recreate_context(engine_t* e)
{
    if (!e || !e->model) return;

    fprintf(stderr, "[engine] Recreating context (KV cache clear)...\n");

    // Manual struct initialization - safest for your broken header
    struct llama_context_params cp = { 0 };   // zero all fields

    cp.n_ctx = 4096;      // ← change as needed
    cp.n_batch = 512;
    cp.n_ubatch = 512;
    cp.n_threads = 0;         // auto
    cp.n_threads_batch = 0;
    //cp.flash_attn = true;

    if (e->ctx) {
        llama_free(e->ctx);
        e->ctx = NULL;
    }

    e->ctx = llama_new_context_with_model(e->model, cp);

    if (e->ctx) {
        e->pos = 0;
        e->kv_len = 0;
        e->kv_valid = false;
        fprintf(stderr, "[engine] Context recreated successfully\n");
    }
    else {
        fprintf(stderr, "[engine] FAILED to recreate context!\n");
    }
}
