#ifndef ENGINE_GGUF_LOADER_H
#define ENGINE_GGUF_LOADER_H

#include <stdint.h>
#include <stddef.h>

// -----------------------------
// GGUF tensor type enum
// -----------------------------
typedef enum {
    ENGINE_GGUF_TYPE_F32,
    ENGINE_GGUF_TYPE_F16,
    ENGINE_GGUF_TYPE_Q4_0,
    ENGINE_GGUF_TYPE_Q4_1,
    ENGINE_GGUF_TYPE_Q5_0,
    ENGINE_GGUF_TYPE_Q5_1,
    ENGINE_GGUF_TYPE_Q8_0,
    ENGINE_GGUF_TYPE_Q8_1,
    ENGINE_GGUF_TYPE_Q2_K,
    ENGINE_GGUF_TYPE_Q3_K,
    ENGINE_GGUF_TYPE_Q4_K,
    ENGINE_GGUF_TYPE_Q5_K,
    ENGINE_GGUF_TYPE_Q6_K
} engine_gguf_type_t;

// -----------------------------
// GGUF header
// -----------------------------
typedef struct engine_gguf_header {
    uint32_t version;
    uint64_t n_tensors;
    uint64_t n_kv;
    uint64_t tensor_data_offset;
} engine_gguf_header_t;

// -----------------------------
// Tensor info
// -----------------------------
typedef struct engine_gguf_tensor_info {
    char* name;
    uint32_t  n_dims;
    uint32_t  type;
    uint64_t  offset;
    uint64_t* dims;
} engine_gguf_tensor_info_t;

// -----------------------------
// Loader context
// -----------------------------
typedef struct engine_gguf_loader {
    uint8_t* base;
    uint64_t              size;

    engine_gguf_header_t  hdr;
    engine_gguf_tensor_info_t* tensors;
} engine_gguf_loader_t;

// -----------------------------
// Loader API
// -----------------------------
engine_gguf_loader_t* engine_gguf_open(const char* path);
void                   engine_gguf_close(engine_gguf_loader_t* ctx);

void engine_gguf_get_header(engine_gguf_loader_t* ctx,
    engine_gguf_header_t* out);

void engine_gguf_get_tensors(engine_gguf_loader_t* ctx,
    engine_gguf_tensor_info_t** out_tensors,
    uint64_t* out_count);

size_t engine_gguf_tensor_nbytes(const engine_gguf_tensor_info_t* ti);

#endif // ENGINE_GGUF_LOADER_H
