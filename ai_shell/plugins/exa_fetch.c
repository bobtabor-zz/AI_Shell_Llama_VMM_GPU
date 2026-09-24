#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "../include/plugin.h"
#include "exa_fetch.h"

#pragma comment(lib, "winhttp.lib")

char* http_post_json_fetch(const wchar_t* host, const wchar_t* path, const char* json_body) {
    HINTERNET s = WinHttpOpen(
        L"Mozilla/5.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!s) return NULL;

    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!c) { WinHttpCloseHandle(s); return NULL; }

    HINTERNET r = WinHttpOpenRequest(
        c, L"POST", path, NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!r) {
        WinHttpCloseHandle(c);
        WinHttpCloseHandle(s);
        return NULL;
    }

    // Convert EXA_API_KEY to UTF16 safely
    wchar_t w_api_key[256];
    MultiByteToWideChar(CP_UTF8, 0, EXA_API_KEY, -1, w_api_key, 256);

    wchar_t header_api_key[512];
    swprintf(header_api_key, 512, L"x-api-key: %s", w_api_key);

    WinHttpAddRequestHeaders(r, L"Content-Type: application/json", -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(r, header_api_key, -1, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL ok = WinHttpSendRequest(
        r,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        (LPVOID)json_body,
        (DWORD)strlen(json_body),
        (DWORD)strlen(json_body),
        0);

    if (!ok) {
        WinHttpCloseHandle(r);
        WinHttpCloseHandle(c);
        WinHttpCloseHandle(s);
        return NULL;
    }

    WinHttpReceiveResponse(r, NULL);

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

        char* newbuf = realloc(buf, total + downloaded + 1);
        if (!newbuf) {
            free(chunk);
            free(buf);
            WinHttpCloseHandle(r);
            WinHttpCloseHandle(c);
            WinHttpCloseHandle(s);
            return NULL;
        }
        buf = newbuf;

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