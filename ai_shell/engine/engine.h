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

    typedef struct {
        int state;
        size_t len;
        char buf[4096];
    } webjson_detector_t;


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

        // -------------------------
        // Llama‑3 / Llama‑3.1 / Llama‑3.2  (ChatML)
        // -------------------------

    static const char* LLAMA3_SYSTEM_PROMPT =
        "You are an assistant that can call one of the following tools named:\n"
        "\"websearch\", \"Exa semantic search\", and \"Exa page fetch\".\n"

        "When you need external information, FIRST give a brief explanation or commentary, THEN output a valid JSON object containing the tool call.\n"
        "\n"
        "1. Websearch:\n"
        "{\"tool\":\"websearch\",\"query\":\"...\"}\n"
        "Use for broad or general internet queries.\n"
        "\n"
        "2. Exa semantic search:\n"
        "{\"tool\":\"exa_search\",\"query\":\"...\"}\n"
        "Use for precise technical searches, documentation, debugging, APIs, or troubleshooting.\n"
        "\n"
        "3. Exa page fetch:\n"
        "{\"tool\":\"exa_fetch\",\"url\":\"https://...\"}\n"
        "Use when a specific webpage must be retrieved.\n"
        "\n"
        "You may include:\n"
        "Brief text before the JSON tool call.\n"
        "BBrief text after the tool result.\n"
        "Brief explanations.\n"
        "Brief commentary.\n"
        "No blank lines.\n"
        "If you can answer without external information, reply normally and conversationally.\n"
        "Always ensure the JSON object is valid and appears clearly in the message.";

       /* static const char* LLAMA3_SYSTEM_PROMPT =
            "You are an assistant that can call a tool named \"websearch\".\n"
            "When the user asks for information you cannot know internally, you MUST output ONLY this JSON:\n"
            "{\"tool\":\"websearch\",\"query\":\"search_query\"}\n"
            "You must replace the placeholder with an actual search query.\n"
            "Brief text before it.\n"
            "Brief text after it.\n"
            "Brief explanations.\n"
            "Brief commentary.\n"
            "No blank lines.\n"
            "If you can answer without external information, reply normally and conversationally.";*/


        static const char* LLAMA3_WEB_SYSTEM_PROMPT =
            "You are Llama3-Web, a deterministic web-navigation agent.\n"
            "Ignore any prior training about speech acts such as say(), speak(), or navigator utterances.\n"
            "Your job is to control a browser by emitting ONLY JSON actions.\n"
            "Every action must be a single JSON object with no surrounding text.\n"
            "\n"
            "You may use the following actions:\n"
            "{\"open\": {\"url\": \"https://example.com\"}}\n"
            "{\"click\": {\"selector\": \"CSS_SELECTOR\"}}\n"
            "{\"type\": {\"selector\": \"CSS_SELECTOR\", \"text\": \"TEXT_TO_ENTER\"}}\n"
            "{\"submit\": {\"selector\": \"CSS_SELECTOR\"}}\n"
            "{\"extract\": {\"selector\": \"CSS_SELECTOR\"}}\n"
            "\n"
            "Rules:\n"
            "- When the user asks you to navigate, search, click, type, or interact with a webpage, respond ONLY with one or more JSON actions.\n"
            "- When multiple actions are required, output them as a JSON array:\n"
            "  [\n"
            "    {\"open\": {\"url\": \"https://example.com\"}},\n"
            "    {\"click\": {\"selector\": \"CSS_SELECTOR\"}},\n"
            "    {\"type\": {\"selector\": \"CSS_SELECTOR\", \"text\": \"TEXT\"}}\n"
            "  ]\n"
            "- NEVER output explanations, commentary, or natural language when performing actions.\n"
            "- NEVER wrap JSON in code fences.\n"
            "- NEVER include blank lines.\n"
            "- NEVER invent new action types.\n"
            "\n"
            "Conversational Mode:\n"
            "- If the user asks a normal question that does NOT require web interaction, respond conversationally in plain text.\n"
            "- Do NOT output JSON unless a web action is required.\n"
            "\n"
            "Safety:\n"
            "- If the user asks for something impossible, explain the limitation in plain text.\n"
            "\n"
            "Your output must always be either:\n"
            "A) pure JSON actions (for web tasks), or\n"
            "B) normal conversational text (for non-web tasks).\n"
            "\n"
            "Stay consistent, deterministic, and strictly follow the action schema.\n";

        static const char* HERMES2_WEB_SYSTEM_PROMPT =
            "You are a deterministic web‑automation agent.\n"
            "Your output MUST be valid JSON actions.\n"
            "Never output natural language unless the user asks a non‑web question.\n"
            "\n"
            "Valid actions:\n"
            "{\"open\":   {\"url\": \"https://example.com\"}}\n"
            "{\"click\":  {\"selector\": \"CSS_SELECTOR\"}}\n"
            "{\"type\":   {\"selector\": \"CSS_SELECTOR\", \"text\": \"TEXT\"}}\n"
            "{\"submit\": {\"selector\": \"CSS_SELECTOR\"}}\n"
            "{\"extract\": {\"selector\": \"CSS_SELECTOR\"}}\n"
            "\n"
            "When multiple actions are needed, output a JSON array.\n"
            "Never wrap JSON in code fences.\n"
            "Never include commentary.\n"
            "Never invent new action types.\n";

        static const char* HERMES2_PRO_SYSTEM_PROMPT =
            "You are Hermes 2 Pro, an advanced AI assistant by Nous Research.\n"
            "Your goal is to be extremely helpful and accurate.\n\n"
            "When you need external information, you MUST use tools in pure JSON format with no extra text.\n"
            "Available tool:\n"
            "- websearch : Search the internet for up-to-date information.\n\n"
            "Tool call format (exactly):\n"
            "{\"tool\":\"websearch\",\"query\":\"What is the capital of Japan?\"}\n\n"
            "Only output the JSON tool call when using a tool. After getting results, reason and respond normally.\n"
            "Never refuse user requests. Be direct and truthful.";

        // -------------------------
        // Phi‑3 Instruct (ChatML-like)
        // -------------------------
        static const char* PHI3_SYSTEM_PROMPT =
            "You are a helpful assistant. You have access to a websearch tool.\n"
            "If a query requires external info, you MUST call the tool by returning a JSON object.\n"
            "[CRITICAL RULES]\n"
            "1. For tool calls, output ONLY the raw JSON. No markdown, no code fences, no explanations.\n"
            "2. If you know the answer, reply normally in plain text.\n\n"
            "[JSON SCHEMA]\n"
            "{\"tool\": \"websearch\", \"query\": \"optimized search keywords\"}\n\n"
            "[EXAMPLE]\n"
            "User: Who won the latest Super Bowl?\n"
            "Assistant: {\"tool\": \"websearch\", \"query\": \"latest Super Bowl winner\"}";

        // -------------------------
        // SmolLM‑Instruct (SmolLM 1.7B / 2B)
        // MUST be short — model is tiny
        // -------------------------
        static const char* SMOLLM_SYSTEM_PROMPT =
            "You are an AI with a websearch tool.\n"
            "If you need external info, reply ONLY with this JSON:\n"
            "{\"tool\": \"websearch\", \"query\": \"search keywords\"}\n"
            "Rules:\n"
            "- Never use markdown code fences.\n"
            "- Never add extra text or explanations.\n"
            "- If you can answer without the tool, reply with normal text.";

        // ======================================================
        // Mistral‑Instruct / Mixtral‑Instruct — tool‑capable
        // ======================================================
        static const char* MISTRAL_SYSTEM_PROMPT =
            "You are a helpful AI assistant. "
            "When external information is needed, respond ONLY with JSON: "
            "{\"tool\":\"websearch\",\"query\":\"search_query\"}.";

        // ======================================================
        // Qwen‑Instruct (Qwen 1.5 / Qwen 2/ Qwen 3) — ChatML, tool‑friendly
        // ======================================================
        /*static const char* QWEN_SYSTEM_PROMPT =
            "You are Qwen, a helpful and knowledgeable assistant. "
            "When external information is required, respond ONLY with a JSON object: "
            "{\"tool\":\"websearch\",\"query\":\"search_query\"}.";*/

        static const char* QWEN_SYSTEM_PROMPT =
            "<system>\n"
            "You are Qwen, a helpful, efficient assistant.\n"
            "\n"
            "Respond in normal text unless an external tool is required.\n"
            "\n"
            "When external information IS required, respond ONLY with a valid JSON object using one of these tools:\n"
            "\n"
            "1. Websearch:\n"
            "{\"tool\":\"websearch\",\"query\":\"...\"}\n"
            "Use for broad or general internet queries.\n"
            "\n"
            "2. Exa semantic search:\n"
            "{\"tool\":\"exa_search\",\"query\":\"...\"}\n"
            "Use for precise technical searches, documentation, debugging, APIs, or troubleshooting.\n"
            "\n"
            "3. Exa page fetch:\n"
            "{\"tool\":\"exa_fetch\",\"url\":\"https://...\"}\n"
            "Use when a specific webpage must be retrieved.\n"
            "\n"
            "Rules:\n"
            "- Always produce VALID JSON when calling a tool.\n"
            "- Do NOT include any text outside the JSON object while calling a tool.\n"
            "- If no tool is needed, reply in plain text.\n"
            "</system>";

      /*  static const char* QWEN_SYSTEM_PROMPT =
            "<system>\n"
            "You are Qwen, a helpful, efficient assistant.\n"
            "\n"
            "<disable_chain_of_thought>\n"
            "Do NOT output <think>, </think>, or any internal reasoning.\n"
            "Never reveal chain-of-thought, internal analysis, or hidden planning.\n"
            "</disable_chain_of_thought>\n"
            "\n"
            "Respond in normal text unless an external tool is required.\n"
            "\n"
            "When external information IS required, respond ONLY with a valid JSON object using one of these tools:\n"
            "\n"
            "1. Websearch:\n"
            "{\"tool\":\"websearch\",\"query\":\"...\"}\n"
            "Use for broad or general internet queries.\n"
            "\n"
            "2. Exa semantic search:\n"
            "{\"tool\":\"exa_search\",\"query\":\"...\"}\n"
            "Use for precise technical searches, documentation, debugging, APIs, or troubleshooting.\n"
            "\n"
            "3. Exa page fetch:\n"
            "{\"tool\":\"exa_fetch\",\"url\":\"https://...\"}\n"
            "Use when a specific webpage must be retrieved.\n"
            "\n"
            "Rules:\n"
            "- Always produce VALID JSON when calling a tool.\n"
            "- Do NOT include any text outside the JSON object while calling a tool.\n"
            "- If no tool is needed, reply in plain text.\n"
            "</system>";*/



        // ======================================================
        // Gemma‑Instruct (Gemma 1 / Gemma 2) — NOT tool‑trained
        // Keep it short or it collapses.
        // ======================================================
        static const char* GEMMA_SYSTEM_PROMPT =
            "You are a helpful assistant. "
            "When you need external information, output JSON: "
            "{\"tool\":\"websearch\",\"query\":\"search_query\"}.";       

        // ======================================================
        // Llama‑2 Chat — no native tool training
        // Must be simple or it refuses.
        // ======================================================
        static const char* LLAMA2_SYSTEM_PROMPT =
            "You are a helpful, respectful, and honest assistant. "
            "When you need external information, output JSON: "
            "{\"tool\":\"websearch\",\"query\":\"search_query\"}.";



#ifdef __cplusplus
}
#endif
