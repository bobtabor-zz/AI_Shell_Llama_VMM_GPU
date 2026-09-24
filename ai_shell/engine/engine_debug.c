#include "engine.h"
#include "vmm.h"
#include <stdio.h>

void engine_dump_vmm_tensors(engine_t* e) {
    if (!e || !e->vmm_model) {
        printf("No VMM model loaded.\n");
        return;
    }

    vmm_model_t* vm = e->vmm_model;

    printf("=== VMM Tensor Dump ===\n");
    printf("Tensor count: %llu\n\n", (unsigned long long)vm->tensor_count);

    for (uint64_t i = 0; i < vm->tensor_count; ++i) {
        vmm_tensor_meta_t* m = &vm->meta[i];

        printf("[%4llu] %-48s | dtype=%u | dims=%u | shape=(",
            (unsigned long long)i,
            m->name,
            m->dtype,
            m->n_dims
        );

        for (uint32_t d = 0; d < m->n_dims; d++) {
            printf("%llu", (unsigned long long)m->ne[d]);
            if (d + 1 < m->n_dims) printf(", ");
        }

        printf(") | size=%llu bytes | offset=%llu\n",
            (unsigned long long)m->size_bytes,
            (unsigned long long)m->offset
        );
    }

    printf("\n=== End of VMM Tensor Dump ===\n");
}
