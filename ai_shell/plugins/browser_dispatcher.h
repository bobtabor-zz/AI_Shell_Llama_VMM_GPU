#ifndef BROWSER_DISPATCHER_H
#define BROWSER_DISPATCHER_H

#include <windows.h>
#include "../include/plugin.h"

// ------------------------------------------------------------
// Action types
// ------------------------------------------------------------
typedef enum {
    WEB_ACT_OPEN,
    WEB_ACT_CLICK,
    WEB_ACT_TYPE,
    WEB_ACT_SUBMIT,
    WEB_ACT_EXTRACT,
    WEB_ACT_UNKNOWN
} web_action_type_t;

// ------------------------------------------------------------
// Parsed action struct
// ------------------------------------------------------------
typedef struct {
    web_action_type_t type;
    char url[512];
    char selector[512];
    char text[512];
} web_action_t;

// ------------------------------------------------------------
// Dispatcher API
// ------------------------------------------------------------
char* dispatch_web_action(const char* json);

#endif

