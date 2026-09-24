#include "../engine/engine.h"
#include "llama.h"
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/plugin.h"
#include "../server/http_server.h"

// -----------------------------------------------------------------------------
// Global engine instance
// -----------------------------------------------------------------------------

engine_t * g_engine = NULL;
bool was_chat_console = false;

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------

static void trim(char * s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = 0;
    }
}

// Parses a command line into argc/argv with quote support
int parse_args(char* line, char** argv, int max_args) {
    int argc = 0;
    char* p = line;

    while (*p && argc < max_args) {
        // skip whitespace
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (*p == '"') {
            // quoted argument
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = 0; // terminate arg
        }
        else {
            // normal argument
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = 0;
        }
    }

    return argc;
}

// -----------------------------------------------------------------------------
// Commands
// -----------------------------------------------------------------------------

void cmd_open(int argc, char** argv) {
    if (argc < 2) {
        printf("ERR missing model path\n");
        return;
    }

    const char* path = argv[argc - 1];  // ALWAYS last token

    if (g_engine) {
        engine_close(g_engine);
        g_engine = NULL;
    }

    g_engine = engine_open(path);

    if (!g_engine) {
        printf("ERR failed_to_open_model\n");
        return;
    }

    printf("OK model_loaded\n");
}

void cmd_close(int argc, char** argv) {
    if (g_engine) {
        engine_close(g_engine);
        g_engine = NULL;
    }
    printf("OK closed\n");
}


void cmd_infer(int argc, char** argv) {
    if (!g_engine) {
        printf("ERR no_model\n");
        return;
    }

    if (argc < 2) {
        printf("ERR missing prompt\n");
        return;
    }

    const char* prompt = argv[argc - 1];

    char out[8192];
   /////////////* int n = engine_generate(
   ////////////     g_engine,
   ////////////     prompt,
   ////////////     out,
   ////////////     sizeof(out),
   ////////////     128,
   ////////////     0.8f,
   ////////////     40,
   ////////////     0.9f,
   ////////////     true
   //////////// );*/

   ///////////////////////* if (n < 0) {
   //////////////////////     printf("ERR infer_failed\n");
   //////////////////////     return;
   ////////////////////// }

   ////////////////////// pri*/ntf("OUT %s\n", out);
}

static char chat_history[65536];

static void cmd_ping(int argc, char ** argv) {
    (void) argc;
    (void) argv;
    printf("PONG\n");
}

static void cmd_stats(int argc, char ** argv) {
    (void) argc;
    (void) argv;

    printf("OK\n");
    printf("ENGINE %s\n", g_engine ? "LOADED" : "NONE");
    printf("END\n");
}

// -----------------------------------------------------------------------------
// Command dispatcher
// -----------------------------------------------------------------------------



static void dispatch(char* line) {
    trim(line);
    if (line[0] == 0) {
        return;
    }

    char* argv[32];
    int argc = parse_args(line, argv, 32);

    if (argc == 0) {
        return;
    }

    // Normalize command to uppercase
    char cmd_buf[64];
    strncpy(cmd_buf, argv[0], sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    for (char* p = cmd_buf; *p; ++p) {
        *p = (char)toupper((unsigned char)*p);
    }

    const char* cmd = cmd_buf;

    if (strcmp(cmd, "OPEN") == 0) {
        cmd_open(argc, argv);

    }

    else if (strcmp(cmd, "CLOSE") == 0) {
        cmd_close(argc, argv);

    }

    else if (strcmp(cmd, "INFER") == 0) {
        cmd_infer(argc, argv);

    }

    else if (strncmp(line, "UNLOAD", 6) == 0) {
            if (g_engine) {
                engine_close(g_engine);
                g_engine = NULL;
                printf("MODEL UNLOADED\n");
                fflush(stdout);
            }
            else {
                printf("NO MODEL LOADED\n");
                fflush(stdout);
            }
            return;
    }


    else if (strcmp(cmd, "CHAT") == 0) {
        char reply[4096];

        was_chat_console = true;

        char user_msg[4096] = { 0 };
        for (int i = 1; i < argc; i++) {
            strcat(user_msg, argv[i]);
            if (i + 1 < argc) strcat(user_msg, " ");
        }

        if (!g_engine) {
            printf("ERR no_model_loaded\n");
            return;
        }

        if (engine_chat_html(g_engine, user_msg, reply, sizeof(reply)) == 0) {
            //printf("AI> %s\n", reply);
        }
        else {
            printf("ERR chat_failed\n");
        }
        printf("\n");
    }


    else if (strcmp(cmd, "PING") == 0) {
        cmd_ping(argc, argv);

    }
    else if (strcmp(cmd, "STATS") == 0) {
        cmd_stats(argc, argv);

    }
 
  else if (strcmp(cmd, "PLUGIN") == 0) {

      if (argc < 3) {
          printf("usage: plugin <name> <args...>\n");
          return;
      }

      const char* plugin_name = argv[1];

      plugin_fn fn = plugin_lookup(plugin_name);
      if (!fn) {
          printf("ERR unknown_plugin\n");
          return;
      }

      int p_argc = argc - 2;
      char** p_argv = &argv[2];

      char* result = fn(p_argc, p_argv);
      if (result)
          printf("%s\n", result);
    }

  else {
      printf("ERR unknown_command\n");
    }
}

DWORD WINAPI http_server_thread(LPVOID param) {
    int port = (int)(intptr_t)param;
    http_server_start(port);
    return 0;
}

// -----------------------------------------------------------------------------
// Main loop
// -----------------------------------------------------------------------------

int main(void) {
    char line[65536];

    printf("[main] Starting...\n");
    llama_backend_init();

    // ⭐ REGISTER PLUGINS HERE ⭐
    plugin_register("ddg", plugin_ddg);
    plugin_register("summarize_term", plugin_summarize_file);  // need to check
    plugin_register("websearch", plugin_websearch);
    plugin_register("exa_search", plugin_exa_search); 
    plugin_register("exa_fetch", plugin_exa_fetch);
    plugin_register("summarize_file", plugin_summarize_file_html);

    // ⭐ START HTTP SERVER IN BACKGROUND THREAD ⭐
    CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)http_server_start, (void*)8080, 0, NULL);

    /*CreateThread(
        NULL,
        0,
        http_server_thread,
        (LPVOID)(intptr_t)8080,
        0,
        NULL
    );*/


    printf("[http] server running on http://localhost:8080\n");

    printf("ai> ");
    fflush(stdout);

    while (fgets(line, sizeof(line), stdin)) {
        dispatch(line);

        printf("ai> ");
        fflush(stdout);
    }

    if (g_engine) {
        engine_close(g_engine);
    }

    return 0;
}

