#include <string.h>
#include <string.h>
#include "../include/plugin.h"

#define MAX_PLUGINS 64


const char* EXA_API_KEY = "9f2e59ea-3380-49df-9a69-111037639ecf";

void init_exa_key(void)
{
    const char* env = getenv("EXA_API_KEY");
    if (env)
        EXA_API_KEY = env;
}


typedef struct {
    const char* name;
    plugin_fn fn;
} plugin_entry_t;

static plugin_entry_t g_plugins[MAX_PLUGINS];
static int g_plugin_count = 0;

void plugin_register(const char* name, plugin_fn fn) {
    if (g_plugin_count >= MAX_PLUGINS)
        return;

    g_plugins[g_plugin_count].name = name;
    g_plugins[g_plugin_count].fn = fn;
    g_plugin_count++;
}

plugin_fn plugin_lookup(const char* name) {
    for (int i = 0; i < g_plugin_count; i++) {
        if (strcmp(g_plugins[i].name, name) == 0)
            return g_plugins[i].fn;
    }
    return NULL;
}
