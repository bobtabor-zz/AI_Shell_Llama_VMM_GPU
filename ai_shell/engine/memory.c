
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "memory.h"

// ===============================
// Initialize memory
// ===============================
void memory_init(memory_t *m) {
    if (!m) return;
    memset(m->long_term_summary, 0, sizeof(m->long_term_summary));
    memset(m->facts, 0, sizeof(m->facts));
    memset(m->topics, 0, sizeof(m->topics));
}

// ===============================
// Fact Memory
// ===============================
bool memory_set_fact(memory_t *m, const char *key, const char *value) {
    if (!m || !key || !value) return false;

    // Check if exists
    for (int i = 0; i < MAX_FACTS; i++) {
        if (m->facts[i].in_use && strcmp(m->facts[i].key, key) == 0) {
            snprintf(m->facts[i].value, sizeof(m->facts[i].value), "%s", value);
            return true;
        }
    }

    // Create new
    for (int i = 0; i < MAX_FACTS; i++) {
        if (!m->facts[i].in_use) {
            m->facts[i].in_use = true;
            snprintf(m->facts[i].key, sizeof(m->facts[i].key), "%s", key);
            snprintf(m->facts[i].value, sizeof(m->facts[i].value), "%s", value);
            return true;
        }
    }
    return false;
}

const char *memory_get_fact(memory_t *m, const char *key) {
    if (!m || !key) return NULL;
    for (int i = 0; i < MAX_FACTS; i++) {
        if (m->facts[i].in_use && strcmp(m->facts[i].key, key) == 0) {
            return m->facts[i].value;
        }
    }
    return NULL;
}

bool memory_remove_fact(memory_t *m, const char *key) {
    if (!m || !key) return false;
    for (int i = 0; i < MAX_FACTS; i++) {
        if (m->facts[i].in_use && strcmp(m->facts[i].key, key) == 0) {
            m->facts[i].in_use = false;
            return true;
        }
    }
    return false;
}

// ===============================
// Topic Records
// ===============================
bool memory_add_topic(memory_t *m, const char *topic, const char *data, int importance) {
    if (importance < 0 || importance > 10)
        importance = 5;
    if (!m || !topic || !data) return false;
    for (int i = 0; i < MAX_TOPIC_RECORDS; i++) {
        if (!m->topics[i].in_use) {
            m->topics[i].in_use = true;
            snprintf(m->topics[i].topic, sizeof(m->topics[i].topic), "%s", topic);
            snprintf(m->topics[i].data, sizeof(m->topics[i].data), "%s", data);
            m->topics[i].importance = importance;
            return true;
        }
    }
    return false;
}

bool memory_update_topic(memory_t *m, const char *topic, const char *data, int importance) {
    if (!m || !topic || !data) return false;
    for (int i = 0; i < MAX_TOPIC_RECORDS; i++) {
        if (m->topics[i].in_use && strcmp(m->topics[i].topic, topic) == 0) {
            snprintf(m->topics[i].data, sizeof(m->topics[i].data), "%s", data);
            m->topics[i].importance = importance;
            return true;
        }
    }
    return memory_add_topic(m, topic, data, importance);
}

const char *memory_get_topic(memory_t *m, const char *topic) {
    if (!m || !topic) return NULL;
    for (int i = 0; i < MAX_TOPIC_RECORDS; i++) {
        if (m->topics[i].in_use && strcmp(m->topics[i].topic, topic) == 0) {
            return m->topics[i].data;
        }
    }
    return NULL;
}

// ===============================
// Long-term Summary
// ===============================
void memory_append_summary(memory_t *m, const char *text) {
    if (!m || !text) return;
    strncat(m->long_term_summary, text, sizeof(m->long_term_summary) - strlen(m->long_term_summary) - 1);
}

// ===============================
// Summarization Logic
// ===============================
// llm_summarize: user provides function pointer that calls model
void memory_summarize(memory_t *m, const char *context_dump, const char *(*llm_summarize)(const char *input)) {
    if (!m || !context_dump || !llm_summarize) return;

    const char *result = llm_summarize(context_dump);
    if (result) {
        memory_append_summary(m, result);
    }
}

// ===============================
// Export unified memory block
// ===============================
void memory_export_system_prompt(memory_t *m, char *out, size_t out_size) {
    if (!m || !out) return;

    snprintf(out, out_size,
        "Long-term Memory Summary:\n%s\n\nFacts:\n",
        m->long_term_summary);

    for (int i = 0; i < MAX_FACTS; i++) {
        if (m->facts[i].in_use) {
            snprintf(out + strlen(out), out_size - strlen(out), "- %s: %s\n", m->facts[i].key, m->facts[i].value);
        }
    }

    snprintf(out + strlen(out), out_size - strlen(out), "\nTopics:\n");
    for (int i = 0; i < MAX_TOPIC_RECORDS; i++) {
        if (m->topics[i].in_use) {
            snprintf(out + strlen(out), out_size - strlen(out),
                "Topic: %s (Importance %d)\n%s\n\n",
                m->topics[i].topic,
                m->topics[i].importance,
                m->topics[i].data);
        }
    }
}



// Save memory to disk as JSON-like text
bool memory_save(memory_t* m, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "{\n");

    // Save summary
    fprintf(f, "\"summary\": \"%s\",\n", m->long_term_summary);

    // Save facts
    fprintf(f, "\"facts\": [\n");
    for (int i = 0; i < MAX_FACTS; i++) {
        if (m->facts[i].in_use) {
            fprintf(f, "  {\"key\": \"%s\", \"value\": \"%s\"},\n",
                m->facts[i].key, m->facts[i].value);
        }
    }
    fprintf(f, "],\n");

    // Save topics
    fprintf(f, "\"topics\": [\n");
    for (int i = 0; i < MAX_TOPIC_RECORDS; i++) {
        if (m->topics[i].in_use) {
            fprintf(f,
                "  {\"topic\": \"%s\", \"data\": \"%s\", \"importance\": %d},\n",
                m->topics[i].topic,
                m->topics[i].data,
                m->topics[i].importance
            );
        }
    }
    fprintf(f, "]\n");

    fprintf(f, "}\n");

    fclose(f);
    return true;
}


// Load memory from disk
bool memory_load(memory_t* m, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;

    // crude simple loader (you can expand later)
    char line[8192];

    // Clear memory first
    memory_init(m);

    while (fgets(line, sizeof(line), f)) {
        // Load summary
        if (strstr(line, "\"summary\"")) {
            char* p = strchr(line, ':');
            if (p) {
                p += 2;
                char* end = strrchr(p, '"');
                if (end) *end = 0;
                strncpy(m->long_term_summary, p, sizeof(m->long_term_summary) - 1);
            }
        }

        // Load facts
        if (strstr(line, "\"key\"")) {
            char key[MAX_FACT_KEY];
            char value[MAX_FACT_VALUE];

            sscanf(line, " {\"key\": \"%[^\"]\", \"value\": \"%[^\"]\"}", key, value);
            memory_set_fact(m, key, value);
        }

        // Load topics
        if (strstr(line, "\"topic\"")) {
            char topic[128];
            char data[MAX_TOPIC_DATA];
            int importance = 0;

            sscanf(
                line,
                "  {\"topic\": \"%[^\"]\", \"data\": \"%[^\"]\", \"importance\": %d}",
                topic, data, &importance
            );
            memory_add_topic(m, topic, data, importance);
        }
    }

    fclose(f);
    return true;
}
