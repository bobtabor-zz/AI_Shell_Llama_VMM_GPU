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
#include "memory.h"

#include <dxgi.h>
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

char* g_plugin_result = NULL;
static char g_transcript[60000];

// Global hybrid memory object
static memory_t g_memory;

typedef struct {
    int state;
    char buf[2048];
    size_t len;
} json_detector_t;


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


// Summarizes text using the main engine (Option A)
static char* llm_summarize_with_engine(engine_t* e, const char* text) {
    if (!e || !text) return NULL;

    // Reset engine for clean summarization
    engine_reset(e);

    // Feed system instruction
    engine_feed_system(e,
        "You are an assistant that produces extremely concise summaries of text. "
        "Summaries must preserve key facts and topics."
    );

    // Feed text to summarize
    engine_feed_user(e, text);

    // Allocate reply
    char* reply = malloc(65536);
    reply[0] = 0;

    engine_generate_reply(e, reply, 65536, 2048);

    return reply;
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

    g_transcript[0] = 0;
}

// ============================================================================
// MODEL FAMILY ENUM + GLOBAL
// ============================================================================
typedef enum {
    MODEL_LLAMA3,
    MODEL_LLAMA3_WEB,
    MODEL_HERMES2_WEB,
    MODEL_HERMES2_PRO,
    MODEL_PHI3,
    MODEL_SMOLLM,
    MODEL_MISTRAL,
    MODEL_QWEN,
    MODEL_GEMMA,
    MODEL_LLAMA2,
    MODEL_UNKNOWN
} model_family_t;

model_family_t g_model_family = MODEL_UNKNOWN;

static model_family_t detect_model_family(const char* path, struct llama_model* model) {
    // ---- GGUF metadata detection ----
    char arch_buf[128] = { 0 };

    int32_t result = llama_model_meta_val_str(model, "general.architecture",
        arch_buf, sizeof(arch_buf));
    char a[128];

    if (result > 0 && arch_buf[0] != '\0') {
        
        snprintf(a, sizeof(a), "%s", arch_buf);

        // Convert to lowercase for easier comparison
        for (char* p = a; *p; ++p) {
            *p = (char)tolower(*p);
        }

       // if (strcmp(a, "llama3") == 0) return MODEL_LLAMA3;
	   //if (strcmp(a, "llama") == 0) return MODEL_LLAMA3;  /// Llama‑3 is sometimes just "llama" in GGUF
       // if (strcmp(a, "phi3") == 0) return MODEL_PHI3;
       // if (strcmp(a, "smollm") == 0) return MODEL_SMOLLM;
       // if (strcmp(a, "mistral") == 0) return MODEL_MISTRAL;
        if (strcmp(a, "qwen2") == 0 || strcmp(a, "qwen") == 0 || strcmp(a, "qwen3") == 0)
            return MODEL_QWEN;
       // if (strcmp(a, "gemma") == 0) return MODEL_GEMMA;

        // Optional: add more architectures here
    }
        
      // ---- 2. Filename Fallback (Secondary check) ----
    if (strcmp(a, "llama") == 0) {

        char lower2[4096];
        snprintf(lower2, sizeof(lower2), "%s", path);
        for (char* p = lower2; *p; ++p) *p = (char)tolower(*p);

        /*if (strstr(lower2, "hermes-2-pro") && strstr(lower2, "web")) {
            return MODEL_HERMES2_WEB;
        }*/

        if (strstr(lower2, "hermes-2-pro-llama-3")) {
            return MODEL_HERMES2_PRO;
        }    

        // Llama‑3‑Web detection (must come BEFORE generic llama3)
        if (strstr(lower2, "llama-3-8b-web") ||
            strstr(lower2, "llama3-8b-web") ||
            strstr(lower2, "web")) {
            return MODEL_LLAMA3_WEB;
        }

        // Generic Llama‑3
        if (strstr(lower2, "llama-3") || strstr(lower2, "llama3"))
            return MODEL_LLAMA3;

        if (strstr(lower2, "phi-3") || strstr(lower2, "phi3"))
            return MODEL_PHI3;

        if (strstr(lower2, "smollm"))
            return MODEL_SMOLLM;

        if (strstr(lower2, "mistral"))
            return MODEL_MISTRAL;

        if (strstr(lower2, "qwen_qwen3") || strstr(lower2, "qwen") || strstr(lower2, "qwen3"))
            return MODEL_QWEN;

        if (strstr(lower2, "gemma"))
            return MODEL_GEMMA;

        if (strstr(lower2, "llama-2") || strstr(lower2, "llama2"))
            return MODEL_LLAMA2;

        return MODEL_UNKNOWN;

    }
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

// ---- Llama‑3 web if i can find it----
static void wrap_llama3_web_system(char* dst, size_t n, const char* sys) {
    snprintf(dst, n,
        "[MODE: JSON_BROWSER_AGENT]\n%s\n",
        sys
    ); 
}

static void wrap_llama3_web_user(char* dst, size_t n, const char* usr) {    
    snprintf(dst, n, "user(\"%s\")\n", usr);
}

// ---- hermes2_web ----

static void wrap_hermes2_web_system(char* dst, size_t n, const char* sys) {
    snprintf(dst, n, "%s", sys);
}


static void wrap_hermes2_web_user(char* dst, size_t n, const char* usr) {
    snprintf(dst, n, "%s", usr);
}


// ---- Hermes-2-Pro-Llama-3-8B ----
static void wrap_hermes2_pro_system(char* dst, size_t n, const char* sys) {
    snprintf(dst, n,
        "<|begin_of_text|><|start_header_id|>system<|end_header_id|>\n\n"
        "%s<|eot_id|>\n",
        sys);
}

static void wrap_hermes2_pro_user(char* dst, size_t n, const char* usr) {
    snprintf(dst, n,
        "<|start_header_id|>user<|end_header_id|>\n\n"
        "%s<|eot_id|>\n"
        "<|start_header_id|>assistant<|end_header_id|>\n\n",   // ready for generation
        usr);
}

// Optional: for adding previous assistant replies to history
static void wrap_hermes2_pro_assistant(char* dst, size_t n, const char* asst) {
    snprintf(dst, n,
        "%s<|eot_id|>\n",   // just close previous assistant turn
        asst);
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
    case MODEL_HERMES2_PRO: wrap_hermes2_pro_system(dst, n, sys); break;
    case MODEL_HERMES2_WEB: wrap_hermes2_web_system(dst, n, sys); break;
    case MODEL_LLAMA3_WEB: wrap_llama3_web_system(dst, n, sys); break;
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
    case MODEL_HERMES2_PRO: wrap_hermes2_pro_user(dst, n, usr); break;
    case MODEL_HERMES2_WEB: wrap_hermes2_web_user(dst, n, usr); break;
    case MODEL_LLAMA3_WEB: wrap_llama3_web_user(dst, n, usr); break;
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

const char* engine_default_system_prompt(void) {
    switch (g_model_family) {
    case MODEL_HERMES2_PRO: return HERMES2_PRO_SYSTEM_PROMPT;
    case MODEL_HERMES2_WEB: return HERMES2_WEB_SYSTEM_PROMPT;
    case MODEL_LLAMA3_WEB: return LLAMA3_WEB_SYSTEM_PROMPT;
    case MODEL_LLAMA3: return LLAMA3_SYSTEM_PROMPT;
    case MODEL_SMOLLM: return SMOLLM_SYSTEM_PROMPT;
    case MODEL_PHI3:   return PHI3_SYSTEM_PROMPT;
    case MODEL_MISTRAL:return MISTRAL_SYSTEM_PROMPT;
    case MODEL_QWEN:   return QWEN_SYSTEM_PROMPT;
    case MODEL_GEMMA:  return GEMMA_SYSTEM_PROMPT;
    case MODEL_LLAMA2: return LLAMA2_SYSTEM_PROMPT;
    default:           return LLAMA3_SYSTEM_PROMPT;
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

    fprintf(stderr, "\n=== SYSTEM WRAPPER OUTPUT in engine_feed_system_prompt===\n%s\n", buf);

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

int engine_feed_system(engine_t* e, const char* system_text_raw) {
    if (!e || !e->model || !e->ctx || !system_text_raw)
        return -1;

    const struct llama_vocab* vocab = llama_model_get_vocab(e->model);

    char buf[4096];
    engine_wrap_system(buf, sizeof(buf), system_text_raw);

    llama_token tokens[2048];
    int n_tokens = llama_tokenize(
        vocab,
        buf,
        (int32_t)strlen(buf),
        tokens,
        2048,
        false,
        true
    );
    if (n_tokens <= 0) return -1;

    struct llama_batch batch = { 0 };

    llama_pos      pos_arr[2048];
    int32_t        n_seq_arr[2048];
    llama_seq_id   seq_id_arr[2048];
    llama_seq_id* seq_ptr_arr[2048];
    int8_t         logits_arr[2048];

    batch.token = tokens;
    batch.pos = pos_arr;
    batch.n_seq_id = n_seq_arr;
    batch.seq_id = seq_ptr_arr;
    batch.logits = logits_arr;
    batch.n_tokens = 0;

    llama_pos pos = e->pos;

    for (int i = 0; i < n_tokens; i++) {
        pos_arr[i] = pos;
        n_seq_arr[i] = 1;
        seq_id_arr[i] = e->seq_id;
        seq_ptr_arr[i] = &seq_id_arr[i];
        logits_arr[i] = (i == n_tokens - 1);
        batch.n_tokens++;
        pos++;
    }

    int rc = llama_decode(e->ctx, batch);
    if (rc != 0) return rc;

    e->pos = pos;
    e->kv_valid = true;
    e->kv_len = pos;

    return 0;
}


// ------------------------------------------------------------
// Feed user message (templated) incrementally - STABLE NATIVE BATCH
// ------------------------------------------------------------

int engine_feed_user(engine_t* e, const char* user_text_raw) {
    if (!e || !e->model || !e->ctx || !user_text_raw)
        return -1;

    const struct llama_vocab* vocab = llama_model_get_vocab(e->model);

    char buf[4096];
    engine_wrap_user(buf, sizeof(buf), user_text_raw);


    // Fact extraction stub — expand later
    // Example: detect "my name is <name>"
    if (strstr(user_text_raw, "my name is ")) {
        const char* p = strstr(user_text_raw, "my name is ") + strlen("my name is ");
        char name[128];
        sscanf(p, "%127s", name);
        memory_set_fact(&g_memory, "user_name", name);
    }

    if (strstr(user_text_raw, "i live in ")) {
        const char* p = strstr(user_text_raw, "i live in ") + strlen("i live in ");
        char city[128];
        sscanf(p, "%127s", city);
        memory_set_fact(&g_memory, "location", city);
    }

    if (strstr(user_text_raw, "my favorite language is ")) {
        const char* p = strstr(user_text_raw, "my favorite language is ") + strlen("my favorite language is ");
        char lang[128];
        sscanf(p, "%127s", lang);
        memory_set_fact(&g_memory, "favorite_language", lang);
    }

    if (strstr(user_text_raw, "working on ")) {
        const char* p = strstr(user_text_raw, "working on ") + strlen("working on ");
        char project[128];
        sscanf(p, "%127s", project);
        memory_set_fact(&g_memory, "current_project", project);
    }


    llama_token tokens[2048];
    int n_tokens = llama_tokenize(
        vocab,
        buf,
        (int32_t)strlen(buf),
        tokens,
        2048,
        false,
        true
    );
    if (n_tokens <= 0) return -1;

    struct llama_batch batch = { 0 };

    llama_pos      pos_arr[2048];
    int32_t        n_seq_arr[2048];
    llama_seq_id   seq_id_arr[2048];
    llama_seq_id* seq_ptr_arr[2048];
    int8_t         logits_arr[2048];

    batch.token = tokens;
    batch.pos = pos_arr;
    batch.n_seq_id = n_seq_arr;
    batch.seq_id = seq_ptr_arr;
    batch.logits = logits_arr;
    batch.n_tokens = 0;

    llama_pos pos = e->pos;

    for (int i = 0; i < n_tokens; i++) {
        pos_arr[i] = pos;
        n_seq_arr[i] = 1;
        seq_id_arr[i] = e->seq_id;
        seq_ptr_arr[i] = &seq_id_arr[i];
        logits_arr[i] = (i == n_tokens - 1);
        batch.n_tokens++;
        pos++;
    }

    int rc = llama_decode(e->ctx, batch);
    if (rc != 0) return rc;

    e->pos = pos;
    e->kv_valid = true;
    e->kv_len = pos;


    // ========================
    // HYBRID MEMORY TRIGGER
    // ========================

    // If KV cache exceeds 75% → summarize old tokens

    if(e->kv_len > (e->ctx_params.n_ctx * 3 / 4)) {

        char context_dump[50000];
        size_t len = strlen(g_transcript);
        size_t slice_start = (len > 4000 ? len - 4000 : 0);
        snprintf(context_dump, sizeof(context_dump), "%s", g_transcript + slice_start);

        char* result = llm_summarize_with_engine(e, context_dump);

        if (result) {
            memory_append_summary(&g_memory, result);
            free(result);
        }

        engine_reset(e);

        char mem_block[200000];
        memory_export_system_prompt(&g_memory, mem_block, sizeof(mem_block));

        engine_feed_system(e, mem_block);

        return 0;
    }


    return 0;
}


char* extract_websearch_query(const char* json) {
    const char* key = "\"query\":\"";
    const char* start = strstr(json, key);
    if (!start) return NULL;

    start += strlen(key);

    const char* end = strchr(start, '"');
    if (!end) return NULL;

    size_t len = end - start;
    char* out = malloc(len + 1);
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}



char* extract_webfetch_url(const char* json) {
    const char* p = strstr(json, "\"url\":\"");
    if (!p) return NULL;
    p += strlen("\"url\":\"");
    const char* end = strchr(p, '"');
    if (!end) return NULL;
    size_t len = end - p;

    char* out = malloc(len + 1);
    strncpy(out, p, len);
    out[len] = 0;
    return out;
}

// Note: changed bool to int for standard C compatibility unless using <stdbool.h>

int repair_json(const char* input, char* output, size_t out_size) {
    size_t len = strlen(input);
    if (len + 4 >= out_size) return 0;

    memset(output, 0, out_size);
    memcpy(output, input, len);

    // Locate the start of the JSON block
    const char* prefix_marker = "\"tool\":\"websearch\",\"query\":\"";
    const char* match = strstr(input, prefix_marker);

    if (match != NULL) {
        size_t search_start = match - input + strlen(prefix_marker);

        // Scan forward to find how the string payload terminates
        for (size_t i = search_start; i < len; i++) {

            // Check for a literal double quote error (\"")
            if (i <= len - 3 && input[i] == '\\' && input[i + 1] == '"' && input[i + 2] == '"') {
                output[i + 2] = '}';
                return 1;
            }

            // FIX: Find the regular closing quote mark of the search phrase
            if (input[i] == '"' && input[i - 1] != '\\') {
                // If it isn't followed immediately by a closing brace, fix it!
                if (i + 1 >= len || output[i + 1] != '}') {
                    // Make space by shifting the rest of the string right by 1 byte
                    memmove(&output[i + 2], &output[i + 1], len - i);
                    output[i + 1] = '}';
                    return 1;
                }
            }
        }
    }

    return 0;
}

//llama3_web  stuffs
bool detect_web_action(const char* s) {
    return strstr(s, "{\"open\"") ||
        strstr(s, "{\"click\"") ||
        strstr(s, "{\"type\"") ||
        strstr(s, "{\"submit\"") ||
        strstr(s, "{\"extract\"");
}

bool detect_json_action(const char* s) {
    return strstr(s, "{\"open\"") ||
        strstr(s, "{\"click\"") ||
        strstr(s, "{\"type\"") ||
        strstr(s, "{\"submit\"") ||
        strstr(s, "{\"extract\"");
}


int engine_generate_reply(
    engine_t* e,
    char* out,
    size_t out_size,
    int max_tokens)
{
    
    if (!e || !e->model || !e->ctx || !out || out_size == 0)
        return -1;

    // ========================
    // SAFE HYBRID MEMORY TRIGGER (TOP OF generate_reply)
    // ========================
    if (e->kv_len > (e->ctx_params.n_ctx * 3 / 4)) {

        char context_dump[50000];
        size_t len = strlen(g_transcript);
        size_t slice_start = (len > 4000 ? len - 4000 : 0);
        snprintf(context_dump, sizeof(context_dump), "%s", g_transcript + slice_start);

        char* result = llm_summarize_with_engine(e, context_dump);

        if (result) {
            memory_append_summary(&g_memory, result);
            free(result);
        }

        engine_reset(e);

        char mem_block[200000];
        memory_export_system_prompt(&g_memory, mem_block, sizeof(mem_block));
        engine_feed_system(e, mem_block);
    }

    const struct llama_vocab* vocab = llama_model_get_vocab(e->model);

    out[0] = '\0';
    size_t out_len = 0;
    int n_gen = 0;

    llama_token eos_tok = llama_token_eos(vocab);
    llama_token eot_tok = llama_token_eot(vocab);

    struct llama_batch batch = llama_batch_init(1, 0, 1);

    bool qwen_thinking = false;

    bool json_started = false;
    bool json_complete = false;
    json_detector_t jd = { 0 };
    char json_block[4096] = { 0 };
    jd.state = 0;
    jd.len = 0;
    jd.buf[0] = '\0';
    char repaired[4096];

   /* const char* sys_prompt = engine_default_system_prompt();
    fprintf(stderr, "\n=== FULL PROMPT SENT TO MODEL ===\n%s\n", sys_prompt);*/  /// test output

    while (n_gen < max_tokens && out_len + 8 < out_size) {

        // 1. Sample next token
        llama_token tok = engine_sample_next(e);

        // 2. Convert token → text
        char piece[256];
        int n = llama_token_to_piece(vocab, tok, piece, sizeof(piece), 0, false);
        if (n < 0) break;
        if (n >= 0 && n < sizeof(piece)) piece[n] = '\0';
               
       // fprintf(stderr, "[tok %02d] %s\n", n_gen, piece); // test output

        // If Qwen, detect <think> start/end
        if (g_model_family == MODEL_QWEN) {

            // detect "<think>"
            if (strstr(piece, "<think>")) {
                qwen_thinking = true;
            }

            // detect "</think>"
            if (strstr(piece, "</think>")) {
                qwen_thinking = false;
            }

            /*if (qwen_thinking) {
                memcpy(out + out_len, piece, n);
                goto qwen_next_token;
            }*/

            // If still inside <think>, skip ALL JSON detection
            if (qwen_thinking) {
                // Still append output normally
                if (out_len + n < out_size - 1) {
                    memcpy(out + out_len, piece, n);
                    out_len += n;
                    out[out_len] = '\0';
                }

                // And skip tool logic
                goto qwen_next_token;
            }
        }


        if (!qwen_thinking) {
            if (g_model_family != MODEL_LLAMA3_WEB && g_model_family != MODEL_HERMES2_WEB)
            {
                // 3. JSON STATE MACHINE FEED (Only process if there are characters)
                if (n > 0) {
                    for (int i = 0; i < n; i++) {
                        char c = piece[i];
                        if (jd.len < sizeof(jd.buf) - 1) {
                            jd.buf[jd.len++] = c;
                            jd.buf[jd.len] = 0;
                        }
                        switch (jd.state) {
                        case 0:                          
                            if (strstr(jd.buf, "{\"tool\":"))
                            {
                                jd.state = 1;
                                break;
                            }
                        case 1:
                            // Detect ANY of the supported tools
                            if (strstr(jd.buf, "{\"tool\":\"websearch\",\"query\":\"") ||
                                strstr(jd.buf, "{\"tool\":\"exa_search\",\"query\":\"") ||
                                strstr(jd.buf, "{\"tool\":\"exa_fetch\",\"url\":\""))
                            {
                                jd.state = 2;
                            }
                            break;

                        case 2:
                            // Detect closing brace of JSON
                            if (c == '}' && jd.len > 2 && jd.buf[jd.len - 2] == '"') {
                                json_complete = true;
                            }
                            break;
                        }

                    }
                }

                // 4. Detect JSON start
                if (jd.state == 2 && !json_started)
                {
                    json_started = true;
                  //  out_len = 0;
                  //  out[0] = '\0';
                }

                if (jd.state == 2)
                {
                    bool fixed = repair_json(jd.buf, repaired, sizeof(repaired));
                    if (fixed) {
                        json_complete = true;
                        strncpy(json_block, repaired, sizeof(json_block) - 1);
                        json_block[sizeof(json_block) - 1] = 0;
                    }
                    else
                    {
                        if (json_complete)
                        {
                            strncpy(json_block, jd.buf, sizeof(json_block) - 1);
                            json_block[sizeof(json_block) - 1] = 0;
                        }
                    }
                }

                // 5. Capture JSON or normal text
                if (out_len + n < out_size - 1) {
                    memcpy(out + out_len, piece, n);
                    out_len += n;
                    out[out_len] = '\0';
                }

                // =====================================================================
                // RUN STEP 6 HERE FIRST: Check if JSON is complete BEFORE checking EOT!
                // =====================================================================
                if (json_complete) {

                    // ==========================
                    // 1. Check for EXA SEARCH
                    // ==========================
                    if (strstr(json_block, "\"tool\":\"exa_search\"")) {
                        char* query = extract_websearch_query(json_block);
                        if (query) {
                            char* argvv[1] = { query };
                            char* result = plugin_exa_search(1, argvv);
                            free(query);

                            /*if (result) {
                                snprintf(out, out_size, "%s\n\n[EXA SEARCH RESULT]\n%s",
                                    json_block, result);
                                free(result);
                            }*/
                            if (result) {
                                // Append the JSON block + result to output
                                size_t written = snprintf(out + out_len, out_size - out_len,
                                    "\n\n[EXA SEARCH RESULT]\n%s",
                                    result);

                                out_len += written;
                                if (out_len >= out_size) out_len = out_size - 1;

                                free(result);
                            }
                            // Cleanup
                            jd.state = 0;
                            jd.len = 0;
                            jd.buf[0] = 0;
                            json_started = false;
                            json_complete = false;
                            memset(json_block, 0, sizeof(json_block));

                            //llama_batch_free(batch);
                            //return (int)strlen(out);

                             /* DO NOT RETURN HERE — allow model to continue generating commentary */
                            continue;
                        }
                    }


                    // ==========================
                    // 2. Check for EXA FETCH
                    // ==========================
                    if (strstr(json_block, "\"tool\":\"exa_fetch\"")) {
                        char* url = extract_webfetch_url(json_block);
                        if (url) {
                            char* argvv[1] = { url };
                            char* result = plugin_exa_fetch(1, argvv);
                            free(url);

                            /*if (result) {
                                snprintf(out, out_size, "%s\n\n[EXA FETCH RESULT]\n%s",
                                    json_block, result);
                                free(result);
                            }*/
                            if (result) {
                                // Append the JSON block + result to output
                                size_t written = snprintf(out + out_len, out_size - out_len,
                                    "\n\n[EXA FETCH RESULT]\n%s",
                                    result);

                                out_len += written;
                                if (out_len >= out_size) out_len = out_size - 1;

                                free(result);
                            }

                            // Cleanup
                            jd.state = 0;
                            jd.len = 0;
                            jd.buf[0] = 0;
                            json_started = false;
                            json_complete = false;
                            memset(json_block, 0, sizeof(json_block));

                            //llama_batch_free(batch);
                            //return (int)strlen(out);

                             /* DO NOT RETURN HERE — allow model to continue generating commentary */
                            continue;
                        }
                    }


                    // ==========================
                    // 3. Default WEBSEARCH handler
                    // ==========================
                    if (strstr(json_block, "\"tool\":\"websearch\"")) {
                        char* query = extract_websearch_query(json_block);
                        if (query) {
                            char* argvv[1] = { query };
                            char* result = plugin_websearch(1, argvv);
                            free(query);

                           /* if (result) {
                                snprintf(out, out_size, "%s\n\n[WEBSEARCH RESULT]\n%s",
                                    json_block, result);
                                free(result);
                            }*/

                            if (result) {
                                // Append the JSON block + result to output
                                size_t written = snprintf(out + out_len, out_size - out_len,
                                    "\n\n[WEBSEARCH RESULT]\n%s",
                                    result);

                                out_len += written;
                                if (out_len >= out_size) out_len = out_size - 1;

                                free(result);
                            }

                            // Cleanup
                            jd.state = 0;
                            jd.len = 0;
                            jd.buf[0] = 0;
                            json_started = false;
                            json_complete = false;
                            memset(json_block, 0, sizeof(json_block));

                            /*llama_batch_free(batch);
                            return (int)strlen(out);*/

                            /* DO NOT RETURN HERE — allow model to continue generating commentary */
                            continue;
                        }
                    }
                }
            }
        }

        if (g_model_family == MODEL_LLAMA3_WEB)
        {
            // 1. Accumulate characters into JSON buffer
            for (int i = 0; i < n; i++) {
                char c = piece[i];
                if (jd.len < sizeof(jd.buf) - 1) {
                    jd.buf[jd.len++] = c;
                    jd.buf[jd.len] = 0;
                }
            }

            // 2. If JSON action is complete → return immediately
            if (detect_web_action(jd.buf)) {
                strncpy(out, jd.buf, out_size - 1);
                out[out_size - 1] = 0;

                // Reset JSON state
                jd.state = 0;
                jd.len = 0;
                jd.buf[0] = 0;
                json_started = false;
                json_complete = false;
                memset(json_block, 0, sizeof(json_block));

                llama_batch_free(batch);
                return (int)strlen(out);
            }

            // 3. DO NOT break on EOS/EOT/control tokens until JSON is complete
            //    WebLlama often emits trailing control tokens AFTER the JSON block.
            continue;
        }


        if (g_model_family == MODEL_HERMES2_WEB) {

            for (int i = 0; i < n; i++) {
                char c = piece[i];
                if (jd.len < sizeof(jd.buf) - 1) {
                    jd.buf[jd.len++] = c;
                    jd.buf[jd.len] = 0;
                }
            }

            if (detect_json_action(jd.buf)) {
                strncpy(out, jd.buf, out_size - 1);
                out[out_size - 1] = 0;

                jd.state = 0;
                jd.len = 0;
                jd.buf[0] = 0;

                llama_batch_free(batch);
                return (int)strlen(out);
            }

            if (tok == eos_tok || tok == eot_tok || llama_token_is_control(vocab, tok))
            {
                break;
            }
        }

        qwen_next_token:


        // =====================================================================
        // NOW IT IS SAFE TO BREAK: If no tool was called, handle control tokens
        // =====================================================================
        if (tok == eos_tok) break;
        if (tok == eot_tok && eot_tok != -1) break;

        // if (llama_token_is_control(vocab, tok)) break;
        // Qwen Thinking models use control tokens for <think>
        // Do NOT break for MODEL_QWEN
        if (g_model_family != MODEL_QWEN || !qwen_thinking) {
            if (llama_token_is_control(vocab, tok)) break;
        }


        // 8. Prepare batch for next decode (unchanged)        
        batch.n_tokens = 1;
        batch.token[0] = tok;
        batch.pos[0] = e->pos;
        batch.n_seq_id[0] = 1;
        batch.seq_id[0][0] = e->seq_id;
        batch.logits[0] = 1;

        int rc = llama_decode(e->ctx, batch);
        if (rc != 0) {
            llama_batch_free(batch);
            return rc;
        }

        e->pos++;
        e->kv_len = e->pos;
        n_gen++;
    }


    strncat(g_transcript, out, sizeof(g_transcript) - strlen(g_transcript) - 1);
    strncat(g_transcript, "\n", sizeof(g_transcript) - strlen(g_transcript) - 1);


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

    // Feed system prompt once per session / when KV is empty
    if (!e->kv_valid) {
        const char* sys = engine_default_system_prompt();
        int sys_rc = engine_feed_system(e, sys);
        if (sys_rc != 0) {
            engine_reset(e);
            return -1;
        }
    }


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

    int rc = engine_generate_reply(e, model_reply, out_size, 1024);
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

    // Topic update stub
    /*if (strstr(model_reply, "project") || strstr(model_reply, "code")) {
        memory_update_topic(&g_memory, "current_project", model_reply, 5);
    }*/

    const char* project =
        memory_get_fact(&g_memory, "current_project");

    if (project && project[0]) {

        char project_summary[512];

        _snprintf(
            project_summary,
            sizeof(project_summary),
            "%.500s",
            model_reply
        );

        memory_update_topic(
            &g_memory,
            project,
            project_summary,
            5
        );
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

    const char* sys_prompt = engine_default_system_prompt();
    //engine_feed_system_prompt(e, "You are a helpful assistant.");
   // engine_feed_system_prompt(e, sys_prompt);
    engine_feed_system(e, sys_prompt);


    // Reinject memory after reset
        char mem_block[200000];
    memory_export_system_prompt(&g_memory, mem_block, sizeof(mem_block));
    engine_feed_system(e, mem_block);


}

size_t detect_gpu_vram_mb(void)
{
    IDXGIFactory* factory = NULL;
    IDXGIAdapter* adapter = NULL;

    CreateDXGIFactory(&IID_IDXGIFactory, (void**)&factory);
    factory->lpVtbl->EnumAdapters(factory, 0, &adapter);

    DXGI_ADAPTER_DESC desc;
    adapter->lpVtbl->GetDesc(adapter, &desc);

    return (size_t)(desc.DedicatedVideoMemory / (1024 * 1024));
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

    // ---------------- GPU OFFLOAD (optional) ----------------

    size_t vram_mb = detect_gpu_vram_mb();
    printf("[engine] GPU VRAM = %zu MiB\n", vram_mb);;


    if (vram_mb < 4096)
    {
        mparams.n_gpu_layers = 0;
        printf("[engine] GPU too small, using CPU\n");
        printf("[engine] GPU offload not supported\n");
    }
    else 
    {
        mparams.n_gpu_layers = 999;
        mparams.split_mode = LLAMA_SPLIT_MODE_LAYER;
        printf("[engine] GPU offload enabled\n");
    }

   //if (llama_supports_gpu_offload()) {
   //    mparams.n_gpu_layers = 999;          // offload as many layers as possible
   //    mparams.split_mode = LLAMA_SPLIT_MODE_LAYER;
   //    printf("[engine] GPU offload enabled\n");
   //}
   //else {
   //    mparams.n_gpu_layers = 0;
   //    printf("[engine] GPU offload not supported\n");
   //}

   // --------------------------------------------------------

    e->model = llama_load_model_from_file(model_path, mparams);
    if (!e->model) {
        free(e);
        return NULL;
    }

    // ⭐ Detect model family here
    g_model_family = detect_model_family(model_path, e->model);
    printf("[engine] detected model family: %d\n", (int)g_model_family);

    const struct llama_vocab* vocab = llama_model_get_vocab(e->model);
    
    
    // 3. Create context
    struct llama_context_params cparams = llama_context_default_params();

    // DYNAMIC METADATA EXTRACTION
    // Read the native maximum training context limit embedded directly inside the GGUF file
    int32_t model_train_ctx = llama_model_n_ctx_train(e->model);

    //if (model_train_ctx > 0) {
    //    // Successfully pulled from GGUF metadata. Assign it directly!
    //    cparams.n_ctx = model_train_ctx;
    //    printf("[SUCCESS] GGUF metadata found! Context length auto-populated to: %d tokens.\n", cparams.n_ctx);
    //}
    //else {
    //    // Fallback guard condition in case the metadata key is missing
    //    cparams.n_ctx = 4096;
    //    printf("[WARN] GGUF metadata context length unavailable. Falling back to default: 4096 tokens.\n");
    //}



    // Thinking models declare massive max context (over 250k)
    // but llama.cpp cannot allocate KV that large in CPU RAM unless you have >48GB free
    if (model_train_ctx > 8192) {
        cparams.n_ctx = 8192;        // SAFE VALUE
    }
    else if (model_train_ctx > 0) {
        cparams.n_ctx = model_train_ctx;
    }
    else {
        cparams.n_ctx = 4096;        // fallback
    }

    printf("[engine] Using capped context length: %d tokens.\n", cparams.n_ctx);


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
    //if (llama_supports_gpu_offload()) {
    //    mparams.n_gpu_layers = 999;          // offload as many layers as possible
    //    mparams.split_mode = LLAMA_SPLIT_MODE_LAYER;
    //    printf("[engine] GPU offload enabled\n");
    //}
    //else {
    //    mparams.n_gpu_layers = 0;
    //    printf("[engine] GPU offload not supported\n");
    //}

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

    //const char* sys_prompt = engine_default_system_prompt();
    ////printf("[engine] system prompt =\n%s\n", sys_prompt);
    //
    //if (engine_feed_system_prompt(e, sys_prompt) != 0) {
    //    llama_free(e->ctx);
    //    llama_free_model(e->model);
    //    free(e);
    //    return NULL;
    //}

    //// Initialize hybrid memory system
    //memory_init(&g_memory);

    ////memory_load(&g_memory, "memory.json");
    //if (!memory_load(&g_memory, "memory.json")) {
    //    printf("[memory] memory.json NOT FOUND\n");
    //}
    //else {
    //    printf("[memory] memory.json loaded\n");
    //}

    //// Export memory block (currently empty) and inject into system prompt
    //char mem_block[200000];
    //memory_export_system_prompt(&g_memory, mem_block, sizeof(mem_block));

    //printf("%s\n", mem_block);

    //engine_feed_system(e, mem_block);

    // Initialize hybrid memory system
    memory_init(&g_memory);

    if (!memory_load(&g_memory, "memory.json")) {
        printf("[memory] memory.json NOT FOUND\n");
    }
    else {
        printf("[memory] memory.json loaded\n");
    }

    // Export memory
    char mem_block[200000];
    memory_export_system_prompt(&g_memory, mem_block, sizeof(mem_block));

    const char* sys_prompt = engine_default_system_prompt();

    // Build ONE system prompt
    char combined_prompt[220000];

    snprintf(
        combined_prompt,
        sizeof(combined_prompt),
        "%s\n\n"
        "=== PERSISTENT USER MEMORY ===\n"
        "%s\n"
        "=== END MEMORY ===\n"
        "The above memory contains facts about the user.\n"
        "Use those facts when answering.\n"
        "Do not claim you cannot remember if the facts appear above.\n",
        sys_prompt,
        mem_block
    );

    printf(
        "\n===== COMBINED SYSTEM PROMPT =====\n%s\n",
        combined_prompt
    );

    if (engine_feed_system_prompt(e, combined_prompt) != 0) {
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

   // memory_save(&g_memory, "memory.json");
    int fact_count = 0;

    for (int i = 0; i < MAX_FACTS; i++) {
        if (g_memory.facts[i].in_use)
            fact_count++;
    }

    printf("[memory] facts=%d\n", fact_count);

    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);

    printf("[memory] cwd = %s\n", cwd);

    bool rc = memory_save(&g_memory, "memory.json");

    printf("[memory] save rc = %d\n", rc);


    free(e);
}
