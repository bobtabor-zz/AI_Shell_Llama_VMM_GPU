#ifndef WINVER
#define WINVER 0x0602 /* Target Windows 8+ */
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602 /* Target Windows 8+ */
#endif

// plugin_chromium.c — Chromium DevTools search backend
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/plugin.h"
#include "cJSON.h"

/* Manually define the macro if winhttp.h omitted it */
#ifndef WINHTTP_OPTION_UPGRADE_TO_WEBSOCKET
#define WINHTTP_OPTION_UPGRADE_TO_WEBSOCKET 114
#endif

#pragma comment(lib, "winhttp.lib")

// ------------------------------------------------------------
// Send a WebSocket message
// ------------------------------------------------------------
static BOOL ws_send(HINTERNET ws, const char* msg) {
    return WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (BYTE*)msg, (DWORD)strlen(msg)) == ERROR_SUCCESS;
}

// ------------------------------------------------------------
// Receive complete WebSocket message into malloc'd buffer (Handles Fragments)
// ------------------------------------------------------------
static char* ws_recv(HINTERNET ws) {
    BYTE buf[16384]; /* Increased to 16KB per read frame slice to handle large HTML trees */
    DWORD len = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
    char* out = NULL;
    size_t total_allocated = 65536; /* Start allocation window at 64KB */
    size_t total_received = 0;

    out = (char*)malloc(total_allocated);
    if (!out) return NULL;
    out[0] = '\0';

    do {
        if (WinHttpWebSocketReceive(ws, buf, sizeof(buf), &len, &type) != ERROR_SUCCESS) {
            free(out);
            return NULL;
        }

        while (total_received + len >= total_allocated) {
            total_allocated *= 2;
            char* temp = (char*)realloc(out, total_allocated);
            if (!temp) {
                free(out);
                return NULL;
            }
            out = temp;
        }

        if (len > 0) {
            memcpy(out + total_received, buf, len);
            total_received += len;
        }
        out[total_received] = '\0';

    } while (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
        type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE);

    return out;
}

static char* http_read_all(HINTERNET hRequest) {
    DWORD size = 0, downloaded = 0;
    char* buffer = (char*)malloc(1);
    size_t total = 0;
    if (!buffer) return NULL;
    buffer[0] = '\0';

    do {
        if (!WinHttpQueryDataAvailable(hRequest, &size)) break;
        if (size == 0) break;
        char* chunk = (char*)malloc(size + 1);
        if (!chunk) break;
        if (!WinHttpReadData(hRequest, chunk, size, &downloaded)) {
            free(chunk);
            break;
        }
        chunk[downloaded] = 0;
        char* temp = (char*)realloc(buffer, total + downloaded + 1);
        if (!temp) {
            free(chunk);
            break;
        }
        buffer = temp;
        memcpy(buffer + total, chunk, downloaded);
        total += downloaded;
        buffer[total] = 0;
        free(chunk);
    } while (size > 0);
    return buffer;
}

// ------------------------------------------------------------
// Connect to Chromium DevTools
// ------------------------------------------------------------
HINTERNET chromium_connect(void) {
    HINTERNET hSession = NULL;
    HINTERNET hConnect = NULL;
    HINTERNET hRequest = NULL;
    HINTERNET hWSReq = NULL;
    HINTERNET hWebSocket = NULL;
    char* json = NULL;
    cJSON* root = NULL;
    cJSON* first = NULL;
    cJSON* wsurl = NULL;
    const char* full_ws_url = NULL;
    const char* path = NULL;
    wchar_t wpath[512];

    hSession = WinHttpOpen(L"ChromiumDevTools", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!hSession) return NULL;

    hConnect = WinHttpConnect(hSession, L"localhost", 9222, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return NULL;
    }

    hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/json", NULL, NULL, NULL, 0);
    if (!hRequest) goto cleanup_connect;

    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0)) goto cleanup_request;
    if (!WinHttpReceiveResponse(hRequest, NULL)) goto cleanup_request;

    json = http_read_all(hRequest);
    WinHttpCloseHandle(hRequest);
    hRequest = NULL;

    if (!json) goto cleanup_connect;

    root = cJSON_Parse(json);
    free(json);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        goto cleanup_connect;
    }

    first = cJSON_GetArrayItem(root, 0);
    if (!first) {
        cJSON_Delete(root);
        goto cleanup_connect;
    }

    wsurl = cJSON_GetObjectItem(first, "webSocketDebuggerUrl");
    if (!wsurl || !cJSON_IsString(wsurl)) {
        cJSON_Delete(root);
        goto cleanup_connect;
    }

    full_ws_url = wsurl->valuestring;
    path = strstr(full_ws_url, "/devtools/");
    if (!path) {
        cJSON_Delete(root);
        goto cleanup_connect;
    }

    swprintf(wpath, 512, L"%hs", path);
    cJSON_Delete(root);

    hWSReq = WinHttpOpenRequest(hConnect, L"GET", wpath, NULL, NULL, NULL, 0);
    if (!hWSReq) goto cleanup_connect;

    if (!WinHttpSetOption(hWSReq, WINHTTP_OPTION_UPGRADE_TO_WEBSOCKET, NULL, 0)) {
        goto cleanup_wsreq;
    }

    if (!WinHttpSendRequest(hWSReq, NULL, 0, NULL, 0, 0, 0)) goto cleanup_wsreq;
    if (!WinHttpReceiveResponse(hWSReq, NULL)) goto cleanup_wsreq;

    hWebSocket = WinHttpWebSocketCompleteUpgrade(hWSReq, NULL);
    WinHttpCloseHandle(hWSReq);

    if (!hWebSocket) goto cleanup_connect;

    return hWebSocket;

cleanup_wsreq:
    WinHttpCloseHandle(hWSReq);
cleanup_request:
    if (hRequest) WinHttpCloseHandle(hRequest);
cleanup_connect:
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return NULL;
}

static void brave_json_escape(char* dst, size_t dst_sz, const char* src) {
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

// ------------------------------------------------------------
// MAIN PLUGIN FUNCTION
// ------------------------------------------------------------
char* plugin_chromium(int argc, char** argv) {
    if (argc < 1) return _strdup("{\"error\":\"no query\"}");

    char query[2048] = { 0 };
    for (int i = 0; i < argc; i++) {
        strcat_s(query, sizeof(query), argv[i]);
        if (i + 1 < argc) strcat_s(query, sizeof(query), " ");
    }

    HINTERNET ws = chromium_connect();
    if (!ws) return _strdup("{\"error\":\"chromium_connect_failed\"}");

    /* Enable Page notifications */
    ws_send(ws, "{\"id\":1,\"method\":\"Page.enable\"}");
    free(ws_recv(ws));

    /* Target Google Search securely via navigation format layout */
    char nav[2048];
    snprintf(nav, sizeof(nav), "{\"id\":2,\"method\":\"Page.navigate\",\"params\":{\"url\":\"https://google.com\"}}", query);
    ws_send(ws, nav);

    /* Drain the navigation confirm message packet frame */
    char* nav_confirm = ws_recv(ws);
    if (nav_confirm) free(nav_confirm);

    /* Await result presence containers or safety timeout step */
    int retries = 0;
    int page_is_ready = 0;
    while (retries < 50) {
        ws_send(ws, "{\"id\":99,\"method\":\"Runtime.evaluate\",\"params\":{"
            "\"expression\":\"document.querySelector('#search, #main, body') !== null\","
            "\"returnByValue\":true}}");

        char* poll_resp = ws_recv(ws);
        if (poll_resp) {
            cJSON* poll_root = cJSON_Parse(poll_resp);
            if (poll_root) {
                cJSON* p_res = cJSON_GetObjectItem(poll_root, "result");
                cJSON* p_inn = p_res ? cJSON_GetObjectItem(p_res, "result") : NULL;
                cJSON* p_val = p_inn ? cJSON_GetObjectItem(p_inn, "value") : NULL;

                if (p_val && cJSON_IsTrue(p_val)) {
                    page_is_ready = 1;
                }
                cJSON_Delete(poll_root);
            }
            free(poll_resp);
        }

        if (page_is_ready) break;
        Sleep(100);
        retries++;
    }

    /* 4. Evaluate document out to isolated inner payload string buffer */
    ws_send(ws, "{\"id\":4,\"method\":\"Runtime.evaluate\","
        "\"params\":{\"expression\":\"document.documentElement.outerHTML\",\"returnByValue\":true}}");

    /* CRITICAL FIX: Loop until we receive the packet matching our command transaction (id: 4) */
    char* resp = NULL;
    while (1) {
        char* raw_msg = ws_recv(ws);
        if (!raw_msg) break;

        cJSON* check_root = cJSON_Parse(raw_msg);
        if (check_root) {
            cJSON* id_item = cJSON_GetObjectItem(check_root, "id");
            /* Check if this message is the direct response to our HTML fetch command */
            if (id_item && cJSON_IsNumber(id_item) && id_item->valueint == 4) {
                resp = raw_msg; /* Found it! */
                cJSON_Delete(check_root);
                break;
            }
            cJSON_Delete(check_root);
        }
        free(raw_msg); /* Discard background/lifecycle event packets */
    }

    WinHttpWebSocketClose(ws, 0, NULL, 0);

    if (!resp) return _strdup("{\"error\":\"chromium_no_response\"}");

    cJSON* root = cJSON_Parse(resp);
    free(resp);
    if (!root) return _strdup("{\"error\":\"chromium_bad_json\"}");

    cJSON* result = cJSON_GetObjectItem(root, "result");
    cJSON* inner = result ? cJSON_GetObjectItem(result, "result") : NULL;
    cJSON* value = inner ? cJSON_GetObjectItem(inner, "value") : NULL;

    if (!value || !cJSON_IsString(value)) {
        cJSON_Delete(root);
        return _strdup("{\"error\":\"chromium_no_html\"}");
    }

    const char* html = value->valuestring;
    size_t max = strlen(html) * 2 + 1;
    char* esc = (char*)malloc(max);
    if (!esc) {
        cJSON_Delete(root);
        return _strdup("{\"error\":\"out_of_memory\"}");
    }
    brave_json_escape(esc, max, html);

    size_t out_len = strlen(esc) + 128;
    char* out = (char*)malloc(out_len);
    if (out) {
        snprintf(out, out_len, "{\"type\":\"chromium_html\",\"html\":\"%s\"}", esc);
    }

    free(esc);
    cJSON_Delete(root);
    return out;
}
