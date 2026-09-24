// plugin_brave.c — Hybrid JSON Brave Search (search + knowledge), 2026-ready
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/plugin.h"

#pragma comment(lib, "winhttp.lib")

// ------------------------------------------------------------
// URL encode
// ------------------------------------------------------------
static char* brave_url_encode(const char* s) {
    const char* hex = "0123456789ABCDEF";
    size_t len = strlen(s);
    char* out = malloc(len * 3 + 1);
    char* p = out;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = c;
        }
        else {
            *p++ = '%';
            *p++ = hex[c >> 4];
            *p++ = hex[c & 15];
        }
    }
    *p = 0;
    return out;
}

// ------------------------------------------------------------
// JSON escape
// ------------------------------------------------------------
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
// Generic HTTP GET (UTF-8) for a host + path
// ------------------------------------------------------------
static char* brave_http_get_utf8(const wchar_t* host, const wchar_t* path) {
    HINTERNET s = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        L"AppleWebKit/537.36 (KHTML, like Gecko) "
        L"Chrome/124.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!s) return NULL;

    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!c) {
        WinHttpCloseHandle(s);
        return NULL;
    }

    HINTERNET r = WinHttpOpenRequest(
        c, L"GET", path, NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!r) {
        WinHttpCloseHandle(c);
        WinHttpCloseHandle(s);
        return NULL;
    }

    LPCWSTR headers =
        L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        L"AppleWebKit/537.36 (KHTML, like Gecko) "
        L"Chrome/124.0 Safari/537.36\r\n"
        L"Accept: application/json,text/html;q=0.9,*/*;q=0.8\r\n"
        L"Accept-Language: en-US,en;q=0.9\r\n"
        L"Cache-Control: no-cache\r\n"
        L"Pragma: no-cache\r\n"
        L"Connection: keep-alive\r\n";

    WinHttpAddRequestHeaders(r, headers, -1, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL ok = WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok) {
        WinHttpCloseHandle(r);
        WinHttpCloseHandle(c);
        WinHttpCloseHandle(s);
        return NULL;
    }

    ok = WinHttpReceiveResponse(r, NULL);
    if (!ok) {
        WinHttpCloseHandle(r);
        WinHttpCloseHandle(c);
        WinHttpCloseHandle(s);
        return NULL;
    }

    DWORD size = 0, downloaded = 0;
    char* buf = malloc(1);
    size_t total = 0;

    do {
        if (!WinHttpQueryDataAvailable(r, &size)) break;
        if (size == 0) break;

        char* chunk = malloc(size + 1);
        if (!WinHttpReadData(r, chunk, size, &downloaded)) {
            free(chunk);
            break;
        }

        chunk[downloaded] = 0;

        buf = realloc(buf, total + downloaded + 1);
        memcpy(buf + total, chunk, downloaded);
        total += downloaded;
        buf[total] = 0;

        free(chunk);
    } while (size > 0);

    WinHttpCloseHandle(r);
    WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);

    return buf;
}

// ------------------------------------------------------------
// Brave JSON search API (public search endpoint style)
// Returns malloc'd JSON string on success, NULL on failure
// ------------------------------------------------------------
static char* brave_json_search(const char* query) {
    char* enc = brave_url_encode(query);

    wchar_t path[1024];
    swprintf(path, 1024, L"/search?q=%hs&source=web&summary=1", enc);
    free(enc);

    char* body = brave_http_get_utf8(L"search.brave.com", path);
    if (!body) return NULL;

    // 🔥 INSERT THIS CHECK RIGHT HERE
    if (strstr(body, "<html") || strstr(body, "<HTML")) {
        char* out = _strdup("{\"error\":\"brave_html_redirect\"}");
        free(body);
        return out;
    }

    // 1. Escape HTML into a dynamically allocated buffer
    size_t src_len = strlen(body);

    // Worst case: every char becomes 2 chars (\" or \\)
    size_t max_esc = src_len * 2 + 1;

    char* esc = malloc(max_esc);
    if (!esc) {
        free(body);
        return _strdup("{\"error\":\"oom\"}");
    }

    brave_json_escape(esc, max_esc, body);

    // 2. Allocate output JSON buffer
    size_t out_len = strlen(esc) + 128;
    char* out = malloc(out_len);
    if (!out) {
        free(body);
        free(esc);
        return _strdup("{\"error\":\"oom\"}");
    }

    // 3. Format JSON safely
    snprintf(out, out_len,
        "{"
        "\"type\":\"brave_raw\","
        "\"body\":\"%s\""
        "}",
        esc
    );

    // 4. Cleanup
    free(body);
    free(esc);

    return out;

}

// ------------------------------------------------------------
// MAIN PLUGIN FUNCTION (Brave Search)
// ------------------------------------------------------------
char* plugin_brave(int argc, char** argv) {
    if (argc < 1)
        return _strdup("{\"error\":\"no query\"}");

    // Build query
    char query[1024] = { 0 };
    for (int i = 0; i < argc; i++) {
        strcat(query, argv[i]);
        if (i + 1 < argc) strcat(query, " ");
    }

    // 1) Try Brave JSON search API
    char* json_res = brave_json_search(query);
    if (json_res)
        return json_res;

    // 2) Fallback: simple error JSON
    return _strdup("{\"error\":\"brave_http_failed\"}");
}
