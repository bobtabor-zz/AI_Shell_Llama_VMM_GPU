
#ifndef MEMORY_H
#define MEMORY_H

#include <stdbool.h>
#include <stdint.h>

// =============================
// Hybrid Memory System (Option C)
// =============================
// Features:
// 1. Long-term summary memory
// 2. Key-value fact memory
// 3. Topic-based memory records
// 4. Summarization interfaces
// 5. Integration hooks for engine

#define MAX_FACTS 256
#define MAX_TOPIC_RECORDS 128
#define MAX_TOPIC_DATA 4096
#define MAX_SUMMARY_SIZE 262144   // 256 KB summary
#define MAX_FACT_KEY 128
#define MAX_FACT_VALUE 512

// -----------------------------
// Fact memory (Key-Value store)
// -----------------------------
typedef struct {
    char key[MAX_FACT_KEY];
    char value[MAX_FACT_VALUE];
    bool in_use;
} fact_item_t;

// -----------------------------
// Topic memory records
// -----------------------------
typedef struct {
    char topic[128];
    char data[MAX_TOPIC_DATA];
    int importance; // 1-10
    bool in_use;
} topic_record_t;

// -----------------------------
// Main memory context
// -----------------------------
typedef struct {
    char long_term_summary[MAX_SUMMARY_SIZE];
    fact_item_t facts[MAX_FACTS];
    topic_record_t topics[MAX_TOPIC_RECORDS];
} memory_t;

// ====================
// API Function Prototypes
// ====================

void memory_init(memory_t *m);

// Fact memory management
bool memory_set_fact(memory_t *m, const char *key, const char *value);
const char *memory_get_fact(memory_t *m, const char *key);
bool memory_remove_fact(memory_t *m, const char *key);

// Topic records
bool memory_add_topic(memory_t *m, const char *topic, const char *data, int importance);
bool memory_update_topic(memory_t *m, const char *topic, const char *data, int importance);
const char *memory_get_topic(memory_t *m, const char *topic);

// Long-term summary block
void memory_append_summary(memory_t *m, const char *text);

// Summarization entry point (called when KV cache is near full)
// engine pointers are opaque here to avoid dependency
void memory_summarize(memory_t *m, const char *context_dump, const char *(*llm_summarize)(const char *input));

// Export unified memory block as a single system prompt
void memory_export_system_prompt(memory_t *m, char *out, size_t out_size);


bool memory_save(memory_t* m, const char* path);
bool memory_load(memory_t* m, const char* path);


#endif
