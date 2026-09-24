#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "llama.h"
#include "../vmm/vmm.h"

#ifdef __cplusplus
extern "C" {
#endif
#define MAX_TURNS 32
#define MAX_PATH 260


    // engine.h — canonical tool_call_t and helper (single definition)
#ifndef ENGINE_TOOL_CALL_T
#define ENGINE_TOOL_CALL_T

#include <stdlib.h> // ensure free() is declared

    typedef struct {
        char* name;        // tool name, e.g. "web_search"
        char* arguments;   // JSON string or raw argument text (malloc'd, caller frees)
    } tool_call_t;

    // Single helper only. Use this everywhere to free tool_call_t fields.
    static inline void tool_call_free(tool_call_t* c) {
        if (!c) return;
        if (c->name) { free(c->name); c->name = NULL; }
        if (c->arguments) { free(c->arguments); c->arguments = NULL; }
    }

#endif // ENGINE_TOOL_CALL_T



    extern bool was_chat_console;

    typedef struct {
        char role[8];   // "User" / "AI"
        char text[4096];
    } engine_turn_t;

    typedef struct {
        char role[8];   // "User" / "AI"
        char text[4096];
    } html_turn_t;

    struct llama_context_params ctx_params;


    typedef struct engine {
        struct llama_model* model;
        struct llama_context* ctx;
        //int64_t pos;
        //char conversation[16384];
        struct llama_context_params ctx_params;
		struct llama_model_params model_params;

        html_turn_t html_turns[128];
        int html_n_turns;

        int html_seq_id;   // always 0 for HTML chat

        engine_turn_t turns[MAX_TURNS];
        int n_turns;

        int32_t seq_id;   // single stable sequence
        llama_pos pos;    // current position in this sequence
        // --- Sampling parameters (needed for incremental generation) ---
        float temp;
        int   top_k;
        float top_p;

        vmm_model_t* vmm_model;
        int               cached_vocab_size;
        llama_token_data* candidates_buf;
        struct llama_sampler* sampler;
        // 🌟 ADD THIS: One big persistent batch buffer
        struct llama_batch persistent_batch;

        // ⭐ KV subsystem
        llama_token assistant_tok;
        llama_token inst_end_tok;
        llama_token sys_end_tok;
        int n_past;

        bool    kv_valid;      // is there a meaningful sequence in KV?
        int64_t kv_len;        // how many tokens are currently in KV?

    } engine_t;

    int engine_recreate_context(engine_t* e);

    // Inside engine.h
    void engine_reset(engine_t* e);

   int engine_chat(engine_t* e,
        const char* user_input,
        char* out,
        size_t out_size);

   int engine_chat_html(engine_t* e,
       const char* user_input,
       char* out,
       size_t out_size);

    /*void engine_kv_cache_clear(engine_t* e);*/

    void cmd_open(int argc, char** argv);
    void cmd_close(int argc, char** argv);
    void cmd_infer(int argc, char** argv);

    engine_t* engine_open(const char* path);
    void      engine_close(engine_t* e);

    int engine_generate(
        engine_t* e,
        const char* prompt,
        char* out,
        size_t out_size,
        int max_tokens,
        float temp,
        int top_k,
        float top_p,
        bool stream
    );

    static const char* TOOL_SYSTEM_PROMPT =
        "You are a helpful assistant with access to a websearch tool.\n"
        "CRITICAL: If the user's query requires external or real-time information, you MUST call the tool.\n\n"
        "To call the tool, you must output ONLY a valid JSON object. Do not include markdown code fences, explanations, or any text before or after the JSON.\n"
        "JSON Format:\n"
        "{\"tool\": \"websearch\", \"query\": \"<search terms>\"}\n\n"
        "Example of tool call:\n"
        "User: What is the weather in Tokyo right now?\n"
        "Assistant: {\"tool\": \"websearch\", \"query\": \"Tokyo weather right now\"}\n\n"
        "If you can answer fully using your internal knowledge, reply normally with plain text. Do not use JSON.";


       /* "You are a helpful assistant. If a query requires external info, you MUST call the websearch tool.\n"
        "To call it, output ONLY this raw JSON object, no code fences, no extra text:\n"
        "{\"tool\":\"websearch\",\"query\":\"<user query>\"}\n"
        "If you can answer using internal knowledge, reply normally without JSON.";*/


   /* static const char* TOOL_SYSTEM_PROMPT =
        "You are a helpful assistant.\n"
        "\n"
        "You have access to a tool named \"websearch\".\n"
        "\n"
        "When the user asks a question that requires external information, you MUST respond with a JSON object in the following exact format:\n"
        "\n"
        "{\"tool\":\"websearch\",\"query\":\"<the user question here>\"}\n"
        "\n"
        "Rules:\n"
        "- Output ONLY the JSON object.\n"
        "- Do NOT add explanations, commentary, or text before or after the JSON.\n"
        "- Do NOT wrap the JSON in code fences.\n"
        "- Do NOT change the field names \"tool\" or \"query\".\n"
        "- Do NOT add extra fields.\n"
        "- Do NOT answer the question yourself until AFTER the tool result is provided.\n"
        "- If you add anything outside the JSON object, the tool call will fail.\n"
        "\n"
        "After the tool result is provided, you may answer normally\n"
        "\n"
        "If the user asks something that you can answer using your internal knowledge, answer normally without calling the tool.\n";*/

   /* static const char* websearch_grammar =
        "root ::= object\n"
        "object ::= '{\"tool\":\"websearch\",\"query\":\"' query '\"}'\n"
        "query ::= char*\n"
        "char ::= [^\"\\\\] | escape\n"
        "escape ::= '\\\\' [\"\\\\/bfnrt]\n";*/
        // Paste this exactly as-is. The backslashes ensure that both C and GBNF 
        // 
    // read the internal double quotes perfectly.
    static const char* websearch_grammar =
        "root   ::= object\n"
        "object ::= \"{\\\"tool\\\":\\\"websearch\\\",\\\"query\\\":\\\"\" query \"\\\"}\"\n"
        "query  ::= [^\\\"]*\n";

    static const char* phi3_tool_prompt =
        "You are a helpful assistant. You have access to a websearch tool.\n"
        "If a query requires external info, you MUST call the tool by returning a JSON object.\n"
        "[CRITICAL RULES]\n"
        "1. For tool calls, output ONLY the raw JSON. No markdown, no code fences, no explanations.\n"
        "2. If you know the answer, reply normally in plain text.\n\n"
        "[JSON SCHEMA]\n"
        "{\"tool\": \"websearch\", \"query\": \"<optimized search keywords>\"}\n\n"
        "[EXAMPLE]\n"
        "User: Who won the latest Super Bowl?\n"
        "Assistant: {\"tool\": \"websearch\", \"query\": \"latest Super Bowl winner\"}";

    static const char* smollm_tool_prompt =
        "You are an AI with a websearch tool.\n"
        "If you need external info, reply ONLY with this JSON:\n"
        "{\"tool\": \"websearch\", \"query\": \"search keywords\"}\n"
        "Rules:\n"
        "- Never use markdown code fences.\n"
        "- Never add extra text or explanations.\n"
        "- If you can answer without the tool, reply with normal text.";


#ifdef __cplusplus
}
#endif
