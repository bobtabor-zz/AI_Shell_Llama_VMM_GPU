#include "browser_dispatcher.h"
#include "llama_runner.h"        // your llama.cpp wrapper
#include "dom_encoder.h"         // your DOM → text encoder
#include "../include/plugin.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int is_final_answer(const char* s) {
    // WebLlama final answer is plain text (no JSON)
    return strstr(s, "{\"") == NULL;
}

void run_weblama_agent(const char* user_goal) {
    const int MAX_STEPS = 32;

    // 0. Start browser on a default page
    plugin_chromium(1, (char* []) { "open https://www.google.com" });

    for (int step = 0; step < MAX_STEPS; ++step) {

        // 1. Capture DOM + URL
        char* dom_html = plugin_chromium(1, (char* []) { "extract_dom" });
        char* url = plugin_chromium(1, (char* []) { "extract_url" });

        // 2. Encode DOM into readable text
        char* dom_text = encode_dom_to_text(dom_html, url);

        // 3. Build prompt for WebLlama
        char prompt[65536];
        snprintf(prompt, sizeof(prompt),
            "You are Llama3-Web, a deterministic web-navigation agent.\n"
            "User goal: %s\n\n"
            "Current page:\n%s\n\n"
            "Decide the next action.\n"
            "Respond ONLY with JSON actions.\n",
            user_goal, dom_text
        );

        // 4. Run model
        char* model_out = llama_generate_json(prompt);

        // 5. Check if model gave final answer (plain text)
        if (is_final_answer(model_out)) {
            printf("\n=== FINAL ANSWER ===\n%s\n", model_out);
            free(model_out);
            break;
        }

        // 6. Dispatch JSON action(s)
        //    Your dispatcher already handles single-action JSON.
        char* result = dispatch_web_action(model_out);

        // 7. Print debug
        printf("[step %d] action: %s\n", step, model_out);
        printf("[browser] result: %s\n", result);

        free(result);
        free(model_out);

        // 8. Loop continues: browser state changed → new DOM → new prompt
    }
}
