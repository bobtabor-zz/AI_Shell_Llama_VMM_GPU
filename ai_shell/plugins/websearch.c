// websearch.c — unified Wikipedia + DuckDuckGo web search plugin (structured, image-aware)
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/plugin.h"
#include "cJSON.h"
#include <stdbool.h>
#include <ctype.h>
#include "exa_search.h"
#include "exa_fetch.h"

#pragma comment(lib, "winhttp.lib")

// Forward declaration of DDG plugin (from ddg.c)
extern char* plugin_ddg(int argc, char** argv);

static char* extract_google_urls(const char* html) {
    const char* p = html;
    size_t cap = 4096;
    char* out = malloc(cap);
    size_t o = 0;

    while ((p = strstr(p, "href=\"/url?q=")) != NULL) {
        p += strlen("href=\"/url?q=");

        const char* start = p;
        const char* end = strchr(p, '&');
        if (!end) break;

        size_t len = end - start;
        if (len < 5 || len > 2000) continue;

        if (o + len + 2 > cap) {
            cap *= 2;
            out = realloc(out, cap);
        }

        memcpy(out + o, start, len);
        o += len;
        out[o++] = '\n';
    }

    out[o] = '\0';
    return out;
}


// ------------------------------------------------------------
// Simple HTTP GET (UTF-8)
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
// Simple query cache
// ------------------------------------------------------------
#define CACHE_SIZE 16
typedef struct {
    char query[256];
    char* json;
} CacheEntry;

static CacheEntry g_cache[CACHE_SIZE];

static char* cache_get(const char* q) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (g_cache[i].json && _stricmp(g_cache[i].query, q) == 0)
            return _strdup(g_cache[i].json);
    }
    return NULL;
}

static void cache_put(const char* q, const char* json) {
    static int idx = 0;
    int i = idx++ % CACHE_SIZE;

    free(g_cache[i].json);
    strncpy(g_cache[i].query, q, 255);
    g_cache[i].query[255] = 0;
    g_cache[i].json = _strdup(json);
}

// ------------------------------------------------------------
// Wikipedia search → summary (raw JSON)
// ------------------------------------------------------------
static char* wikipedia_summary_raw(const char* query) {
    wchar_t encoded[2048];
    int ei = 0;

    for (int i = 0; query[i] && ei < 2040; i++) {
        if (query[i] == ' ') {
            encoded[ei++] = L'%'; encoded[ei++] = L'2'; encoded[ei++] = L'0';
        }
        else {
            encoded[ei++] = (wchar_t)query[i];
        }
    }
    encoded[ei] = 0;

    wchar_t search_path[1024];
    swprintf(search_path, 1024,
        L"/w/api.php?action=query&list=search&srsearch=%ls&format=json&utf8=1",
        encoded);

    char* search_json = http_get_utf8(L"en.wikipedia.org", search_path);
    if (!search_json) return NULL;

    cJSON* root = cJSON_Parse(search_json);
    free(search_json);
    if (!root) return NULL;

    cJSON* qobj = cJSON_GetObjectItem(root, "query");
    cJSON* arr = qobj ? cJSON_GetObjectItem(qobj, "search") : NULL;
    cJSON* first = (arr && cJSON_IsArray(arr)) ? cJSON_GetArrayItem(arr, 0) : NULL;
    cJSON* title = first ? cJSON_GetObjectItem(first, "title") : NULL;

    if (!cJSON_IsString(title)) {
        cJSON_Delete(root);
        return NULL;
    }

    wchar_t wtitle[1024];
    int wi = 0;
    for (; title->valuestring[wi] && wi < 1023; wi++)
        wtitle[wi] = (wchar_t)title->valuestring[wi];
    wtitle[wi] = 0;

    cJSON_Delete(root);

    wchar_t summary_path[1024];
    swprintf(summary_path, 1024,
        L"/api/rest_v1/page/summary/%ls", wtitle);

    return http_get_utf8(L"en.wikipedia.org", summary_path);
}

// ------------------------------------------------------------
// Wikipedia structured output
// ------------------------------------------------------------
static char* wikipedia_structured(const char* query) {
    char* raw = wikipedia_summary_raw(query);
    if (!raw) return NULL;

    cJSON* root = cJSON_Parse(raw);
    free(raw);
    if (!root) return NULL;

    const char* title = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
    const char* desc = cJSON_GetStringValue(cJSON_GetObjectItem(root, "description"));
    const char* extract = cJSON_GetStringValue(cJSON_GetObjectItem(root, "extract"));

    cJSON* urls = cJSON_GetObjectItem(root, "content_urls");
    cJSON* desk = urls ? cJSON_GetObjectItem(urls, "desktop") : NULL;
    const char* page_url = desk ? cJSON_GetStringValue(cJSON_GetObjectItem(desk, "page")) : "";

    cJSON* thumb = cJSON_GetObjectItem(root, "thumbnail");
    const char* thumb_src = thumb ? cJSON_GetStringValue(cJSON_GetObjectItem(thumb, "source")) : NULL;

    cJSON* orig = cJSON_GetObjectItem(root, "originalimage");
    const char* orig_src = orig ? cJSON_GetStringValue(cJSON_GetObjectItem(orig, "source")) : NULL;

    // Fallback logic
    const char* final_thumb = NULL;
    if (thumb_src && thumb_src[0])
        final_thumb = thumb_src;
    else if (orig_src && orig_src[0])
        final_thumb = orig_src;

    // Build structured JSON
    cJSON* out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "source", "wikipedia");
    cJSON_AddStringToObject(out, "title", title ? title : "");
    cJSON_AddStringToObject(out, "description", desc ? desc : "");
    cJSON_AddStringToObject(out, "summary", extract ? extract : "");
    cJSON_AddStringToObject(out, "page_url", page_url ? page_url : "");

    if (final_thumb)
        cJSON_AddStringToObject(out, "thumbnail", final_thumb);
    if (orig_src)
        cJSON_AddStringToObject(out, "original_image", orig_src);

    cJSON* images = cJSON_CreateArray();
    if (orig_src)
        cJSON_AddItemToArray(images, cJSON_CreateString(orig_src));
    else if (final_thumb)
        cJSON_AddItemToArray(images, cJSON_CreateString(final_thumb));
    cJSON_AddItemToObject(out, "images", images);

    char* out_str = cJSON_PrintUnformatted(out);

    // ALSO output plain text URLs for the UI to detect
    // Append plain URL at the end so UI can render it
    if (orig_src) {
        size_t len = strlen(out_str) + strlen(orig_src) + 4;
        char* merged = malloc(len);
        snprintf(merged, len, "%s\n%s", out_str, orig_src);
        free(out_str);
        out_str = merged;
    }
    else if (final_thumb) {
        size_t len = strlen(out_str) + strlen(final_thumb) + 4;
        char* merged = malloc(len);
        snprintf(merged, len, "%s\n%s", out_str, final_thumb);
        free(out_str);
        out_str = merged;
    }

    cJSON_Delete(out);
    cJSON_Delete(root);

    return out_str;
}

char* openverse_api_images(const char* query) {
    // FIX 1: Explicitly define broad array string buffers for wide char spaces
    wchar_t wQuery[512] = { 0 };
    MultiByteToWideChar(CP_UTF8, 0, query, -1, wQuery, 512);

    wchar_t wPath[1024] = { 0 };
    swprintf_s(wPath, 1024, L"/v1/images/?q=%s&page_size=3", wQuery);

    // Call your functional http_get_utf8 tool anonymously (no key!)
    char* json_data = http_get_utf8(L"api.openverse.org", wPath);
    if (!json_data) return NULL;

    cJSON* incoming = cJSON_Parse(json_data);
    free(json_data);
    if (!incoming) return NULL;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "source", "openverse");

    cJSON* images_array = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "images", images_array);

    // Openverse returns an array called "results" containing "url" strings
    cJSON* ov_results = cJSON_GetObjectItem(incoming, "results");
    if (ov_results && cJSON_IsArray(ov_results)) {
        cJSON* element = NULL;
        cJSON_ArrayForEach(element, ov_results) {
            cJSON* url_item = cJSON_GetObjectItem(element, "url");
            if (url_item && cJSON_IsString(url_item)) {
                // Add directly as clean raw cJSON strings
                cJSON_AddItemToArray(images_array, cJSON_CreateString(url_item->valuestring));
            }
        }
    }

    cJSON_Delete(incoming);
    char* final_output = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return final_output;
}


// ------------------------------------------------------------
// MAIN ENTRY - Combines Wikipedia and DuckDuckGo Results Successfully
// ------------------------------------------------------------
char* plugin_websearch(int argc, char** argv) {
    if (argc < 1) return _strdup("{\"error\":\"no query\"}");

    char query[512] = { 0 };
    for (int i = 0; i < argc; i++) {
        size_t current_len = strlen(query);
        size_t space_left = sizeof(query) - current_len - 1;
        if (space_left > 0) {
            strncat_s(query, sizeof(query), argv[i], space_left);
        }
        current_len = strlen(query);
        space_left = sizeof(query) - current_len - 1;
        if (i + 1 < argc && space_left > 0) {
            strncat_s(query, sizeof(query), " ", space_left);
        }
    }

    // Cache lookup checkpoint
    char* cached = cache_get(query);
    if (cached) return cached;

    // ==================== IMAGE REQUEST DETECTION ====================
    bool is_image_request = false;
    char* lower_query = _strdup(query);
    if (lower_query != NULL) {
        for (char* p = lower_query; *p; p++) *p = tolower((unsigned char)*p);
        const char* visual_tokens[] = { "image", "picture", "photo", "pic", "graphic", "illustration", "diagram", "drawing", "sketch", "screenshot", "wallpaper", "show me", "look at", "view", "display", "render", "search for", "draw", ".jpg", ".png", ".gif", ".jpeg" };
        size_t num_tokens = sizeof(visual_tokens) / sizeof(visual_tokens[0]);
        for (size_t i = 0; i < num_tokens; i++) {
            if (strstr(lower_query, visual_tokens[i]) != NULL) {
                is_image_request = true;
                break;
            }
        }
        free(lower_query);
    }

    // Create a unified master JSON root object to hold all combined metrics
    cJSON* master_root = cJSON_CreateObject();
    cJSON_AddStringToObject(master_root, "query", query);
    cJSON_AddBoolToObject(master_root, "image_mode", is_image_request);
    if (is_image_request) {
        cJSON_AddStringToObject(master_root, "message", "Here are the combined search images I found:");
    }

    cJSON* master_results = cJSON_CreateArray();
    cJSON_AddItemToObject(master_root, "results", master_results);

    cJSON* master_images = cJSON_CreateArray();
    cJSON_AddItemToObject(master_root, "images", master_images);

    bool collected_any_data = false;

    // ==================== 1. WIKIPEDIA SECTOR ====================
    char* wiki = wikipedia_structured(query);
    if (wiki) {
        cJSON* wiki_json = cJSON_Parse(wiki);
        if (wiki_json) {
            collected_any_data = true;
            // Extract textual details
            cJSON* w_results = cJSON_GetObjectItem(wiki_json, "results");
            if (w_results && cJSON_IsArray(w_results)) {
                cJSON* item = NULL;
                cJSON_ArrayForEach(item, w_results) {
                    cJSON_AddItemToArray(master_results, cJSON_Duplicate(item, true));
                }
            }
            else {
                // Fallback: If wiki just returns a flat summary string, package it up
                cJSON* summary = cJSON_GetObjectItem(wiki_json, "summary");
                if (summary && cJSON_IsString(summary)) {
                    cJSON* item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "source", "wikipedia");
                    cJSON_AddStringToObject(item, "title", summary->valuestring);
                    cJSON_AddItemToArray(master_results, item);
                }
            }
            // Extract Wikipedia image details
            cJSON* w_images = cJSON_GetObjectItem(wiki_json, "images");
            if (w_images && cJSON_IsArray(w_images)) {
                cJSON* img = NULL;
                cJSON_ArrayForEach(img, w_images) {
                    cJSON_AddItemToArray(master_images, cJSON_Duplicate(img, true));
                }
            }
            cJSON_Delete(wiki_json);
        }
        
        //char wiki_text[2048];   // writable buffer
        //snprintf(wiki_text, sizeof(wiki_text), "%s", wiki);

        free(wiki);
    }

    // ==================== 2. OPENVERSE IMAGE SECTOR ====================
     
    if (is_image_request) {
        char* ov_data = openverse_api_images(query);
        if (ov_data) {
            cJSON* ov_json = cJSON_Parse(ov_data);
            if (ov_json) {
                collected_any_data = true;
                cJSON* ov_images = cJSON_GetObjectItem(ov_json, "images");
                if (ov_images && cJSON_IsArray(ov_images)) {
                    cJSON* img = NULL;
                    cJSON_ArrayForEach(img, ov_images) {

                        // Check if it's a string (since your helper creates flat strings)
                        if (cJSON_IsString(img) && strlen(img->valuestring) > 0) {

                            // 1. Append directly to the 'images' array for your UI layer
                            cJSON_AddItemToArray(master_images, cJSON_CreateString(img->valuestring));

                            // 2. ⭐ FIX: Append to 'master_results' so your local GGUF can see it!
                            cJSON* result_item = cJSON_CreateObject();
                            cJSON_AddStringToObject(result_item, "source", "Openverse Media");

                            // Combine a tag label and the image url into the title field
                            char tag_title[2048] = { 0 };
                            snprintf(tag_title, sizeof(tag_title), "[Openverse Image Asset] Found Picture Link: %s", img->valuestring);

                            cJSON_AddStringToObject(result_item, "title", tag_title);
                            cJSON_AddStringToObject(result_item, "url", img->valuestring);

                            cJSON_AddItemToArray(master_results, result_item);
                        }
                        // Fallback check: If your helper returned raw objects instead of unpacked strings
                        else if (cJSON_IsObject(img)) {
                            cJSON* url_prop = cJSON_GetObjectItem(img, "url");
                            if (url_prop && cJSON_IsString(url_prop) && strlen(url_prop->valuestring) > 0) {

                                cJSON_AddItemToArray(master_images, cJSON_CreateString(url_prop->valuestring));

                                cJSON* result_item = cJSON_CreateObject();
                                cJSON_AddStringToObject(result_item, "source", "Openverse Media");

                                char tag_title[2048] = { 0 };
                                snprintf(tag_title, sizeof(tag_title), "[Openverse Image Asset] Found Picture Link: %s", url_prop->valuestring);

                                cJSON_AddStringToObject(result_item, "title", tag_title);
                                cJSON_AddStringToObject(result_item, "url", url_prop->valuestring);

                                cJSON_AddItemToArray(master_results, result_item);
                            }
                        }
                    }
                }
                cJSON_Delete(ov_json);
            }
            free(ov_data);
        }
    }

    // ==================== 3. DUCKDUCKGO SECTOR ====================
    char* mock_argv;
    mock_argv = query;

    //char* ddg = plugin_ddg(1, mock_argv);
    char* ddg = plugin_ddg(argc, argv);
    if (ddg) {
        cJSON* ddg_json = cJSON_Parse(ddg);
        if (ddg_json) {
            //collected_any_data = true;

             // === Handle DDG AnswerType (weather, dictionary, calculation, etc.) ===
            cJSON* answerType = cJSON_GetObjectItem(ddg_json, "AnswerType");
            if (answerType && cJSON_IsString(answerType)) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "source", "DDG");
                char combined_AT[512] = { 0 };
                snprintf(combined_AT, sizeof(combined_AT),
                    "DuckDuckGo AnswerType: %s", answerType->valuestring);
                cJSON_AddStringToObject(item, "title", combined_AT);
                cJSON_AddStringToObject(item, "snippet", ddg);
                cJSON_AddItemToArray(master_results, item);
                collected_any_data = true;
            }


            if (cJSON_IsArray(ddg_json)) {
                cJSON* element = NULL;
                cJSON_ArrayForEach(element, ddg_json) {
                    cJSON* title_item = cJSON_GetObjectItem(element, "title");
                    cJSON* snippet_item = cJSON_GetObjectItem(element, "snippet");
                    cJSON* url_item = cJSON_GetObjectItem(element, "url");

                    if (title_item && cJSON_IsString(title_item)) {
                        // CRITICAL FIX: Skip this item if it is a DuckDuckGo/Bing Ad link!
                        if (url_item && url_item->valuestring) {
                            if (strstr(url_item->valuestring, "/y.js") != NULL ||
                                strstr(url_item->valuestring, "aclick") != NULL ||
                                strstr(url_item->valuestring, "ad_") != NULL) {
                                continue; // Skip to the next result in the array
                            }
                        }

                        cJSON* item = cJSON_CreateObject();
                        cJSON_AddStringToObject(item, "source", "DDG");

                        char combined[1024] = { 0 };
                        snprintf(combined, sizeof(combined), "%s - %s",
                            title_item->valuestring,
                            (snippet_item && snippet_item->valuestring) ? snippet_item->valuestring : "");

                        cJSON_AddStringToObject(item, "title", combined);
                        if (url_item && url_item->valuestring) {
                            cJSON_AddStringToObject(item, "url", url_item->valuestring);
                        }
                        cJSON_AddItemToArray(master_results, item);
                    }
                }
            }
            else if (cJSON_IsObject(ddg_json)) {
                cJSON* title_item = cJSON_GetObjectItem(ddg_json, "title");
                cJSON* snippet_item = cJSON_GetObjectItem(ddg_json, "snippet");
                cJSON* url_item = cJSON_GetObjectItem(ddg_json, "url");

                if (title_item && cJSON_IsString(title_item)) {
                    // CRITICAL FIX: Skip this object if it's a lone ad layout placement
                    bool is_ad = false;
                    if (url_item && url_item->valuestring) {
                        if (strstr(url_item->valuestring, "/y.js") != NULL ||
                            strstr(url_item->valuestring, "aclick") != NULL ||
                            strstr(url_item->valuestring, "ad_") != NULL) {
                            is_ad = true;
                        }
                    }

                    if (!is_ad) {
                        cJSON* item = cJSON_CreateObject();
                        cJSON_AddStringToObject(item, "source", "DDG");

                        char combined[1024] = { 0 };
                        snprintf(combined, sizeof(combined), "%s - %s",
                            title_item->valuestring,
                            (snippet_item && snippet_item->valuestring) ? snippet_item->valuestring : "");

                        cJSON_AddStringToObject(item, "title", combined);
                        if (url_item && url_item->valuestring) {
                            cJSON_AddStringToObject(item, "url", url_item->valuestring);
                        }
                        cJSON_AddItemToArray(master_results, item);
                    }
                }
            }

            cJSON_Delete(ddg_json);
        }
        //char ddg_text[2048];   // writable buffer
        //snprintf(ddg_text, sizeof(ddg_text), "%s", ddg);
        
        // === DDG FALLBACK: ALWAYS ADD RAW RESULT IF NOTHING PARSED ===
        if (ddg && strlen(ddg) > 0) {
            // Only add fallback if the parsed DDG JSON gave you nothing
            if (!collected_any_data) {
                cJSON* raw_ddg = cJSON_CreateObject();
                cJSON_AddStringToObject(raw_ddg, "source", "DDG");
                cJSON_AddStringToObject(raw_ddg, "title", "Raw DuckDuckGo response");
                cJSON_AddStringToObject(raw_ddg, "snippet", ddg);
                cJSON_AddItemToArray(master_results, raw_ddg);
                collected_any_data = true;
            }
        }

        free(ddg);
    }

   // ==================== 4. CHROMIUM SEARCH SECTOR ====================

    char* chromium = plugin_chromium(argc, argv);
    if (chromium) {

       /* printf("\n================ RAW CHROMIUM OUTPUT ================\n%s\n\n", chromium);*/

        cJSON* chromium_json = cJSON_Parse(chromium);
        if (chromium_json) {
            collected_any_data = true;

            // We expect: { "type": "chromium_html", "html": "<!doctype html>..." }
           cJSON* type_item = cJSON_GetObjectItemCaseSensitive(chromium_json, "type");
           cJSON* html_item = cJSON_GetObjectItemCaseSensitive(chromium_json, "html");

         //   printf("TYPE: %s\n", type_item ? type_item->valuestring : "NULL");
         //   printf("HTML: %s\n", html_item ? html_item->valuestring : "NULL");

           if (type_item && cJSON_IsString(type_item) &&
               html_item && cJSON_IsString(html_item) &&
               strcmp(type_item->valuestring, "chromium_html") == 0) {

               cJSON* item = cJSON_CreateObject();
               cJSON_AddStringToObject(item, "source", "Chromium");
               cJSON_AddStringToObject(item, "title", "Chromium HTML snapshot");

               char full_url[2048];
               snprintf(full_url, sizeof(full_url),
                   "https://www.google.com/search?q=%s", query);
               cJSON_AddStringToObject(item, "url", full_url);

               char* urls = extract_google_urls(html_item->valuestring);
               if (urls && strlen(urls) > 0) {
                   cJSON_AddStringToObject(item, "snippet", urls);
                   free(urls);
               }
               else {
                   cJSON_AddStringToObject(item, "snippet", "No URLs found.");
               }

               cJSON_AddItemToArray(master_results, item);
           }


            cJSON_Delete(chromium_json);
        }

        // === Chromium Fallback: Always add raw HTML if structured parse fails ===
        if (chromium && strlen(chromium) > 0) {
            // Only add fallback if HTML didn't match chromium_html format
            if (!collected_any_data) {
                cJSON* raw_chrome = cJSON_CreateObject();
                cJSON_AddStringToObject(raw_chrome, "source", "Chromium");
                cJSON_AddStringToObject(raw_chrome, "title", "Raw Chromium HTML");
                cJSON_AddStringToObject(raw_chrome, "snippet", chromium);
                cJSON_AddItemToArray(master_results, raw_chrome);
                collected_any_data = true;
            }
        }

        free(chromium);
    }

    // ==================== WRAP UP & CACHE SHIPMENT ====================
    if (collected_any_data) {
        char* final_json = cJSON_PrintUnformatted(master_root);
        cJSON_Delete(master_root);
        cache_put(query, final_json);
        return final_json;
    }

    // Failure Path
    cJSON_Delete(master_root);
    size_t error_buf_size = strlen(query) + 128;
    char* error = (char*)malloc(error_buf_size);
    if (error) {
        snprintf(error, error_buf_size, "{\"error\":\"websearch_failed\",\"query\":\"%s\"}", query);
        cache_put(query, error);
        return error;
    }
    return _strdup("{\"error\":\"allocation_failed\"}");
}

char* plugin_exa_search(int argc, char** argv) {
    if (argc < 1) return _strdup("{\"error\":\"exa_missing_query\"}");

    char query[512] = { 0 };
    for (int i = 0; i < argc; i++) {
        strncat(query, argv[i], sizeof(query) - strlen(query) - 2);
        if (i + 1 < argc) strncat(query, " ", sizeof(query) - strlen(query) - 2);
    }

    char json_body[2048];
    snprintf(json_body, sizeof(json_body),
        "{ \"query\": \"%s\", \"n_tokens\": 2048 }",
        query);

    char* result = http_post_json_search(
        L"api.exa.ai",
        L"/search",
        json_body);

    if (!result)
        return _strdup("{\"error\":\"exa_search_failed\"}");

    return result;
}

char* plugin_exa_fetch(int argc, char** argv) {
    if (argc < 1) return _strdup("{\"error\":\"exa_missing_url\"}");

    char json_body[2048];
    snprintf(json_body, sizeof(json_body),
        "{\"urls\": [\"%s\"], \"text\": true}",  //"{ \"url\": \"%s\" }",
        argv[0]);

    char* result = http_post_json_fetch(
        L"api.exa.ai",
        L"/contents",    // L"/fetch",
        json_body);

    if (!result)
        return _strdup("{\"error\":\"exa_fetch_failed\"}");

    return result;
}
