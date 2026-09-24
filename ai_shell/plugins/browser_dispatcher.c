#include "browser_dispatcher.h"
#include "../include/plugin.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ------------------------------------------------------------
// Parse Llama3-Web JSON into action struct
// ------------------------------------------------------------
web_action_t parse_web_action(const char* json) {
    web_action_t act;
    memset(&act, 0, sizeof(web_action_t));

    if (strstr(json, "\"open\"")) act.type = WEB_ACT_OPEN;
    else if (strstr(json, "\"click\"")) act.type = WEB_ACT_CLICK;
    else if (strstr(json, "\"type\"")) act.type = WEB_ACT_TYPE;
    else if (strstr(json, "\"submit\"")) act.type = WEB_ACT_SUBMIT;
    else if (strstr(json, "\"extract\"")) act.type = WEB_ACT_EXTRACT;
    else act.type = WEB_ACT_UNKNOWN;

    cJSON* root = cJSON_Parse(json);
    if (!root) return act;

    cJSON* obj = NULL;

    switch (act.type) {
    case WEB_ACT_OPEN:
        obj = cJSON_GetObjectItem(root, "open");
        if (obj) {
            cJSON* url = cJSON_GetObjectItem(obj, "url");
            if (url && cJSON_IsString(url))
                strncpy(act.url, url->valuestring, sizeof(act.url) - 1);
        }
        break;

    case WEB_ACT_CLICK:
        obj = cJSON_GetObjectItem(root, "click");
        if (obj) {
            cJSON* sel = cJSON_GetObjectItem(obj, "selector");
            if (sel && cJSON_IsString(sel))
                strncpy(act.selector, sel->valuestring, sizeof(act.selector) - 1);
        }
        break;

    case WEB_ACT_TYPE:
        obj = cJSON_GetObjectItem(root, "type");
        if (obj) {
            cJSON* sel = cJSON_GetObjectItem(obj, "selector");
            cJSON* txt = cJSON_GetObjectItem(obj, "text");
            if (sel && cJSON_IsString(sel))
                strncpy(act.selector, sel->valuestring, sizeof(act.selector) - 1);
            if (txt && cJSON_IsString(txt))
                strncpy(act.text, txt->valuestring, sizeof(act.text) - 1);
        }
        break;

    case WEB_ACT_SUBMIT:
        obj = cJSON_GetObjectItem(root, "submit");
        if (obj) {
            cJSON* sel = cJSON_GetObjectItem(obj, "selector");
            if (sel && cJSON_IsString(sel))
                strncpy(act.selector, sel->valuestring, sizeof(act.selector) - 1);
        }
        break;

    case WEB_ACT_EXTRACT:
        obj = cJSON_GetObjectItem(root, "extract");
        if (obj) {
            cJSON* sel = cJSON_GetObjectItem(obj, "selector");
            if (sel && cJSON_IsString(sel))
                strncpy(act.selector, sel->valuestring, sizeof(act.selector) - 1);
        }
        break;
    }

    cJSON_Delete(root);
    return act;
}

// ------------------------------------------------------------
// Dispatcher: uses plugin_chromium() ONLY
// ------------------------------------------------------------
char* dispatch_web_action(const char* json) {
    web_action_t act = parse_web_action(json);

    char arg[1024] = { 0 };

    switch (act.type) {

    case WEB_ACT_OPEN:
        snprintf(arg, sizeof(arg), "open %s", act.url);
        return plugin_chromium(1, (char**)&arg);

    case WEB_ACT_CLICK:
        snprintf(arg, sizeof(arg), "click %s", act.selector);
        return plugin_chromium(1, (char**)&arg);

    case WEB_ACT_TYPE:
        snprintf(arg, sizeof(arg), "type %s %s", act.selector, act.text);
        return plugin_chromium(1, (char**)&arg);

    case WEB_ACT_SUBMIT:
        snprintf(arg, sizeof(arg), "submit %s", act.selector);
        return plugin_chromium(1, (char**)&arg);

    case WEB_ACT_EXTRACT:
        snprintf(arg, sizeof(arg), "extract %s", act.selector);
        return plugin_chromium(1, (char**)&arg);

    default:
        return _strdup("{\"error\":\"unknown_action\"}");
    }
}
