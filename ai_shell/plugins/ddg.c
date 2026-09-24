// plugin_ddg.c — Hybrid JSON + HTML DuckDuckGo search (weather + knowledge + search), 2026-ready
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
static char* url_encode(const char* s) {
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
// URL decode helpers
// ------------------------------------------------------------
static char hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

static void url_decode_inplace(char* s) {
    char* d = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            *d++ = (hex_val(s[1]) << 4) | hex_val(s[2]);
            s += 3;
        }
        else if (*s == '+') {
            *d++ = ' ';
            s++;
        }
        else {
            *d++ = *s++;
        }
    }
    *d = 0;
}

// ------------------------------------------------------------
// Unwrap DDG redirect URLs
// ------------------------------------------------------------
static void unwrap_ddg_redirect(char* url) {
    char* p = strstr(url, "uddg=");
    if (!p) return;

    p += 5;

    char* end = strchr(p, '&');
    if (end) *end = 0;

    memmove(url, p, strlen(p) + 1);
    url_decode_inplace(url);
}

// ------------------------------------------------------------
// Strip HTML tags
// ------------------------------------------------------------
static void strip_tags_inplace(char* s) {
    char* d = s;
    int in_tag = 0;

    while (*s) {
        if (*s == '<') in_tag = 1;
        else if (*s == '>') in_tag = 0;
        else if (!in_tag) *d++ = *s;
        s++;
    }
    *d = 0;
}

// ------------------------------------------------------------
// Escape JSON
// ------------------------------------------------------------
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

// ------------------------------------------------------------
// Extract substring between markers
// ------------------------------------------------------------
static char* extract_between(const char* start, const char* open, const char* close) {
    char* p = strstr((char*)start, open);
    if (!p) return NULL;
    p += strlen(open);

    char* q = strstr(p, close);
    if (!q) return NULL;

    size_t len = (size_t)(q - p);
    char* out = malloc(len + 1);
    memcpy(out, p, len);
    out[len] = 0;
    return out;
}

// ------------------------------------------------------------
// Generic HTTP GET (UTF-8) for a host + path
// ------------------------------------------------------------
static char* http_get_utf8(const wchar_t* host, const wchar_t* path) {
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
        L"Accept: text/html,application/json;q=0.9,*/*;q=0.8\r\n"
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
// Try JSON search API (duckduckgo-search–style)
// Returns malloc'd JSON string on success, NULL on failure
// ------------------------------------------------------------
static char* ddg_json_search(const char* query) {
    char* enc = url_encode(query);

    wchar_t path[1024];
    swprintf(path, 1024, L"/d.js?q=%hs&kl=us-en&l=us-en&dl=en&ct=US", enc);
    free(enc);

    char* body = http_get_utf8(L"duckduckgo.com", path);
    if (!body) return NULL;

    char* p = body;
    char* out = malloc(8192);
    size_t oi = 0;

    oi += snprintf(out + oi, 8192 - oi, "{\"type\":\"search\",\"results\":[");

    int count = 0;
    while ((p = strstr(p, "\"t\":\"")) && count < 5) {
        p += 5;
        char* t_end = strchr(p, '"');
        if (!t_end) break;
        size_t t_len = (size_t)(t_end - p);

        char* t = malloc(t_len + 1);
        memcpy(t, p, t_len);
        t[t_len] = 0;

        char* u = strstr(t_end, "\"u\":\"");
        if (!u) { free(t); break; }
        u += 5;
        char* u_end = strchr(u, '"');
        if (!u_end) { free(t); break; }
        size_t u_len = (size_t)(u_end - u);

        char* url = malloc(u_len + 1);
        memcpy(url, u, u_len);
        url[u_len] = 0;

        char* a = strstr(u_end, "\"a\":\"");
        char* snippet = NULL;
        if (a) {
            a += 5;
            char* a_end = strchr(a, '"');
            if (a_end) {
                size_t a_len = (size_t)(a_end - a);
                snippet = malloc(a_len + 1);
                memcpy(snippet, a, a_len);
                snippet[a_len] = 0;
            }
        }

        char esc_t[1024] = { 0 };
        char esc_u[2048] = { 0 };
        char esc_a[2048] = { 0 };

        json_escape(esc_t, sizeof(esc_t), t);
        json_escape(esc_u, sizeof(esc_u), url);
        json_escape(esc_a, sizeof(esc_a), snippet ? snippet : "");

        if (count > 0)
            oi += snprintf(out + oi, 8192 - oi, ",");

        oi += snprintf(out + oi, 8192 - oi,
            "{"
            "\"title\":\"%s\","
            "\"url\":\"%s\","
            "\"snippet\":\"%s\""
            "}",
            esc_t, esc_u, esc_a
        );

        free(t);
        free(url);
        if (snippet) free(snippet);

        count++;
        p = u_end;
    }

    oi += snprintf(out + oi, 8192 - oi, "]}");

    free(body);

    if (count == 0) {
        free(out);
        return NULL;
    }

    return out;
}

// ------------------------------------------------------------
// MAIN PLUGIN FUNCTION (Hybrid JSON + HTML + Weather + Knowledge)
// ------------------------------------------------------------
char* plugin_ddg(int argc, char** argv) {
    if (argc < 1)
        return _strdup("{\"error\":\"no query\"}");

    // Build query
    char query[1024] = { 0 };
    for (int i = 0; i < argc; i++) {
        strcat(query, argv[i]);
        if (i + 1 < argc) strcat(query, " ");
    }

    // 1) Try JSON search API first
    char* json_res = ddg_json_search(query);
    if (json_res)
        return json_res;

    // 2) Fallback to HTML (weather + knowledge + normal search)
    char* encoded = url_encode(query);

    wchar_t path[1024];
    swprintf(path, 1024, L"/html/?q=%hs&kl=us-en&kp=-2", encoded);
    free(encoded);

    char* buf = http_get_utf8(L"duckduckgo.com", path);
    if (!buf)
        return _strdup("{\"error\":\"http_failed\"}");

    // ------------------------------------------------------------
    // WEATHER BLOCK
    // ------------------------------------------------------------
    char* weather = strstr(buf, "module--weather");
    if (!weather)
        weather = strstr(buf, "js-weather-wrapper");

    if (weather) {
        char* temp = extract_between(weather, "weather__temp\">", "</");
        char* cond = extract_between(weather, "weather__condition\">", "</");
        char* loc = extract_between(weather, "weather__location\">", "</");

        char esc_temp[128] = { 0 };
        char esc_cond[256] = { 0 };
        char esc_loc[256] = { 0 };

        json_escape(esc_temp, sizeof(esc_temp), temp ? temp : "");
        json_escape(esc_cond, sizeof(esc_cond), cond ? cond : "");
        json_escape(esc_loc, sizeof(esc_loc), loc ? loc : "");

        if (temp) free(temp);
        if (cond) free(cond);
        if (loc)  free(loc);
        free(buf);

        char* out = malloc(1024);
        snprintf(out, 1024,
            "{"
            "\"type\":\"weather\","
            "\"location\":\"%s\","
            "\"temperature\":\"%s\","
            "\"condition\":\"%s\""
            "}",
            esc_loc,
            esc_temp,
            esc_cond
        );
        return out;
    }

    // ------------------------------------------------------------
    // KNOWLEDGE PANEL / ABOUT BOX
    // ------------------------------------------------------------
    char* about = strstr(buf, "module--about");
    if (!about)
        about = strstr(buf, "zci__body");
    if (!about)
        about = strstr(buf, "c-info__title");

    if (about) {
        char* title = extract_between(about, "about__title\">", "</");
        if (!title)
            title = extract_between(about, "c-info__title\">", "</");

        char* desc = extract_between(about, "about__description\">", "</");
        if (!desc)
            desc = extract_between(about, "c-info__content\">", "</");

        char esc_title[512] = { 0 };
        char esc_desc[2048] = { 0 };

        json_escape(esc_title, sizeof(esc_title), title ? title : "");
        json_escape(esc_desc, sizeof(esc_desc), desc ? desc : "");

        if (title) free(title);
        if (desc) free(desc);
        free(buf);

        char* out = malloc(4096);
        snprintf(out, 4096,
            "{"
            "\"type\":\"knowledge\","
            "\"title\":\"%s\","
            "\"description\":\"%s\""
            "}",
            esc_title,
            esc_desc
        );
        return out;
    }

    // ------------------------------------------------------------
    // NORMAL HTML SEARCH (PRIMARY SELECTOR)
    // ------------------------------------------------------------
    char* url_block = strstr(buf, "js-result-title-link");

    // SECONDARY SELECTORS (fallback)
    if (!url_block) {
        url_block = strstr(buf, "result__a");
        if (!url_block)
            url_block = strstr(buf, "result__title");
        if (!url_block)
            url_block = strstr(buf, "result__title-link");
    }

    if (!url_block) {
        free(buf);
        return _strdup("{\"error\":\"no_results\"}");
    }

    char* href = extract_between(url_block, "href=\"", "\"");
    char* title = extract_between(url_block, ">", "</a>");
    char* snippet = NULL;

    char* snip_block = strstr(url_block, "js-result-snippet");
    if (!snip_block)
        snip_block = strstr(url_block, "result__snippet");
    if (!snip_block)
        snip_block = strstr(url_block, "result__body");

    if (snip_block)
        snippet = extract_between(snip_block, ">", "</");

    if (href)    unwrap_ddg_redirect(href);
    if (title)   strip_tags_inplace(title);
    if (snippet) strip_tags_inplace(snippet);

    char esc_title[1024] = { 0 };
    char esc_url[2048] = { 0 };
    char esc_snip[2048] = { 0 };

    json_escape(esc_title, sizeof(esc_title), title ? title : "");
    json_escape(esc_url, sizeof(esc_url), href ? href : "");
    json_escape(esc_snip, sizeof(esc_snip), snippet ? snippet : "");

    free(buf);
    if (href)    free(href);
    if (title)   free(title);
    if (snippet) free(snippet);

    char* out = malloc(4096);
    snprintf(out, 4096,
        "{"
        "\"type\":\"search\","
        "\"title\":\"%s\","
        "\"url\":\"%s\","
        "\"snippet\":\"%s\""
        "}",
        esc_title,
        esc_url,
        esc_snip
    );

    return out;
}