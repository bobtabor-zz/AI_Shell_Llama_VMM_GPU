#include "http_server.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <winsock2.h>
#include <ws2tcpip.h>

//#include "../include/plugin.h"   // plugin_invoke()
#include "../engine/engine.h"


#pragma comment(lib, "Ws2_32.lib")

static int running = 1;

extern engine_t* g_engine;
char* json_escape(const char* s);


// ADD THIS HERE
char* json_escape(const char* s) {
    size_t len = strlen(s);
    // Worst case: every char becomes \x → 2x size
    char* out = (char*)malloc(len * 2 + 1);
    char* p = out;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = s[i];
        switch (c) {
        case '\"': *p++ = '\\'; *p++ = '\"'; break;
        case '\\': *p++ = '\\'; *p++ = '\\'; break;
        case '\n': *p++ = '\\'; *p++ = 'n'; break;
        case '\r': *p++ = '\\'; *p++ = 'r'; break;
        case '\t': *p++ = '\\'; *p++ = 't'; break;
        default:
            if (c < 32) {
                // Control chars → \u00XX
                sprintf(p, "\\u%04x", c);
                p += 6;
            }
            else {
                *p++ = c;
            }
        }
    }

    *p = 0;
    return out;
}
char* json_escape_full(const char* s) {
    size_t len = strlen(s);

    // Worst case: every byte becomes \u00XX (6 chars)
    char* out = (char*)malloc(len * 6 + 1);
    if (!out) return NULL;

    char* p = out;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = s[i];

        switch (c) {
        case '\"': *p++ = '\\'; *p++ = '\"'; break;
        case '\\': *p++ = '\\'; *p++ = '\\'; break;
        case '\b': *p++ = '\\'; *p++ = 'b';  break;
        case '\f': *p++ = '\\'; *p++ = 'f';  break;
        case '\n': *p++ = '\\'; *p++ = 'n';  break;
        case '\r': *p++ = '\\'; *p++ = 'r';  break;
        case '\t': *p++ = '\\'; *p++ = 't';  break;

        default:
            if (c < 0x20) {
                // Control characters → \u00XX
                sprintf(p, "\\u%04x", c);
                p += 6;
            }
            else {
                *p++ = c;
            }
        }
    }

    *p = '\0';
    return out;
}


static void send_http_response(SOCKET client, const char* json) {
    char header[512];
    int len = (int)strlen(json);

    sprintf(header,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        len
    );

    send(client, header, (int)strlen(header), 0);
    send(client, json, len, 0);
}

char* json_extract_string(char* s, char* out, size_t out_sz)
{
    size_t pos = 0;
    s++; // skip opening quote

    while (*s && pos < out_sz - 1) {
        if (*s == '\\') {
            s++;
            if (*s == 'n') out[pos++] = '\n';
            else if (*s == 't') out[pos++] = '\t';
            else if (*s == '\\') out[pos++] = '\\';
            else if (*s == '"') out[pos++] = '"';
            else out[pos++] = *s;
            s++;
        }
        else if (*s == '"') {
            s++; // closing quote
            break;
        }
        else {
            out[pos++] = *s++;
        }
    }

    out[pos] = 0;
    return s;
}



static void handle_client(SOCKET client) {
    char buffer[8192];
    int received = recv(client, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) return;

    buffer[received] = 0;

    // --- CORS preflight ---
    if (strncmp(buffer, "OPTIONS", 7) == 0) {
        const char* resp =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Headers: *\r\n"
            "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
            "Connection: close\r\n\r\n";
        send(client, resp, (int)strlen(resp), 0);
        return;
    }

    // ============================================================
    // ⭐ INSERT YOUR /api/generate ENDPOINT RIGHT HERE
    // ============================================================
    if (strncmp(buffer, "POST /api/generate", 18) == 0) {

        // Extract JSON body
        char* body = strstr(buffer, "\r\n\r\n");
        if (!body) return;
        body += 4;

        // Parse "prompt"
        char* prompt = strstr(body, "\"prompt\"");
        if (!prompt) {
            send_http_response(client, "{\"error\":\"missing prompt\"}");
            return;
        }

        //char* start = strchr(prompt, ':');
        //char* quote1 = strchr(start, '"') + 1;
        //char* quote2 = strchr(quote1, '"');

        char prompt_text[4096];
        //int len = (int)(quote2 - quote1);
        //memcpy(prompt_text, quote1, len);
        //prompt_text[len] = 0;

        char* start = strchr(prompt, ':');
        char* quote1 = strchr(start, '"');

        json_extract_string(quote1, prompt_text, sizeof(prompt_text));


        // --- RUN THE MODEL ---
        char outbuf[65536];
        outbuf[0] = 0;

        int rc = engine_chat_html(g_engine, prompt_text, outbuf, sizeof(outbuf));
        if (rc < 0) {
            send_http_response(client, "{\"error\":\"engine_chat failed\"}");
            return;
        }
        // Escape JSON
        //char* escaped = json_escape(outbuf);
        char* escaped = json_escape_full(outbuf);
        // Build JSON response
        char json[70000];
        snprintf(json, sizeof(json),
            "{ \"prompt\": \"%s\", \"output\": \"%s\" }",
            prompt_text, escaped
        );

        free(escaped);
        send_http_response(client, json);
        return;
    }



    // ============================================================
    // Existing plugin handler (leave this as-is)
    // ============================================================
    char* body = strstr(buffer, "\r\n\r\n");
    if (!body) return;
    body += 4;

   /*char* response = plugin_invoke_http(body);
    if (!response)
        response = _strdup("{\"error\":\"plugin returned null\"}");*/
   
    //send_http_response(client, response);


    send_http_response(client, buffer);
    free(buffer);
}



int http_server_start(int port) {
    WSADATA wsa;
    SOCKET server, client;
    struct sockaddr_in addr;

    WSAStartup(MAKEWORD(2, 2), &wsa);

    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) return -1;

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        return -2;

    listen(server, 5);

    printf("[http] listening on port %d\n", port);

    while (running) {
        client = accept(server, NULL, NULL);
        if (client == INVALID_SOCKET) continue;

        handle_client(client);
        closesocket(client);
    }

    closesocket(server);
    WSACleanup();
    return 0;
}

void http_server_stop(void) {
    running = 0;
}


void test_json_escape() {
    const char* s =
        "Hello \"Bob\"\n"
        "Line2\tTabbed\n"
        "Control:\x01\x02\x03\n"
        "Backslash: \\\n";

    char* escaped = json_escape_full(s);

    printf("ORIGINAL:\n%s\n\n", s);
    printf("ESCAPED:\n%s\n\n", escaped);

    free(escaped);
}
