#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>   // defines CRITICAL_SECTION, InitializeCriticalSection, EnterCriticalSection, ...
#endif

#include "engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTER_MAX_MODELS 16

    typedef struct {
        char name[64];
        char path[MAX_PATH];

        engine_t* engine;

        CRITICAL_SECTION lock;

        long requests;

    } routed_model_t;

    typedef struct {
        routed_model_t models[ROUTER_MAX_MODELS];

        int count;

        routed_model_t* active;
    } router_t;

    extern router_t g_router;

    /* initialization */

    void router_init(void);
    void router_shutdown(void);

    /* model management */

    int router_add_model(
        const char* name,
        const char* path
    );

    engine_t* router_get_engine(
        const char* name
    );

    int router_switch(
        const char* name
    );

    /* helpers */

    engine_t* router_active_engine(void);

    const char* router_active_name(void);

    void router_list_models(void);

#ifdef __cplusplus
}
#endif

