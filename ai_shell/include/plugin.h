#ifndef PLUGIN_H
#define PLUGIN_H
#ifndef WINVER
#define WINVER 0x0602 /* Target Windows 8+ */
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602 /* Target Windows 8+ */
#endif
/* Manually define the macro if winhttp.h omitted it */
#ifndef WINHTTP_OPTION_UPGRADE_TO_WEBSOCKET
#define WINHTTP_OPTION_UPGRADE_TO_WEBSOCKET 114
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

typedef char* (*plugin_fn)(int argc, char** argv);

HINTERNET chromium_connect(void);
BOOL ws_send(HINTERNET ws, const char* msg);
char* ws_recv(HINTERNET ws);

void plugin_register(const char* name, plugin_fn fn);
plugin_fn plugin_lookup(const char* name);


extern const char* EXA_API_KEY;

void init_exa_key(void);

// Declare plugin functions
char* plugin_ddg(int argc, char** argv);
char* plugin_brave(int argc, char** argv);
char* plugin_chromium(int argc, char** argv);
char* plugin_summarize_file(int argc, char** argv);
char* plugin_websearch(int argc, char** argv);
char* plugin_summarize_file_html(int argc, char** argv);
char* plugin_exa_search(int argc, char** argv);
char* plugin_exa_fetch(int argc, char** argv);


#endif
