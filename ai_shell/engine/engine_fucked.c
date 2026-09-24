// engine.c – incremental llama.cpp-style engine for Llama-3-Instruct
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "llama.h"
#include "engine.h"   // defines engine_t, html_turn_t, engine_turn_t, MAX_TURNS
#include "../include/plugin.h"
#include "../vmm/vmm.h"


char* g_plugin_result = NULL;

static llama_token tokenize_single(
    const struct llama_vocab* vocab,
    const char* s
) {
    llama_token tmp[8];
    int n = llama_tokenize(
        vocab,
        s,
        (int)strlen(s),
        tmp,
        8,
        true,   // add_special
        true    // parse_special
    );
    return (n > 0 ? tmp[0] : -1);
}



char* engine_json_extract_string(char* s, char* out, size_t out_sz)
{
    size_t pos = 0;
    s++; // skip opening quote

    while (*s && pos < out_sz - 1) {
        if (*s == '\\') {
            s++;
            if (*s == 'n') out[pos++] = '\n';
            else if (*s == 't') out[pos++] = '\t';
            else if (*s == '\\') out[pos++] = '\\';
            else if (*s == '"') out[pos++] = '"';
            else out[pos++] = *s;
            s++;
        }
        else if (*s == '"') {
            s++; // closing quote
            break;
        }
        else {
            out[pos++] = *s++;
        }
    }

    out[pos] = 0;
    return s;
}


// ------------------------------------------------------------
// Runtime init
// ------------------------------------------------------------

void engine_init_runtime(engine_t* e) {
    if (!e) return;

    e->seq_id = 0;
    e->html_seq_id = 0;
    e->pos = 0;

    e->temp = 0.7f;
    e->top_k = 20;
    e->top_p = 0.9f;

    e->kv_valid = false;
    e->kv_len = 0;

    e->html_n_turns = 0;
    e->n_turns = 0;
}


// ============================================================================
// MODEL FAMILY ENUM + GLOBAL
// ============================================================================
typedef enum {
    MODEL_LLAMA3,
    MODEL_PHI3,
    MODEL_SMOLLM,
    MODEL_MISTRAL,
    MODEL_QWEN,
    MODEL_GEMMA,
    MODEL_LLAMA2,
    MODEL_UNKNOWN
} model_family_t;

model_family_t g_model_family = MODEL_UNKNOWN;


// ============================================================================
// MODEL FAMILY DETECTOR (llama.cpp metadata + filename fallback)
// ============================================================================
static model_family_t detect_model_family(const char* path, struct llama_model* model) {
    const char* arch = NULL;


    // ---- Filename fallback ----
    char lower[4096];
    snprintf(lower, sizeof(lower), "%s", path);
    for (char* p = lower; *p; ++p) *p = (char)tolower(*p);

    if (arch) {
        if (strcmp(arch, "llama3") == 0) return MODEL_LLAMA3;
        if (strcmp(arch, "phi3") == 0) return MODEL_PHI3;
        if (strcmp(arch, "smollm") == 0) return MODEL_SMOLLM;
        if (strcmp(arch, "mistral") == 0) return MODEL_MISTRAL;
        if (strcmp(arch, "qwen2") == 0 || strcmp(arch, "qwen") == 0) return MODEL_QWEN;
        if (strcmp(arch, "gemma") == 0) return MODEL_GEMMA;
        if (strcmp(arch, "llama") == 0) return MODEL_LLAMA2;
    }

    // ---- Filename fallback ----
    char lower2[4096];
    snprintf(lower2, sizeof(lower2), "%s", path);
    for (char* p = lower2; *p; ++p) *p = (char)tolower(*p);

    if (strstr(lower2, "llama-3") || strstr(lower2, "llama3")) return MODEL_LLAMA3;
    if (strstr(lower2, "phi-3") || strstr(lower2, "phi3"))   return MODEL_PHI3;
    if (strstr(lower2, "smollm"))                             return MODEL_SMOLLM;
    if (strstr(lower2, "mistral"))                            return MODEL_MISTRAL;
    if (strstr(lower2, "qwen"))                               return MODEL_QWEN;
    if (strstr(lower2, "gemma"))                              return MODEL_GEMMA;
    if (strstr(lower2, "llama-2") || strstr(lower2 , "llama2")) return MODEL_LLAMA2;

    return MODEL_UNKNOWN;
}


// ============================================================================
// WRAPPER FUNCTIONS (EXACTLY ONE COPY OF EACH)
// ============================================================================

// ---- Llama‑3 ----
static void wrap_llama3_system(char* dst, size_t n, const char* sys) {
    snprintf(dst, n,
        "<|begin_of_text|><|start_header_id|>system<|end_header_id|>\n\n"
        "%s<|eot_id|>\n",
        sys
    );
}

static void wrap_llama3_user(char* dst, size_t n, const char* usr) {
    snprintf(dst, n,
        "<|start_header_id|>user<|end_header_id|>\n\n"
        "%s<|eot_id|>\n"
        "<|start_header_id|>assistant<|end_header_id|>\n\n",
        usr
    );
}


// ---- Phi‑3 ----
static void wrap_phi3_system(char* dst, size_t n, const char* sys) {
    snprintf(dst, n, "<|system|>\n%s\n", sys);
}

static void wrap_phi3_user(char* dst, size_t n, const char* usr) {
    snprintf(dst, n, "<|user|>\n%s\n<|assistant|>\n", usr);
}

//// ---- SmolLM ----

// System prompt
static void wrap_smollm_system(char* dst, size_t n, const char* sys) {
    snprintf(dst, n,
        "<|im_start|>system\n"
        "%s\n"
        "<|im_end|>\n",
        sys
    );
}

// User message
static void wrap_smollm_user(char* dst, size_t n, const char* usr) {
    snprintf(dst, n,
        "<|im_start|>user\n"
        "%s\n"
        "<|im_end|>\n"
        "<|im_start|>assistant\n",   // Important: end with this so the model starts generating
        usr
    );
}



// ---- Mistral ----
static void wrap_mistral_system(char* dst, size_t n, const char* sys) {
    snprintf(dst, n, "<s>[INST] <<SYS>>\n%s\n<</SYS>>\n", sys);
}

static void wrap_mistral_user(char* dst, size_t n, const char* usr) {
    snprintf(dst, n, "%s [/INST]", usr);
}


// ---- Qwen ----
static void wrap_qwen_system(char* dst, size_t n, const char* sys) {
    snprintf(dst, n, "<|im_start|>system\n%s<|im_end|>\n", sys);
}

static void wrap_qwen_user(char* dst, size_t n, const char* usr) {
    snprintf(dst, n,
        "<|im_start|>user\n%s<|im_end|>\n"
        "<|im_start|>assistant\n",
        usr
    );
}


// ---- Gemma ----
static void wrap_gemma_system(char* dst, size_t n, const char* sys) {
    snprintf(dst, n, "<bos><start_of_turn>system\n%s<end_of_turn>\n", sys);
}

//static void wrap_gemma_user(char* dst, size_t n, const char* usr) {
//    snprintf(dst, n,
//        "<start_of_turn>user\n%s<end_of_turn>\n"
//        "<start_of_turn>assistant\n",
//        usr
//    );
//}

static void wrap_gemma_user(char* dst, size_t n, const char* usr) {
    snprintf(dst, n,
        "<bos><start_of_turn>user\n%s<end_of_turn>\n"
        "<start_of_turn>model\n",
        usr);
}

// ---- Llama‑2 ----
static void wrap_llama2_system(char* dst, size_t n, const char* sys) {
    snprintf(dst, n, "[INST] <<SYS>>\n%s\n<</SYS>>\n", sys);
}

static void wrap_llama2_user(char* dst, size_t n, const char* usr) {
    snprintf(dst, n, "%s [/INST]", usr);
}


// ============================================================================
// UNIVERSAL DISPATCHER (CALL THESE FROM FEED_SYSTEM / FEED_USER)
// ============================================================================
void engine_wrap_system(char* dst, size_t n, const char* sys) {
    switch (g_model_family) {
    case MODEL_LLAMA3:  wrap_llama3_system(dst, n, sys); break;
    case MODEL_PHI3:    wrap_phi3_system(dst, n, sys); break;
    case MODEL_SMOLLM:  wrap_smollm_system(dst, n, sys); break;
    case MODEL_MISTRAL: wrap_mistral_system(dst, n, sys); break;
    case MODEL_QWEN:    wrap_qwen_system(dst, n, sys); break;
    case MODEL_GEMMA:   wrap_gemma_system(dst, n, sys); break;
    case MODEL_LLAMA2:  wrap_llama2_system(dst, n, sys); break;
    default: snprintf(dst, n, "%s", sys); break;
    }
}

void engine_wrap_user(char* dst, size_t n, const char* usr) {
    switch (g_model_family) {
    case MODEL_LLAMA3:  wrap_llama3_user(dst, n, usr); break;
    case MODEL_PHI3:    wrap_phi3_user(dst, n, usr); break;
    case MODEL_SMOLLM:  wrap_smollm_user(dst, n, usr); break;
    case MODEL_MISTRAL: wrap_mistral_user(dst, n, usr); break;
    case MODEL_QWEN:    wrap_qwen_user(dst, n, usr); break;
    case MODEL_GEMMA:   wrap_gemma_user(dst, n, usr); break;
    case MODEL_LLAMA2:  wrap_llama2_user(dst, n, usr); break;
    default: snprintf(dst, n, "%s", usr); break;
    }
}

// Patch 2: safe single-token decode helper
// Returns 0 on success, non-zero on failure.
int engine_decode_single_token(engine_t* e, llama_token token, int request_logits) {
    llama_pos pos = e->n_past;
    int32_t n_seq = 1;
    int32_t seq0 = e->seq_id;
    int32_t* seq_ptrs[1] = { &seq0 };
    int8_t logits_arr[1] = { request_logits ? 1 : 0 };

    struct llama_batch batch = { 0 };
    batch.n_tokens = 1;
    batch.token = (llama_token[]){ token };
    batch.pos = &pos;
    batch.n_seq_id = &n_seq;
    batch.seq_id = seq_ptrs;
    batch.logits = logits_arr;

    int rc = llama_decode(e->ctx, batch);
    if (rc != 0) return rc;

    e->n_past++;
    e->pos = e->n_past;
    e->kv_valid = true;
    e->kv_len = e->n_past;
    return 0;
}


// ==========================================
// 🛠️ FIX #1: VALIDATE THE SAMPLER CHAIN SETUP
// ==========================================
struct llama_sampler* engine_create_sampler_chain(engine_t* e, const char* gbnf_grammar) {
    // 1. Initialize the root chain pipeline
    struct llama_sampler* chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (!chain) return NULL;

    // 2. Instantiate your grammar restriction node
    const struct llama_vocab* vocab = llama_model_get_vocab(e->model);
    struct llama_sampler* grammar_node = llama_sampler_init_grammar(vocab, gbnf_grammar, "root");

    // CRITICAL SAFETY CHECK: If the GBNF string has a syntax error, 
    // llama_sampler_init_grammar returns NULL. BREATHE and catch it here!
    if (grammar_node == NULL) {
        fprintf(stderr, "ERROR: GBNF grammar compilation failed! Check your string syntax.\n");
        // Add a standard greedy fallback node instead of breaking the pipeline
        llama_sampler_chain_add(chain, llama_sampler_init_greedy());
        return chain;
    }

    // 3. Only append the node if it's explicitly valid
    llama_sampler_chain_add(chain, grammar_node);

    // 4. Always ensure a final selector node (Greedy or Top-K) terminates the pipeline
    llama_sampler_chain_add(chain, llama_sampler_init_greedy());

    return chain;
}

// ==========================================
// 🛠️ FIX #2: UPDATE THE INFERENCE SAMPLER LOOP
// ==========================================
static llama_token engine_sample_next(engine_t* e) {
    const float* logits = llama_get_logits(e->ctx);
    if (!logits) return -1;

    // Hard check: Ensure the sampler chain structure actually exists
    if (!e->sampler) return -1;

    for (int i = 0; i < e->cached_vocab_size; ++i) {
        e->candidates_buf[i].id = (llama_token)i;
        e->candidates_buf[i].logit = logits[i];
        e->candidates_buf[i].p = 0.0f;
    }

    // Modern layout instantiation
    llama_token_data_array candidates_array = {
        .data = e->candidates_buf,
        .size = (size_t)e->cached_vocab_size,
        .selected = -1,
        .sorted = false
    };

    // This modifies candidates_array in-place
    llama_sampler_apply(e->sampler, &candidates_array);

    // Double-check against token elimination deadlocks
    if (candidates_array.size == 0) {
        return llama_vocab_eos(llama_model_get_vocab(e->model));
    }

    // 🌟 THE FIX: Pass index 0 to select the top candidate chosen by the chain
    llama_token tok = llama_sampler_sample(e->sampler, e->ctx, 0);

    // Register the accepted token choice to advance state trackers
    llama_sampler_accept(e->sampler, tok);

    return tok;
}

// 1. Pristine Replacement for llama_batch_clear
static inline void engine_batch_clear(struct llama_batch* batch) {
    batch->n_tokens = 0;
}

// 2. Pristine Replacement for llama_batch_add
static inline void engine_batch_add(
    struct llama_batch* batch,
    llama_token          id,
    llama_pos            pos,
    llama_seq_id         seq_id, // Pass a raw single ID variable
    bool                 logits)
{
    int index = batch->n_tokens;

    batch->token[index] = id;
    batch->pos[index] = pos;
    batch->n_seq_id[index] = 1;

    // 🌟 THE PTR FIX: Point the multi-sequence matrix row directly to the allocation slot
    batch->seq_id[index][0] = seq_id;

    // Explicit int8_t typecast configuration
    batch->logits[index] = logits ? 1 : 0;

    batch->n_tokens++;
}

int engine_feed_tokens_llama3(engine_t* e, const llama_token* tokens, int n_tokens) {
    if (!e || !e->ctx || !tokens || n_tokens <= 0)
        return -1;

    struct llama_batch batch = llama_batch_init(n_tokens, 0, 1);
    engine_batch_clear(&batch);

    // Safe feed: use e->n_past as canonical KV length
    llama_pos pos = e->n_past;
    for (int i = 0; i < n_tokens; ++i) {
        bool request_logits = (i == n_tokens - 1);
        engine_batch_add(&batch, tokens[i], pos, e->seq_id, request_logits);
        pos++;
    }

    // Basic engine state
    fprintf(stderr, "[DBG] e->n_past=%d e->pos=%d e->kv_len=%lld n_tokens=%d\n",
        e->n_past, e->pos, (long long)e->kv_len, n_tokens);

    // Batch header checks
    fprintf(stderr, "[DBG] batch.n_tokens=%d batch.pos[0]=%d batch.n_seq_id[0]=%d seq_id_ptr=%p\n",
        batch.n_tokens, (int)batch.pos[0], batch.n_seq_id[0],
        (void*)(batch.seq_id ? batch.seq_id[0] : NULL));

    // First token numeric and vocab bounds
    int first_tok = (int)batch.token[0];
    int vocab_n = llama_vocab_n_tokens(llama_model_get_vocab(e->model));
    fprintf(stderr, "[DBG] first_tok=%d vocab_n=%d\n", first_tok, vocab_n);

    // Token->piece and raw bytes (hex) to inspect encoding
    char piece[512] = { 0 };
    int piece_len = llama_token_to_piece(llama_model_get_vocab(e->model),
        (llama_token)first_tok, piece, sizeof(piece), 0, false);
    fprintf(stderr, "[DBG] piece_len=%d piece=\"%s\"\n", piece_len, piece_len > 0 ? piece : "<invalid>");
    for (int i = 0; i < piece_len && i < 64; ++i) fprintf(stderr, "%02X ", (unsigned char)piece[i]);
    fprintf(stderr, "\n");
     
    if ((llama_pos)batch.pos[0] != e->n_past) {
        fprintf(stderr, "[ERR] batch.pos[0]=%d != e->n_past=%d\n", (int)batch.pos[0], e->n_past);
        return -1;
    }

    // Decode the whole batch once
    int rc = llama_decode(e->ctx, batch);
    if (rc != 0) {
        fprintf(stderr, "[FATAL] llama_decode failed with rc=%d; e->n_past=%d; attempted_start_pos=%d\n",
            rc, e->n_past, (int)(e->n_past));
        return rc;
    }

    // Commit state only after successful decode
    e->n_past = pos;
    e->pos = e->n_past;      // keep legacy alias in sync
    e->kv_valid = true;
    e->kv_len = e->n_past;
#if defined(HAVE_LLAMA_SYNCHRONIZE)
    llama_synchronize(e->ctx);
#endif


    e->pos = pos;
    e->kv_valid = true;
    e->kv_len = pos;

    return 0;
}




int engine_tokenize_for_llama3(const struct llama_vocab* vocab, const char* text, llama_token** out_toks) {
    int text_len = (int)strlen(text);

    // 1. FIRST PASS: Pass NULL and 0 to query the exact token count needed.
    // Use negative evaluation fallback if your version follows standard llama.cpp conventions.
    int required_toks = -llama_tokenize(vocab, text, text_len, NULL, 0, true, true);

    if (required_toks <= 0) {
        // Fallback for older llama.cpp versions that return 0 or positive bounds on estimation
        required_toks = text_len + 8;
    }

    // 2. ALLOCATION: Dynamically allocate the exact size required
    *out_toks = (llama_token*)malloc(required_toks * sizeof(llama_token));
    if (!*out_toks) return -1;

    // 3. SECOND PASS: Do the actual tokenization into your allocated buffer
    int actual_toks = llama_tokenize(vocab, text, text_len, *out_toks, required_toks, true, true);

    if (actual_toks < 0) {
        // Buffer was somehow still too small
        free(*out_toks);
        *out_toks = NULL;
        return -1;
    }

    return actual_toks;
}


int engine_feed_system_prompt_llama3(engine_t* e, const char* sys) {
    const struct llama_vocab* vocab = llama_model_get_vocab(e->model);

    char buf[4096];
    engine_wrap_system(buf, sizeof(buf), sys);

    llama_token* toks = NULL;
    int n = engine_tokenize_for_llama3(vocab, buf, &toks);
    if (n <= 0)
        return -1;

    int rc = engine_feed_tokens_llama3(e, toks, n);
    free(toks);
    return rc;
}



int engine_feed_user_llama3(engine_t* e, const char* user) {
    const struct llama_vocab* vocab = llama_model_get_vocab(e->model);

    size_t sz = strlen(user) + 4096;
    char* wrapped = malloc(sz);
    if (!wrapped) return -1;

    engine_wrap_user(wrapped, sz, user);

    llama_token* toks = NULL;
    int n = engine_tokenize_for_llama3(vocab, wrapped, &toks);
    free(wrapped);

    if (n <= 0)
        return -1;

    int rc = engine_feed_tokens_llama3(e, toks, n);
    free(toks);
    return rc;
}

// ------------------------------------------------------------
// Feed system prompt (templated) ONCE per reset/open
// ------------------------------------------------------------

int engine_feed_system_prompt(engine_t* e, const char* system_text) {
    if (!e || !e->model || !e->ctx) return -1;

    const struct llama_vocab* vocab = llama_model_get_vocab(e->model);

    char buf[4096];
    engine_wrap_system(buf, sizeof(buf), system_text);

    llama_token tokens[1024];
    int n_tokens = llama_tokenize(
        vocab,
        buf,
        (int32_t)strlen(buf),
        tokens,
        1024,
        false,  // add_special: false (we wrap them manually)
        true    // FIX: parse_special MUST be true to recognize Llama 3 <|control|> tags!
    );
    if (n_tokens <= 0) return -1;

    struct llama_batch batch = (struct llama_batch){ 0 };

    llama_pos      pos_arr[1024];
    int32_t        n_seq_arr[1024];
    llama_seq_id   seq_id_arr[1024];
    llama_seq_id* seq_ptr_arr[1024];
    int8_t         logits_arr[1024];

    // Array bindings matching your llama.cpp layout specification
    batch.token = tokens;
    batch.pos = pos_arr;
    batch.n_seq_id = n_seq_arr;
    batch.seq_id = seq_ptr_arr;
    batch.logits = logits_arr;
    batch.n_tokens = 0;

    llama_pos pos = e->n_past;


    for (int i = 0; i < n_tokens; ++i) {
        pos_arr[i] = pos;
        n_seq_arr[i] = 1;
        seq_id_arr[i] = e->seq_id;
        seq_ptr_arr[i] = &seq_id_arr[i];
        logits_arr[i] = (i == n_tokens - 1) ? 1 : 0; // Request logits only for the very last token

        batch.n_tokens++;
        pos++;
    }

    // Direct initialization pass evaluation
    int decode_rc = llama_decode(e->ctx, batch);   
    if (decode_rc != 0) {
        // feeding failed, bail out
        return decode_rc;
    }


    // Update your application context states
    e->n_past = pos;
    e->pos = e->n_past;

    e->kv_valid = true;
    e->kv_len = pos;

    return 0;
}



// ------------------------------------------------------------
// Feed user message (templated) incrementally - STABLE NATIVE BATCH
// ------------------------------------------------------------

int engine_feed_user(engine_t* e, const char* user_text_raw) {
    if (!e || !e->model || !e->ctx || !user_text_raw) return -1;

    const struct llama_vocab* vocab = llama_model_get_vocab(e->model);

    // 1. Calculate length and dynamically allocate the wrapped buffer string
    size_t raw_len = strlen(user_text_raw);
    size_t wrapped_size = raw_len + 4096;
    char* wrapped = (char*)malloc(wrapped_size);
    if (!wrapped) return -1;

    engine_wrap_user(wrapped, wrapped_size, user_text_raw);

    // 2. Perform a dry-run token count request
    int n_tokens = llama_tokenize(vocab, wrapped, (int32_t)strlen(wrapped), NULL, 0, false, true);
    if (n_tokens < 0) n_tokens = -n_tokens;
    if (n_tokens <= 0 || n_tokens > 2048) { // Security bounds checking safety check
        free(wrapped);
        return -1;
    }

    // 3. Dynamically allocate a clean array for token identifiers
    llama_token* tokens = (llama_token*)malloc(sizeof(llama_token) * n_tokens);
    if (!tokens) {
        free(wrapped);
        return -1;
    }

    int actual_tokens = llama_tokenize(vocab, wrapped, (int32_t)strlen(wrapped), tokens, n_tokens, false, true);
    free(wrapped);
    if (actual_tokens <= 0) {
        free(tokens);
        return -1;
    }

    // =========================================================================
    // FIX: STACK ALLOCATE THE ARRAYS TO ELIMINATE LLAMA_BATCH_FREE() COMPLETELY
    // =========================================================================
    struct llama_batch batch = (struct llama_batch){ 0 };

    // Dynamically size these stack arrays using Visual Studio VLA or standard local buffers
    // 2048 provides an immense safety ceiling for common incoming user turns
    #define USER_FEED_MAX_TOKENS 2048
    if (actual_tokens > USER_FEED_MAX_TOKENS) {
        free(tokens);
        return -1;
    }

    llama_pos      pos_arr[USER_FEED_MAX_TOKENS];
    int32_t        n_seq_arr[USER_FEED_MAX_TOKENS];
    llama_seq_id   seq_id_arr[USER_FEED_MAX_TOKENS];
    llama_seq_id* seq_ptr_arr[USER_FEED_MAX_TOKENS];
    int8_t         logits_arr[USER_FEED_MAX_TOKENS];

    // Assign your framework's exact pointer properties safely
    batch.token = tokens;
    batch.pos = pos_arr;
    batch.n_seq_id = n_seq_arr;
    batch.seq_id = seq_ptr_arr;
    batch.logits = logits_arr;
    batch.n_tokens = 0;

    llama_pos current_pos = e->n_past;

    // 5. Explicit assignment mapping
    for (int i = 0; i < actual_tokens; ++i) {
        pos_arr[i] = current_pos;
        n_seq_arr[i] = 1;
        seq_id_arr[i] = e->seq_id;
        seq_ptr_arr[i] = &seq_id_arr[i];
        logits_arr[i] = (i == actual_tokens - 1) ? 1 : 0; // Logits requested for sampler only

        batch.n_tokens++;
        current_pos++;
    }

    // 6. Execute inference decoding safely
    int decode_rc = llama_decode(e->ctx, batch);
    if (decode_rc != 0) {
        // feeding failed, bail out
        return decode_rc;
    }

    // 7. Dynamic memory cleanup 
    free(tokens);


    // 8. Commit tracker values on success states
    e->pos = current_pos;
    e->kv_valid = true;
    e->kv_len = current_pos;

    return 0;
}

static bool is_tool_call(const char* s) {
    return strstr(s, "\"tool\"") && strstr(s, "{") && strstr(s, "}");
}

// Replace any existing parse_tool_call implementation with this.
// Requires <stdlib.h>, <string.h>, <stdio.h>. If you have cJSON, include it and enable that branch.

tool_call_t parse_tool_call(const char* s) {
    tool_call_t call = { 0 };
    if (!s) return call;

    // Try a robust JSON parse if cJSON is available
#ifdef HAVE_CJSON
    cJSON* root = cJSON_Parse(s);
    if (root) {
        cJSON* name = cJSON_GetObjectItemCaseSensitive(root, "name");
        if (!name) name = cJSON_GetObjectItemCaseSensitive(root, "tool");
        if (cJSON_IsString(name) && name->valuestring) {
            call.name = _strdup(name->valuestring);
        }

        cJSON* args = cJSON_GetObjectItemCaseSensitive(root, "arguments");
        if (!args) args = cJSON_GetObjectItemCaseSensitive(root, "query");
        if (args) {
            char* txt = cJSON_PrintUnformatted(args);
            if (txt) {
                call.arguments = _strdup(txt);
                cJSON_free(txt);
            }
        }
        cJSON_Delete(root);
        return call;
    }
#endif

    // Fallback: try to parse the simple { "tool":"X", "query":"Y" } pattern
    {
        char tool_buf[128] = { 0 };
        char query_buf[1024] = { 0 };

        // This sscanf tolerates whitespace and simple JSON formatting.
        int matched = sscanf(s, " { \"tool\" : \"%127[^\"]\" , \"query\" : \"%1023[^\"]\" } ",
            tool_buf, query_buf);

        if (matched >= 1) {
            if (tool_buf[0]) call.name = _strdup(tool_buf);
            if (matched == 2 && query_buf[0]) call.arguments = _strdup(query_buf);
            return call;
        }

        // Another fallback: try { "tool":"X" } only
        matched = sscanf(s, " { \"tool\" : \"%127[^\"]\" } ", tool_buf);
        if (matched == 1 && tool_buf[0]) {
            call.name = _strdup(tool_buf);
            return call;
        }
    }

    // If nothing matched, return empty call (caller must check call.name)
    return call;
}

// Returns 1 if the JSON looks like a tool call
int is_universal_tool_json(const char* json) {
    if (!json) return 0;

    // Must start with '{' and end with '}'
    size_t len = strlen(json);
    if (len < 2) return 0;
    if (json[0] != '{' || json[len - 1] != '}') return 0;

    // Look for required fields
    if (strstr(json, "\"tool\"") ||
        strstr(json, "\"tool_call\"") ||
        strstr(json, "\"function\"") ||
        strstr(json, "\"arguments\"")) {
        return 1;
    }

    return 0;
}

// Attempts to extract the FIRST complete JSON object from toolbuf.
// Returns 1 if found and copied into out_json.
int extract_first_json(const char* toolbuf, char* out_json, size_t out_size) {
    const char* start = strchr(toolbuf, '{');
    if (!start) return 0;

    const char* end = strchr(start, '}');
    if (!end) return 0;

    size_t len = (end - start) + 1;
    if (len >= out_size) return 0;

    memcpy(out_json, start, len);
    out_json[len] = '\0';
    return 1;
}

int run_tool_and_continue(engine_t* e, tool_call_t* call) {
    if (!e || !call || !call->name) return -1;

    // Tool name must match what the model emits
    if (strcmp(call->name, "websearch") != 0)
        return -1;

    // -------------------------------
    // 1. Extract query text
    // -------------------------------
    char query_copy[1024] = { 0 };

    if (call->arguments && call->arguments[0]) {
        // Try to extract {"query":"..."}
        const char* p = strstr(call->arguments, "\"query\"");
        if (p) {
            const char* q = strchr(p, ':');
            if (q) {
                q = strchr(q, '"');
                if (q) {
                    q++;
                    const char* end = strchr(q, '"');
                    if (end && end > q) {
                        size_t len = (size_t)(end - q);
                        if (len >= sizeof(query_copy)) len = sizeof(query_copy) - 1;
                        memcpy(query_copy, q, len);
                        query_copy[len] = '\0';
                    }
                }
            }
        }

        // If extraction failed, fall back to raw arguments
        if (query_copy[0] == '\0') {
            strncpy(query_copy, call->arguments, sizeof(query_copy) - 1);
        }
    }
    else {
        return -1;
    }

    // -------------------------------
    // 2. Split into argv[]
    // -------------------------------
    char* argv[32];
    int argc = 0;

    char* tok = strtok(query_copy, " ");
    while (tok && argc < 31) {
        argv[argc++] = tok;
        tok = strtok(NULL, " ");
    }
    argv[argc] = NULL;

    // -------------------------------
    // 3. Call your plugin
    // -------------------------------
    char* result_json = plugin_websearch(argc, argv);
    if (!result_json) {
        result_json = _strdup("{\"error\":\"websearch_failed\"}");
    }

    // -------------------------------
    // 4. Wrap result for the model
    // -------------------------------
    char buf[8192];
    snprintf(buf, sizeof(buf),
        "<|im_start|>assistant\n"
        "%s\n"
        "<|im_end|>\n"
        "<|im_start|>assistant\n",
        result_json
    );

    free(result_json);

    // -------------------------------
    // 5. Feed tool output back into model
    // -------------------------------
    int rc = engine_feed_user(e, buf);
    if (rc != 0) return -1;

    return 0;
}

bool extract_first_json_block(const char* buf, char* out, size_t out_sz) {
    if (!buf || !out || out_sz == 0) return false;

    const char* p = strchr(buf, '{');
    if (!p) return false;

    int depth = 0;
    const char* q = p;
    while (*q) {
        if (*q == '{') {
            depth++;
        }
        else if (*q == '}') {
            depth--;
            if (depth == 0) {
                size_t len = (size_t)(q - p + 1);
                if (len >= out_sz) return false;
                memcpy(out, p, len);
                out[len] = '\0';
                return true;
            }
        }
        q++;
    }
    return false;
}

int engine_decode_batch(engine_t* e, const llama_token* toks, int n_toks) {

    llama_batch batch = llama_batch_init(n_toks, 0, 1);

    llama_pos pos = e->n_past;

    for (int i = 0; i < n_toks; i++) {
        batch.token[i] = toks[i];
        batch.pos[i] = pos + i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = e->seq_id;
        batch.logits[i] = (i == n_toks - 1);   // only last token needs logits
    }

    int rc = llama_decode(e->ctx, batch);
    if (rc != 0) {
        llama_batch_free(batch);
        return rc;
    }

    e->n_past += n_toks;
    e->pos = e->n_past;

    llama_batch_free(batch);
    return 0;
}


typedef enum { GEN_OK = 0, GEN_TOOL = 2, GEN_ERROR = -1 } gen_result_t;

gen_result_t engine_generate_reply(
    engine_t* e,
    char* out,
    size_t out_size,
    int max_tokens,
    char** last_json_out)
{
    if (last_json_out) *last_json_out = NULL;
    if (!e || !e->ctx || !e->model || !out || out_size == 0)
        return GEN_ERROR;

    const struct llama_vocab* vocab = llama_model_get_vocab(e->model);
    if (!vocab) return GEN_ERROR;

    out[0] = '\0';
    size_t out_len = 0;
    int n_gen = 0;

    char toolbuf[8192] = { 0 };
    size_t toolbuf_len = 0;

    const int vocab_size = llama_vocab_n_tokens(vocab);
    llama_token eos_tok = llama_token_eos(vocab);
    llama_token eot_tok = llama_token_eot(vocab);

    llama_token pending[64];
    int pending_count = 0;
    const int MULTI_DECODE_SIZE = 16;

    while (n_gen < max_tokens && out_len + 8 < out_size) {
        const float* logits = llama_get_logits(e->ctx);
        if (!logits) break;

        int tok = 0;
        float best = logits[0];
        for (int i = 1; i < vocab_size; ++i) {
            if (logits[i] > best) { best = logits[i]; tok = i; }
        }

        if (tok == eos_tok || tok == eot_tok)
            break;

        char piece[2048];
        int n = llama_token_to_piece(vocab, tok, piece, sizeof(piece), 0, false);
        if (n <= 0)
            break;

        if (out_len + n < out_size - 1) {
            memcpy(out + out_len, piece, n);
            out_len += n;
            out[out_len] = '\0';
        }
        else {
            break;
        }

        if (toolbuf_len + n < sizeof(toolbuf) - 1) {
            memcpy(toolbuf + toolbuf_len, piece, n);
            toolbuf_len += n;
            toolbuf[toolbuf_len] = '\0';
        }

        char json_block[4096];
        if (extract_first_json_block(toolbuf, json_block, sizeof(json_block))) {
            if (is_universal_tool_json(json_block)) {
                if (last_json_out)
                    *last_json_out = strdup(json_block);

                if (pending_count > 0) {
                    int rc = engine_decode_batch(e, pending, pending_count);
                    if (rc != 0) return GEN_ERROR;
                }

                return GEN_TOOL;
            }
            toolbuf[0] = '\0';
            toolbuf_len = 0;
        }

        pending[pending_count++] = tok;

        if (pending_count == MULTI_DECODE_SIZE) {
            int rc = engine_decode_batch(e, pending, pending_count);
            if (rc != 0) return GEN_ERROR;
            pending_count = 0;
        }

        n_gen++;
    }

    if (pending_count > 0) {
        int rc = engine_decode_batch(e, pending, pending_count);
        if (rc != 0) return GEN_ERROR;
    }

    return GEN_OK;
}


// Patch 3B: orchestrator - engine_chat_html updated to handle GEN_TOOL properly
int engine_chat_html(
    engine_t* e,
    const char* user_input,
    char* out,
    size_t out_size)
{
    if (!e || !user_input || !out || out_size == 0) return -1;
    out[0] = '\0';

    // Reset engine (ensure engine_reset sets e->n_past = 0)
    engine_reset(e);

    // Feed system prompt + user prompt (use your existing helpers)
    if (g_model_family == MODEL_LLAMA3) {
        // your existing llama3 templating + engine_feed_tokens_llama3
        llama_chat_message msgs[2] = {
            { "system", TOOL_SYSTEM_PROMPT },
            { "user",   user_input }
        };

        // Build formatted string and feed tokens (reuse your existing code)
        int formatted_len = llama_chat_apply_template(NULL, msgs, 2, true, NULL, 0);
        if (formatted_len < 0) return -1;
        char* formatted = (char*)malloc(formatted_len + 1);
        if (!formatted) return -1;
        int final_len = llama_chat_apply_template(NULL, msgs, 2, true, formatted, formatted_len + 1);
        if (final_len < 0) { free(formatted); return -1; }

        llama_token* toks = NULL;
        int n = engine_tokenize_for_llama3(llama_model_get_vocab(e->model), formatted, &toks);
        free(formatted);
        if (n <= 0) { free(toks); return -1; }

        int rc_feed = engine_feed_tokens_llama3(e, toks, n);
        free(toks);
        if (rc_feed != 0) return rc_feed;
    }
    else {
        // non-llama3 path
        engine_feed_system_prompt(e, TOOL_SYSTEM_PROMPT);
        engine_feed_user(e, user_input);
    }

    // First generation pass
    char* last_json = NULL;
    gen_result_t gret = engine_generate_reply(e, out, out_size, 1024, &last_json);

    if (gret == GEN_TOOL && last_json) {
        // Parse tool call (use your parse_tool_call that fills tool_call_t)
        tool_call_t call = parse_tool_call(last_json);
        free(last_json);

        // Run the tool and feed result back into the model
        int run_rc = run_tool_and_continue(e, &call);
        // run_tool_and_continue must feed the tool result into the model (advance e->n_past)
        // and then call engine_generate_reply() again to produce the final answer into 'out'.
        // Your existing run_tool_and_continue returns 1; adapt it to return 0 on success.
        if (run_rc != 0) {
            tool_call_free(&call); // if you have this helper
            return -1;
        }

        tool_call_free(&call);

        // After run_tool_and_continue, engine_generate_reply should have been called inside it
        // and 'out' should contain the final answer. If your run_tool_and_continue does not
        // call engine_generate_reply internally, call it here:
        // char* dummy = NULL;
        // engine_generate_reply(e, out, out_size, 1024, &dummy);
        // if (dummy) free(dummy);

        return (int)strlen(out);
    }
    else if (gret == GEN_OK) {
        return (int)strlen(out);
    }
    else {
        return -1;
    }
}

// ------------------------------------------------------------
// Reset conversation
// ------------------------------------------------------------
void engine_reset(engine_t* e) {
    if (!e) return;

    // Prefer to clear KV if the API exposes it
#if defined(HAVE_LLAMA_KV_CLEAR)
    if (e->ctx) llama_kv_cache_clear(e->ctx);
#else
// Otherwise recreate the context to guarantee an empty KV
    if (engine_recreate_context(e) != 0) {
        fprintf(stderr, "[WARN] engine_reset: recreate context failed\n");
    }
#endif

    // Canonical counters
    e->n_past = 0;
    e->pos = 0;
    e->kv_valid = false;
    e->kv_len = 0;

    // Reset sequence id if you want a fresh conversation id
    e->seq_id = 0;
}


// Recreate the llama context and reset engine counters
int engine_recreate_context(engine_t* e) {
    if (!e) return -1;

    // 1) Free existing context if present
    if (e->ctx) {
        // Use the correct free function for your llama build
        llama_free(e->ctx);
        e->ctx = NULL;
    }

    // 2) Create a fresh context. Adapt params to your llama.cpp version.
    // Older builds used: llama_new_context(model, params)
    // Newer builds may use different APIs; replace with your project's call.
    /*struct llama_context_params ctx_params = llama_context_default_params();
    e->ctx = llama_init_from_model(e->model, ctx_params);*/

    e->ctx = llama_init_from_model(e->model, e->ctx_params);


    if (!e->ctx) {
        fprintf(stderr, "[FATAL] engine_recreate_context: failed to create new context\n");
        return -1;
    }

    // 3) Reset canonical counters and flags
    e->n_past = 0;
    e->pos = 0;               // keep alias in sync
    e->kv_valid = false;
    e->kv_len = 0;

    // 4) Reset any persistent batch buffers you keep
    memset(&e->persistent_batch, 0, sizeof(e->persistent_batch));

    return 0;
}



// ------------------------------------------------------------
// Open / close
// ------------------------------------------------------------

engine_t* engine_open(const char* model_path) {

    engine_t* e = calloc(1, sizeof(engine_t));
    if (!e) return NULL;
    e->n_past = 0;

    engine_init_runtime(e);

    // 1. OPEN VMM BEFORE LOADING MODEL
    const char* vmm_build = getenv("LLAMA_VMM_BUILD");
    const char* vmm_use = getenv("LLAMA_VMM_USE");

    char vmm_path[MAX_PATH];
    snprintf(vmm_path, sizeof(vmm_path), "vmm.bin");
    int vmm_initm = 0;
    
    vmm_initm = vmm_init(vmm_path, vmm_build ? 1 : 0);

    // 2. NOW load model (this triggers load_all_data → vmm_write/read)
    struct llama_model_params mparams = llama_model_default_params();
    mparams.use_mmap = false;   // if field exists in your llama.h
    e->model = llama_load_model_from_file(model_path, mparams);
    if (!e->model) {
        free(e);
        return NULL;
    }

    // ⭐ Detect model family here
    g_model_family = detect_model_family(model_path, e->model);
    printf("[engine] detected model family: %d\n", (int)g_model_family);

    const struct llama_vocab* vocab = llama_model_get_vocab(e->model);
    
    // Phi‑3 / Qwen
    e->assistant_tok = tokenize_single(vocab, "<|assistant|>");

    // 3. Create context
    struct llama_context_params cparams = llama_context_default_params();  


    // DYNAMIC METADATA EXTRACTION
    // Read the native maximum training context limit embedded directly inside the GGUF file
    int32_t model_train_ctx = llama_model_n_ctx_train(e->model);

    if (model_train_ctx > 0) {
        // Successfully pulled from GGUF metadata. Assign it directly!
        cparams.n_ctx = model_train_ctx;
        printf("[SUCCESS] GGUF metadata found! Context length auto-populated to: %d tokens.\n", cparams.n_ctx);
    }
    else {
        // Fallback guard condition in case the metadata key is missing
        cparams.n_ctx = 4096;
        printf("[WARN] GGUF metadata context length unavailable. Falling back to default: 4096 tokens.\n");
    }

    // Keep sequence scaling optimized for your singular tracking stream
    cparams.n_seq_max = 1;

    // ---------------- PERFORMANCE TUNING ----------------
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    int physical_cores = sysinfo.dwNumberOfProcessors;
    if (physical_cores > 8) physical_cores = 8;

    cparams.n_threads = physical_cores;
    cparams.n_threads_batch = physical_cores * 2;
    cparams.flash_attn_type = true;
    cparams.type_k = GGML_TYPE_F16;
    cparams.type_v = GGML_TYPE_F16;

#ifdef LLAMA_CONTEXT_PARAMS_HAS_N_UBATCH
    cparams.n_ubatch = 2048;
#endif
    // ---------------- GPU OFFLOAD (optional) ----------------
    if (llama_supports_gpu_offload()) {
        mparams.n_gpu_layers = 999;          // offload as many layers as possible
        mparams.split_mode = LLAMA_SPLIT_MODE_LAYER;
        printf("[engine] GPU offload enabled\n");
    }
    else {
        mparams.n_gpu_layers = 0;
        printf("[engine] GPU offload not supported\n");
    }

    // --------------------------------------------------------

    // Store tuned params for recreate_context()----------------------------------------------------
    e->ctx_params = cparams;
	e->model_params = mparams;

    // Allocate memory matching the precise dynamic token ceiling
    e->ctx = llama_new_context_with_model(e->model, cparams);
    if (!e->ctx) {
        llama_free_model(e->model);
        free(e);
        return NULL;
    }

    if (engine_feed_system_prompt(e, "You are a helpful assistant.") != 0) {
        llama_free(e->ctx);
        llama_free_model(e->model);
        free(e);
        return NULL;
    }
    vmm_cleanup();
    return e;
}


void engine_close(engine_t* e) {
    if (!e) return;

    /*if (e->vmm_model) {
        vmm_model_close(e->vmm_model);
    }*/
    e->n_past = 0;

    //llama_batch_free(e->persistent_batch);

    // 👇 PASTE THE CLEANUP CODE HERE:
    if (e->candidates_buf != NULL) {
        free(e->candidates_buf);
        e->candidates_buf = NULL;
    }

    if (e->ctx) {
        llama_free(e->ctx);
        e->ctx = NULL;
    }

    if (e->model) {
        llama_free_model(e->model);
        e->model = NULL;
    }

    free(e);
}
