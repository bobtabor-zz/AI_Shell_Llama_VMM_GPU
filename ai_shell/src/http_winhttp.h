#ifndef HTTP_WINHTTP_H
#define HTTP_WINHTTP_H

int http_post_json(const char* url,
    const char* api_key,
    const char* body,
    char** out_response);

#endif
#pragma once
