// summarize_file_html.c — LLM-powered file summarizer using engine_generate correctly
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/plugin.h"
#include "../engine/engine.h" 

// Forward declarations matching your engine
extern engine_t* g_engine;

// ---------- file reading ----------
static char* read_file_html(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    buf[rd] = 0;
    *out_size = rd;
    return buf;
}

// ---------- JSON escape ----------
static void json_escape(char* dst, size_t dst_sz, const char* src) {
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 2 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\\' || c == '\"') {
            dst[di++] = '\\';
            dst[di++] = c;
        }
        else if (c == '\r' || c == '\n') {
            dst[di++] = ' ';
        }
        else {
            dst[di++] = c;
        }
    }
    dst[di] = 0;
}

////////---------- LLM helpers ----------///////
static char* call_llm_summary_html(const char* prompt) {
    if (!g_engine)
        return _strdup("no engine loaded");

    char out[2048] = { 0 };

    // You already override max_tokens/temp/top_k/top_p inside engine_generate,
    // so these values are mostly placeholders.
    ////////////////int rc = engine_generate_html(
    ////////////////    g_engine,
    ////////////////    prompt,
    ////////////////    out,
    ////////////////    sizeof(out),
    ////////////////    64,        // max_tokens (will be overridden by your hard-coded block)
    ////////////////    0.7f,
    ////////////////    20,
    ////////////////    0.9f,
    ////////////////    false   // plugins use silent mode for internal chunk summaries
    ////////////////);

//////////////////////   /* if (rc < 0)
//////////////////////        return _strdup("llm error");
//////////////////////
//////////////////////    return _strdup(out);
}


static char* summarize_chunk_html(const char* text) {
    // HARD LIMIT: only embed 600 chars of the chunk into the prompt
    const size_t MAX_EMBED = 600;
    char clipped[700];
    size_t len = strlen(text);
    if (len > MAX_EMBED) len = MAX_EMBED;
    memcpy(clipped, text, len);
    clipped[len] = 0;

    char prompt[1500];
    snprintf(prompt, sizeof(prompt),
        "Summarize the following text in 2–3 sentences, focusing on key ideas:\n\n"
        "%s\n\n"
        "Summary:",
        clipped);

    return call_llm_summary_html(prompt);
}

static char* merge_summaries_html(const char** parts, int count) {
    const int MAX_PARTS = 12;
    if (count > MAX_PARTS) count = MAX_PARTS;

    char prompt[2000];
    size_t pos = 0;

    pos += snprintf(prompt + pos, sizeof(prompt) - pos,
        "Combine the following partial summaries into one short, coherent summary:\n\n");

    for (int i = 0; i < count; i++) {
        const char* s = parts[i];
        size_t len = strlen(s);
        const size_t MAX_PART_LEN = 300;
        if (len > MAX_PART_LEN) len = MAX_PART_LEN;

        if (pos + len + 50 >= sizeof(prompt))
            break;

        pos += snprintf(prompt + pos, sizeof(prompt) - pos,
            "PART %d:\n%.*s\n\n", i + 1, (int)len, s);
    }

    pos += snprintf(prompt + pos, sizeof(prompt) - pos, "FINAL SUMMARY:");

    // ⭐ STREAMING FINAL SUMMARY
    char out[4096] = { 0 };

    ////////////////////////////engine_generate_html(
    ////////////////////////////    g_engine,
    ////////////////////////////    prompt,
    ////////////////////////////    out,
    ////////////////////////////    sizeof(out),
    ////////////////////////////    64,      // max tokens (your engine overrides this anyway)
    ////////////////////////////    0.7f,
    ////////////////////////////    20,
    ////////////////////////////    0.9f,
    ////////////////////////////    true     // ⭐ STREAM ENABLED
    ////////////////////////////);

    // Return the captured output buffer
    return _strdup(out);
}


// ---------- main plugin ----------
char* plugin_summarize_file_html(int argc, char** argv) {
    if (argc < 1)
        return _strdup("{\"error\":\"missing file path\"}");

    if (!g_engine)
        return _strdup("{\"error\":\"no_model_loaded\"}");

    const char* path = argv[0];
    size_t size = 0;

    char* contents = read_file_html(path, &size);    
    if (!contents)
        return _strdup("{\"error\":\"cannot_read_file\"}"); 

    const size_t CHUNK = 1024;
    int chunk_count = (int)((size + CHUNK - 1) / CHUNK);
    const int MAX_CHUNKS = 12;
    if (chunk_count > MAX_CHUNKS) chunk_count = MAX_CHUNKS;
    if (chunk_count <= 0) {
        free(contents);
        return _strdup("{\"error\":\"empty_file\"}");
    }

    char** partial_summaries = (char**)calloc(chunk_count, sizeof(char*));
    if (!partial_summaries) {
        free(contents);
        return _strdup("{\"error\":\"oom\"}");
    }

    for (int i = 0; i < chunk_count; i++) {
        size_t start = (size_t)i * CHUNK;
        if (start >= size) {
            partial_summaries[i] = _strdup("");
            continue;
        }

        size_t end = start + CHUNK;
        if (end > size) end = size;

        size_t len = end - start;
        char* chunk = (char*)malloc(len + 1);
        if (!chunk) {
            partial_summaries[i] = _strdup("");
            continue;
        }

        memcpy(chunk, contents + start, len);
        chunk[len] = 0;

        partial_summaries[i] = summarize_chunk_html(chunk);
        free(chunk);

        if (!partial_summaries[i])
            partial_summaries[i] = _strdup("");
    }

    free(contents);

    char* final_summary = NULL;
    if (chunk_count == 1) {
        final_summary = partial_summaries[0];
        partial_summaries[0] = NULL;
    }
    else {
        final_summary = merge_summaries_html((const char**)partial_summaries, chunk_count);
    }

    for (int i = 0; i < chunk_count; i++)
        if (partial_summaries[i]) free(partial_summaries[i]);
    free(partial_summaries);

    if (!final_summary)
        return _strdup("{\"error\":\"llm_failed\"}");

    char esc[32768];
    json_escape(esc, sizeof(esc), final_summary);
    free(final_summary);

    char* out = (char*)malloc(33000);
    snprintf(out, 33000, "{\"summary\":\"%s\"}", esc);
    return out;
}
