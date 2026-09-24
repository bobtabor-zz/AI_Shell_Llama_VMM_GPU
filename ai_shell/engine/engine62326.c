// engine.c – incremental llama.cpp-style engine for Llama-3-Instruct
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

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

static void wrap_gemma_user(char* dst, size_t n, const char* usr) {
    snprintf(dst, n,
        "<start_of_turn>user\n%s<end_of_turn>\n"
        "<start_of_turn>assistant\n",
        usr
    );
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


// ------------------------------------------------------------
// Simple sampler (greedy; swap with your sampler if desired)
// ------------------------------------------------------------

static llama_token engine_sample_next(engine_t* e) {
    const struct llama_vocab* vocab = llama_model_get_vocab(e->model);
    const float* logits = llama_get_logits(e->ctx);
    int n_vocab = llama_vocab_n_tokens(vocab);

    int best_id = 0;
    float best_logit = logits[0];
    for (int i = 1; i < n_vocab; ++i) {
        if (logits[i] > best_logit) {
            best_logit = logits[i];
            best_id = i;
        }
    }
    return (llama_token)best_id;
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

    llama_pos pos = e->pos;

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
    e->pos = pos;
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

    llama_pos current_pos = e->pos;

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

static char* extract_websearch_query(const char* json) {
    const char* p = strstr(json, "\"query\":\"");
    if (!p) return NULL;

    p += strlen("\"query\":\"");
    const char* end = strchr(p, '"');
    if (!end) return NULL;

    size_t len = end - p;
    char* out = (char*)malloc(len + 1);
    memcpy(out, p, len);
    out[len] = 0;
    return out;
}


int engine_generate_reply(
    engine_t* e,
    char* out,
    size_t out_size,
    int max_tokens)
{
    if (!e || !e->model || !e->ctx || !out || out_size == 0)
        return -1;

    const struct llama_vocab* vocab = llama_model_get_vocab(e->model);

    out[0] = '\0';
    size_t out_len = 0;
    int n_gen = 0;

    llama_token eos_tok = llama_token_eos(vocab);
    llama_token eot_tok = llama_token_eot(vocab);

    // 1. Allocate ONE batch and reuse it for all tokens
    struct llama_batch batch = llama_batch_init(1, 0, 1);


    const char* TOOL_PREFIX = "{\"tool\":\"websearch\",\"query\":\"";

    char toolbuf[1024] = { 0 };
    size_t toolbuf_len = 0;
    bool tool_json_complete = false;


    while (n_gen < max_tokens && out_len + 8 < out_size) {
        // 2. Sample next token from current logits
        llama_token tok = engine_sample_next(e);

        // 3. Token → text
        char piece[256];
        int n = llama_token_to_piece(vocab, tok, piece, sizeof(piece), 0, false);
        if (n <= 0 || out_len + (size_t)n >= out_size)
            break;

        memcpy(out + out_len, piece, n);
        out_len += n;
        out[out_len] = '\0';

        // --- accumulate into tool buffer for JSON detection ---
        if (toolbuf_len + n < sizeof(toolbuf) - 1) {
            memcpy(toolbuf + toolbuf_len, piece, n);
            toolbuf_len += n;
            toolbuf[toolbuf_len] = '\0';
        }
        else {
            // simple sliding window if overflow
            size_t keep = sizeof(toolbuf) / 2;
            memmove(toolbuf, toolbuf + toolbuf_len - keep, keep);
            toolbuf_len = keep;
            if (toolbuf_len + n < sizeof(toolbuf) - 1) {
                memcpy(toolbuf + toolbuf_len, piece, n);
                toolbuf_len += n;
                toolbuf[toolbuf_len] = '\0';
            }
        }

        // --- detect complete websearch JSON ---
        char* start = strstr(toolbuf, "{\"tool\":\"websearch\",\"query\":\"");
        if (start) {
            char* end = strstr(start, "\"}");
            if (end) {
                // full JSON detected
                size_t json_len = (end + 2) - start;
                char json_block[512];
                if (json_len < sizeof(json_block)) {
                    memcpy(json_block, start, json_len);
                    json_block[json_len] = 0;

                    // extract query
                    char* query = extract_websearch_query(json_block);
                    if (query) {
                        char* argvv[1];
                        argvv[0] = query;

                        // --- CALL THE TOOL ---
                        char* result = plugin_websearch(1, argvv);

                        // free query string
                        free(query);

                        // you now have the tool result in `result`
                        // you can inject it into the model or return it
                        // simplest: append to out and break
                        if (result) {
                            strncat(out, "\n\n[WEBSEARCH RESULT]\n", out_size - strlen(out) - 1);
                            strncat(out, result, out_size - strlen(out) - 1);
                            free(result);
                        }

                        break; // stop generation
                    }
                }
            }
        }
        else /// hello or just text
        {
            // Hard stop: explicit EOT (if defined)
            if (tok == eot_tok && eot_tok != -1)
                break;

            // 1. Stop on EOS
            if (tok == eos_tok)
                break;

            // 2. Stop on control tokens (Llama‑3, Qwen, Gemma, etc.)
            if (llama_token_is_control(vocab, tok))
                break;

            //// 3. SmolLM / Llama‑2: stop only AFTER assistant has started

            // 4. Phi‑3 / Qwen: stop if model re‑emits assistant tag after start
            if (n_gen > 0 && e->assistant_tok != -1 && tok == e->assistant_tok)
                break;

            // 5. Phi‑3: stop after first newline once assistant has started
            if (tok == '\n' && n_gen > 0)
                break;

            // FIX: Only stop on text turn-enders AFTER the assistant has actually started typing words
            //if (n_gen > 0) {
            if (strstr(piece, "<|end|>") != NULL ||
                strstr(piece, "<|assistant|>") != NULL ||
                strstr(piece, "[/INST]") != NULL ||
                strstr(piece, "<</SYS>>") != NULL) {
                break;
            }
            //}

            // Stop on double newline AFTER assistant has started
            if (n_gen > 0 &&
                (strstr(piece, "\n\n") || strstr(out, "\n\n")))
            {
                break;
            }

        }

        // 4. Prepare batch for next decode
        batch.n_tokens = 1;
        batch.token[0] = tok;
        batch.pos[0] = e->pos;
        batch.n_seq_id[0] = 1;
        batch.seq_id[0][0] = e->seq_id;
        batch.logits[0] = 1;

        // 5. Decode (single call, no retry, no KV surgery)
        int rc = llama_decode(e->ctx, batch);
        if (rc != 0) {
            llama_batch_free(batch);
            return rc;
        }

        // 6. Advance engine state
        e->pos++;
        e->kv_len = e->pos;
        n_gen++;
    }

    llama_batch_free(batch);
    return (int)out_len;
}

// ------------------------------------------------------------
// HTML chat entry point - Complete Separated Plugin & UI Merge Pipeline
// ------------------------------------------------------------

int engine_chat_html(
    engine_t* e,
    const char* user_input,
    char* out,
    size_t out_size)
{
    if (!e || !user_input || !out || out_size == 0)
        return -1;

    out[0] = '\0';

    char* Plugin_result = NULL;
    char* temp_str = NULL;
    const char* extracted_query = user_input; // Default if no plugin matches
    bool plugin_executed = false;
    char* temp = NULL;

    // 1. Check for "ddg " plugin
    if (strncmp(user_input, "ddg ", 4) == 0) {
        extracted_query = user_input + 4;
        while (*extracted_query == ' ' || *extracted_query == '\t') extracted_query++; // strip leading spaces

        plugin_fn fn = plugin_lookup("ddg");
        if (fn) {
            // Pass the entire clean query as a single argument instead of splitting words!
            char* argvv[1];
            argvv[0] = (char*)extracted_query;
            Plugin_result = fn(1, argvv);
            plugin_executed = true;
        }
    }
  
    else if (strncmp(user_input, "websearch ", 10) == 0) {
        
        extracted_query = user_input + 10;
        temp = _strdup(extracted_query);
        char* token = strtok(temp, " ");
        char* argvv[32];
        int argcv = 0;

        while (token && argcv < 32) {
            argvv[argcv++] = token;
            token = strtok(NULL, " ");
        }

        plugin_fn fn = plugin_lookup("websearch");
        if (fn)
        {
            Plugin_result = fn(argcv, argvv);
            if (Plugin_result) {
                // printf("%s\n", Plugin_result);
                plugin_executed = true;
            }
                
        } 
               
    }
    // 3. Check for "summarize_file " plugin
    else if (strncmp(user_input, "summarize_file ", 15) == 0) {
        const char* raw = user_input + 15;
        while (*raw == ' ' || *raw == '\t') raw++;

        char clean_path[4096];
        engine_json_extract_string((char*)raw - 1, clean_path, sizeof(clean_path));

        char* argvv[1];
        argvv[0] = clean_path;

        Plugin_result = plugin_summarize_file_html(1, argvv);
        extracted_query = "Please synthesize and summarize this file contents.";
        plugin_executed = true;
    }

    // ------------------------------------------------------------
    // Dynamic Ingestion Layer (Background Data Context Feed)
    // ------------------------------------------------------------

    // If a plugin executed successfully, inject data into model KV cache first
    if (plugin_executed && Plugin_result) {
        // Wrapped in system header formatting so LLM treats it as an objective environment fact
        size_t system_block_size = strlen(Plugin_result) + 1024;
        char* system_payload = (char*)malloc(system_block_size);

        if (system_payload) {
            snprintf(system_payload, system_block_size,
                "<|start_header_id|>system<|end_header_id|>\n"
                "Search results background context:\n%s<|eot_id|>\n",
                Plugin_result
            );

            // Feed the background system context block into the model
            //engine_feed_user(e, system_payload);
            free(system_payload);
        }

        // Note: Do NOT free Plugin_result here yet! We keep it to print to the UI.
        if (temp_str) {
            free(temp_str);
            temp_str = NULL;
        }
    }
   
    // ------------------------------------------------------------
    // Feed Clean User Turn (No text mixed into the question)
    // ------------------------------------------------------------

    // This calls engine_feed_user with ONLY your isolated question (e.g. "how fast is a f-16?")
    int feed_rc = engine_feed_user(e, extracted_query);

    if (feed_rc != 0) {
        // 1. Clean up local plugin memory to prevent leaks
        if (plugin_executed && Plugin_result) {
            free(Plugin_result);
        }

        // 2. Flush engine tokens and clear the KV cache
        engine_reset(e);
        return -1;
    }

    // ------------------------------------------------------------
    // Inference Response & UI Compilation Layer
    // ------------------------------------------------------------


    // FIXED: Allocated a complete heap block instead of a broken single-byte variable
    char* model_reply = (char*)malloc(out_size);
    if (!model_reply) {
        if (plugin_executed && Plugin_result) free(Plugin_result);
        return -1;
    }
    model_reply[0] = '\0';

    int rc = engine_generate_reply(e, model_reply, out_size, 256);
    if (rc < 0) {
        free(model_reply);
        if (plugin_executed && Plugin_result) free(Plugin_result);
        return rc;
    }

    // Format output based on whether a plugin was run
    if (plugin_executed && Plugin_result) {
        snprintf(out, out_size,
            "=== Search Results Applied ===\n%s\n"
            "==============================\n\n"
            "AI Assistant:\n%s",
            Plugin_result, model_reply
        );

        free(Plugin_result);
    }
    else {
        strncpy(out, model_reply, out_size - 1);
        out[out_size - 1] = '\0';
    }

    free(model_reply);

    if (was_chat_console == true) // console flag
    {
        // print the generated text to console
        printf("%s", out);
        fflush(stdout);
        was_chat_console = false;
        return 0;
    }
    else
    {
        return (int)strlen(out);  //Web return
    }



   // return (int)strlen(out);
}
 


// ------------------------------------------------------------
// Reset conversation
// ------------------------------------------------------------

void engine_reset(engine_t* e) {
    if (!e || !e->ctx) return;

    struct llama_memory* mem = llama_get_memory(e->ctx);
    if (mem) {
        llama_memory_seq_rm(mem, e->seq_id, 0, INT32_MAX);
    }

    e->pos = 0;
    e->kv_valid = false;
    e->kv_len = 0;

    e->html_n_turns = 0;
    e->n_turns = 0;

    //engine_feed_system_prompt(e, "You are a helpful assistant.");
    engine_feed_system_prompt(e, TOOL_SYSTEM_PROMPT);

}



// ------------------------------------------------------------
// Open / close
// ------------------------------------------------------------

engine_t* engine_open(const char* model_path) {
    engine_t* e = calloc(1, sizeof(engine_t));
    if (!e) return NULL;

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

    /*if (engine_feed_system_prompt(e, "You are a helpful assistant.") != 0) {
        llama_free(e->ctx);
        llama_free_model(e->model);
        free(e);
        return NULL;
    }*/

    if (engine_feed_system_prompt(e, TOOL_SYSTEM_PROMPT) != 0) {
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
