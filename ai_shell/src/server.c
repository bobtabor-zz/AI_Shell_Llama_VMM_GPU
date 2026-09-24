#include "server.h"
#include "engine.h"
#include "util.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>


static volatile int g_running=0;
static engine_t* g_engine=NULL;
static int g_port = 0;



static int parse_json_infer(const char* body,
    char* model, size_t model_sz,
    char* prompt, size_t prompt_sz)
{
    // ---- MODEL ----
    const char* m = strstr(body, "\"model\"");
    if (!m) return -1;

    m = strchr(m, ':');
    if (!m) return -1;
    m++;

    // skip whitespace + opening quote
    while (isspace(*m) || *m == '"') m++;

    const char* mend = m;
    while (*mend && *mend != '"') mend++;

    size_t mlen = mend - m;
    if (mlen >= model_sz) mlen = model_sz - 1;

    strncpy(model, m, mlen);
    model[mlen] = 0;

    // ---- PROMPT ----
    const char* p = strstr(body, "\"prompt\"");
    if (!p) return -1;

    p = strchr(p, ':');
    if (!p) return -1;
    p++;

    // skip whitespace + opening quote
    while (isspace(*p) || *p == '"') p++;

    const char* pend = p;
    while (*pend && *pend != '"') pend++;

    size_t plen = pend - p;
    if (plen >= prompt_sz) plen = prompt_sz - 1;

    strncpy(prompt, p, plen);
    prompt[plen] = 0;

    return 0;
}

static const char* strcasestr_win(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;

    size_t needle_len = strlen(needle);
    if (needle_len == 0) return haystack;

    for (const char* p = haystack; *p; p++) {
        if (_strnicmp(p, needle, needle_len) == 0) {
            return p;
        }
    }
    return NULL;
}


static int parse_json_dense(const char* body, char* model, size_t model_sz, float** x_out, int* x_len){
  const char* m = strstr(body, "\"model\""); if(!m) return -1;
  m = strchr(m, '\"'); if(!m) return -1; m++;
  m = strchr(m, '\"'); if(!m) return -1;
  const char* start = m+1; const char* end = strchr(start, '\"'); if(!end) return -1;
  size_t len = (size_t)(end-start); if(len>=model_sz) len=model_sz-1; strncpy(model,start,len); model[len]='\0';

  const char* x = strstr(end, "\"x\""); if(!x) return -1;
  x = strchr(x, '['); if(!x) return -1; x++;
  int cap=1024; float* arr=(float*)malloc(cap*sizeof(float)); int n=0;

  while(x && *x!=']'){
    char* endptr; double val = strtod(x, &endptr);
    if(endptr==x){ x++; continue; }
    if(n==cap){ cap*=2; arr=(float*)realloc(arr, cap*sizeof(float)); }
    arr[n++] = (float)val;
    x = endptr; while(*x==',' || *x==' '||*x=='\r'||*x=='\n'||*x=='\t') x++;
  }
  *x_out=arr; *x_len=n; return 0;
}

#ifdef _WIN32
static DWORD WINAPI server_thread(LPVOID param) {
    int port = *(int*)param;
    free(param);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u_short)port);

    bind(s, (struct sockaddr*)&addr, sizeof(addr));
    listen(s, 8);
    g_running = 1;

    log_msg(LOG_INFO, "HTTP server listening on %d", port);

    while (g_running) {
        SOCKET c = accept(s, NULL, NULL);
        if (c == INVALID_SOCKET) break;

        char buf[65536];
        int total = 0;

        int header_len = -1;
        int content_length = 0;

        // -----------------------------
        // READ FULL HTTP REQUEST
        // -----------------------------
        while (1) {
            int r = recv(c, buf + total, (int)sizeof(buf) - 1 - total, 0);
            //if (r <= 0) break;
            if (r <= 0) {
                // If we have headers and Content-Length, keep waiting for body
                if (header_len > 0 && content_length > 0) {
                    int body_have = total - header_len;
                    if (body_have < content_length) {
                        // Wait a little for body to arrive
                        Sleep(1);
                        continue;
                    }
                }
                break;
            }

            total += r;
            buf[total] = '\0';

            // find end of headers
            if (header_len < 0) {
                char* header_end = strstr(buf, "\r\n\r\n");
                if (header_end) {
                    header_len = (int)(header_end - buf) + 4;

                    // find Content-Length
                    char* cl = (char*)strcasestr_win(buf, "Content-Length:");
                    if (cl) {
                        cl += strlen("Content-Length:");
                        while (*cl == ' ' || *cl == '\t') cl++;
                        content_length = atoi(cl);
                    }
                }
            }

            // if we know header_len and content_length, check if full body arrived
            if (header_len > 0) {
                int body_have = total - header_len;

                if (content_length > 0 && body_have >= content_length) {
                    break;  // full body received
                }
            }

            // buffer full
            if (total >= (int)sizeof(buf) - 1) break;
        }

        if (total <= 0) {
            closesocket(c);
            continue;
        }

        buf[total] = '\0';

        // -----------------------------
        // EXTRACT BODY
        // -----------------------------
        const char* body = strstr(buf, "\r\n\r\n");
        body = body ? body + 4 : buf;

        log_msg(LOG_INFO, "REQUEST RAW:\n%s", buf);

        char model[128];
        char prompt[32768];
        float* x = NULL;
        int xlen = 0;

        // -----------------------------
        // TEXT INFERENCE
        // -----------------------------
        /*if (parse_json_infer(body, model, sizeof(model), prompt, sizeof(prompt)) == 0 && g_engine) {
            char* out = NULL;
            int rc = engine_infer_text(g_engine, model, prompt, &out);

            if (rc == 0 && out) {
                char json[65536];
                snprintf(json, sizeof(json),
                    "{ \"tokens\": \"%s\" }\r\n",
                    out);

                int json_len = (int)strlen(json);

                char header[256];
                int header_len = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    json_len);

                send(c, header, header_len, 0);
                send(c, json, json_len, 0);

                free(out);
            }
            else {
                const char* msg = "{ \"error\": \"bad request\" }\r\n";
                int msg_len = (int)strlen(msg);

                char header[256];
                int header_len = snprintf(header, sizeof(header),
                    "HTTP/1.1 400 Bad Request\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    msg_len);

                send(c, header, header_len, 0);
                send(c, msg, msg_len, 0);
            }
        }*/

        // -----------------------------
        // DENSE INFERENCE (placeholder)
        // -----------------------------
        //else 
            if (parse_json_dense(body, model, sizeof(model), &x, &xlen) == 0 && g_engine) {
            // TODO
        }

        // -----------------------------
        // BAD REQUEST
        // -----------------------------
        else {
            const char* msg = "{ \"error\": \"bad request\" }\r\n";
            int msg_len = (int)strlen(msg);

            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n"
                "\r\n",
                msg_len);

            send(c, header, header_len, 0);
            send(c, msg, msg_len, 0);
        }

        if (x) free(x);
        closesocket(c);
    }

    closesocket(s);
    WSACleanup();
    return 0;
}
#endif


int server_start(int port, engine_t* e) {
    if (g_running) return -2; // already running
    g_engine = e;
    g_port = port;
    int* pp = (int*)malloc(sizeof(int));
    *pp = port;
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, server_thread, pp, 0, NULL);
    if (!h) return -1;
    CloseHandle(h);
    return 0;
#else
    (void)pp; (void)port;
    return -99;
#endif
}

void server_stop(void) {
    g_running = 0;
}
int server_is_running(void) {
    return g_running;
}

int server_current_port(void) {
    return g_port;
}

