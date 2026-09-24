#include <windows.h>
#include <winhttp.h>
#include <wchar.h>
#include "http_winhttp.h"

#pragma comment(lib, "winhttp.lib")

int http_post_json(const char* url,
    const char* api_key,
    const char* body,
    char** out_response)
{
    //printf("HTTP_WINHTTP_JSON_ACTIVE\n");
    *out_response = NULL;

    // Convert URL to wide
    wchar_t wurl[1024];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);

    URL_COMPONENTS uc = { 0 };
    uc.dwStructSize = sizeof(uc);

    wchar_t host[256], path[1024];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1024;

    if (!WinHttpCrackUrl(wurl, 0, 0, &uc))
        return -1;

    HINTERNET hSession = WinHttpOpen(L"ai_shell/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        NULL, NULL, 0);
    if (!hSession) return -2;

    HINTERNET hConnect = WinHttpConnect(hSession,
        uc.lpszHostName,
        uc.nPort,
        0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return -3;
    }

    DWORD flags = (uc.nPort == 443) ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
        L"POST",
        uc.lpszUrlPath,
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -4;
    }

    // Headers
    wchar_t headers[256];
    swprintf(headers, 256, L"Content-Type: application/json\r\n");

    DWORD body_len = (DWORD)strlen(body);
    printf("HTTP body_len=%lu\n", (unsigned long)body_len);

    // IMPORTANT: no body pointer here
    BOOL ok = WinHttpSendRequest(
        hRequest,
        headers, -1,
        WINHTTP_NO_REQUEST_DATA,
        0,
        body_len,
        0);
    if (!ok) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -5;
    }

    // Now send the body
    DWORD written = 0;
    ok = WinHttpWriteData(hRequest, body, body_len, &written);
    printf("HTTP wrote=%lu\n", (unsigned long)written);
    if (!ok || written != body_len) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -6;
    }

    ok = WinHttpReceiveResponse(hRequest, NULL);
    if (!ok) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -7;
    }

    // Read response
    DWORD size = 0, downloaded = 0;
    char* buffer = NULL;
    size_t total = 0;

    do {
        if (!WinHttpQueryDataAvailable(hRequest, &size)) break;
        if (size == 0) break;

        buffer = (char*)realloc(buffer, total + size + 1);
        if (!buffer) break;

        if (!WinHttpReadData(hRequest, buffer + total, size, &downloaded))
            break;

        total += downloaded;
    } while (size > 0);

    if (buffer) {
        buffer[total] = 0;
        *out_response = buffer;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return 0;
}

