#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "routerEngine.h"

router_t g_router;

void router_init(void)
{
    memset(&g_router, 0, sizeof(g_router));
}

void router_shutdown(void)
{
    for (int i = 0; i < g_router.count; i++)
    {
        if (g_router.models[i].engine)
        {
            printf(
                "[router] unloading %s\n",
                g_router.models[i].name
            );

            engine_close(
                g_router.models[i].engine
            );

            g_router.models[i].engine = NULL;
        }
    }

    g_router.count = 0;
    g_router.active = NULL;
}

int router_add_model(
    const char* name,
    const char* path
)
{
    if (g_router.count >= ROUTER_MAX_MODELS)
    {
        return -1;
    }

    routed_model_t* m =
        &g_router.models[g_router.count];

    memset(m, 0, sizeof(*m));

    strncpy(
        m->name,
        name,
        sizeof(m->name) - 1
    );

    strncpy(
        m->path,
        path,
        sizeof(m->path) - 1
    );

    g_router.count++;

    printf(
        "[router] registered: %s\n",
        name
    );

    return 0;
}

engine_t* router_get_engine(
    const char* name
)
{
    for (int i = 0; i < g_router.count; i++)
    {
        routed_model_t* m =
            &g_router.models[i];

        if (_stricmp(m->name, name) == 0)
        {
            if (!m->engine)
            {
                printf(
                    "[router] loading model %s\n",
                    m->name
                );

                m->engine =
                    engine_open(m->path);

                if (!m->engine)
                {
                    printf(
                        "[router] failed loading %s\n",
                        m->name
                    );

                    return NULL;
                }
            }

            return m->engine;
        }
    }

    return NULL;
}

int router_switch(
    const char* name
)
{
    engine_t* e =
        router_get_engine(name);

    if (!e)
    {
        return -1;
    }

    for (int i = 0; i < g_router.count; i++)
    {
        routed_model_t* m =
            &g_router.models[i];

        if (_stricmp(m->name, name) == 0)
        {
            g_router.active = m;

            printf(
                "[router] active model = %s\n",
                m->name
            );

            return 0;
        }
    }

    return -1;
}

engine_t* router_active_engine(void)
{
    if (!g_router.active)
    {
        return NULL;
    }

    printf(
        "[router debug] active=%p engine=%p\n",
        g_router.active,
        g_router.active ? g_router.active->engine : NULL
    );

    return g_router.active->engine;
}

const char* router_active_name(void)
{
    if (!g_router.active)
    {
        return "none";
    }

    return g_router.active->name;
}

void router_list_models(void)
{
    printf("\n");

    for (int i = 0; i < g_router.count; i++)
    {
        routed_model_t* m =
            &g_router.models[i];

        printf(
            "%c %s [%s]\n",
            (g_router.active == m)
            ? '*'
            : ' ',
            m->name,
            m->engine
            ? "loaded"
            : "unloaded"
        );
    }

    printf("\n");
}