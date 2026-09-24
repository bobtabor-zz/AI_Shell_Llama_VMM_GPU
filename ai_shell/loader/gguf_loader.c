#include "gguf_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -----------------------------------------------------------------------------
// type size helpers (block size and bytes-per-block)
// -----------------------------------------------------------------------------

static int g_blksize[32];
static int g_typesize[32];
static int g_tables_init = 0;

static void init_type_tables(void) {
    // block sizes
    g_blksize[ENGINE_GGUF_TYPE_F32]  = 1;
    g_blksize[ENGINE_GGUF_TYPE_F16]  = 1;
    g_blksize[ENGINE_GGUF_TYPE_Q4_0] = 32;
    g_blksize[ENGINE_GGUF_TYPE_Q4_1] = 32;
    g_blksize[ENGINE_GGUF_TYPE_Q5_0] = 32;
    g_blksize[ENGINE_GGUF_TYPE_Q5_1] = 32;
    g_blksize[ENGINE_GGUF_TYPE_Q8_0] = 32;
    g_blksize[ENGINE_GGUF_TYPE_Q8_1] = 32;

    g_blksize[ENGINE_GGUF_TYPE_Q2_K] = 256;
    g_blksize[ENGINE_GGUF_TYPE_Q3_K] = 256;
    g_blksize[ENGINE_GGUF_TYPE_Q4_K] = 256;
    g_blksize[ENGINE_GGUF_TYPE_Q5_K] = 256;
    g_blksize[ENGINE_GGUF_TYPE_Q6_K] = 256;

    // bytes per block
    g_typesize[ENGINE_GGUF_TYPE_F32]  = 4;
    g_typesize[ENGINE_GGUF_TYPE_F16]  = 2;
    g_typesize[ENGINE_GGUF_TYPE_Q4_0] = 16;
    g_typesize[ENGINE_GGUF_TYPE_Q4_1] = 16;
    g_typesize[ENGINE_GGUF_TYPE_Q5_0] = 20;
    g_typesize[ENGINE_GGUF_TYPE_Q5_1] = 20;
    g_typesize[ENGINE_GGUF_TYPE_Q8_0] = 32;
    g_typesize[ENGINE_GGUF_TYPE_Q8_1] = 32;

    g_typesize[ENGINE_GGUF_TYPE_Q2_K] = 64;
    g_typesize[ENGINE_GGUF_TYPE_Q3_K] = 80;
    g_typesize[ENGINE_GGUF_TYPE_Q4_K] = 96;
    g_typesize[ENGINE_GGUF_TYPE_Q5_K] = 112;
    g_typesize[ENGINE_GGUF_TYPE_Q6_K] = 128;

    g_tables_init = 1;
}

enum {
    GGUF_SCALAR_BOOL = 0,
    GGUF_SCALAR_U8 = 1,
    GGUF_SCALAR_I8 = 2,
    GGUF_SCALAR_U16 = 3,
    GGUF_SCALAR_I16 = 4,
    GGUF_SCALAR_U32 = 5,
    GGUF_SCALAR_I32 = 6,
    GGUF_SCALAR_F32 = 7,
    GGUF_SCALAR_U64 = 8,
    GGUF_SCALAR_I64 = 9,
    GGUF_SCALAR_F64 = 10,
    GGUF_SCALAR_STR = 11,
    GGUF_SCALAR_ARR = 12,
    GGUF_SCALAR_BYTE_BUFFER = 13,   // ⭐ NEW — required for GGUF v3
    GGUF_SCALAR_BOOL_ARR = 14,   // ⭐ NEW
    GGUF_SCALAR_I8_ARR = 15    // ⭐ NEW
};

static void ensure_tables(void) {
    if (!g_tables_init) {
        init_type_tables();
    }
}

static size_t engine_ggml_tensor_size(uint32_t type, const uint64_t * dims, uint32_t n_dims) {
    ensure_tables();

    uint64_t n = 1;
    for (uint32_t i = 0; i < n_dims; ++i) {
        n *= dims[i];
    }

    if (type >= 32) {
        fprintf(stderr, "[ggml_tensor_size] invalid type=%u\n", type);
        return 0;
    }

    uint64_t bs = g_blksize[type];
    uint64_t ts = g_typesize[type];

    if (bs == 0 || ts == 0) {
        fprintf(stderr, "[ggml_tensor_size] unknown layout for type=%u\n", type);
        return 0;
    }

    uint64_t n_blocks = (n + bs - 1) / bs;
    return (size_t) (n_blocks * ts);
}

// -----------------------------------------------------------------------------
// basic readers
// -----------------------------------------------------------------------------

static int read_u32(const uint8_t * base, uint64_t size, uint64_t * off, uint32_t * out) {
    if (*off + 4 > size) {
        return 0;
    }
    memcpy(out, base + *off, 4);
    *off += 4;
    return 1;
}

static int read_u64(const uint8_t * base, uint64_t size, uint64_t * off, uint64_t * out) {
    if (*off + 8 > size) {
        return 0;
    }
    memcpy(out, base + *off, 8);
    *off += 8;
    return 1;
}

static int skip_bytes(uint64_t size, uint64_t * off, uint64_t n) {
    if (*off > size || n > size - *off) {
        return 0;
    }
    *off += n;
    return 1;
}

static int skip_kv_value(const uint8_t* base, uint64_t size, uint64_t* off, uint32_t type) {
    int ok = 1;

    switch (type) {

        // ---------------------------------------------------------
        // Simple scalar types
        // ---------------------------------------------------------
    case GGUF_SCALAR_BOOL:
    case GGUF_SCALAR_U8:
    case GGUF_SCALAR_I8:
        ok = skip_bytes(size, off, 1);
        break;

    case GGUF_SCALAR_U16:
    case GGUF_SCALAR_I16:
        ok = skip_bytes(size, off, 2);
        break;

    case GGUF_SCALAR_U32:
    case GGUF_SCALAR_I32:
    case GGUF_SCALAR_F32:
        ok = skip_bytes(size, off, 4);
        break;

    case GGUF_SCALAR_U64:
    case GGUF_SCALAR_I64:
    case GGUF_SCALAR_F64:
        ok = skip_bytes(size, off, 8);
        break;

        // ---------------------------------------------------------
        // String
        // ---------------------------------------------------------
    case GGUF_SCALAR_STR:
    {
        uint64_t sl = 0;
        if (!read_u64(base, size, off, &sl)) return 0;
        ok = skip_bytes(size, off, sl);
    } break;

    // ---------------------------------------------------------
    // NEW in GGUF v3: BYTE BUFFER
    // ---------------------------------------------------------
    case GGUF_SCALAR_BYTE_BUFFER:
    {
        uint64_t bl = 0;
        if (!read_u64(base, size, off, &bl)) return 0;
        ok = skip_bytes(size, off, bl);
    } break;

    // ---------------------------------------------------------
    // Array
    // ---------------------------------------------------------
    case GGUF_SCALAR_ARR:
    {
        uint32_t arr_type = 0;
        uint64_t arr_len = 0;

        if (!read_u32(base, size, off, &arr_type)) return 0;
        if (!read_u64(base, size, off, &arr_len))  return 0;

        // Array of strings
        if (arr_type == GGUF_SCALAR_STR) {
            for (uint64_t j = 0; j < arr_len && ok; ++j) {
                uint64_t sl = 0;
                if (!read_u64(base, size, off, &sl)) return 0;
                ok = skip_bytes(size, off, sl);
            }
        }

        // Array of byte buffers (GGUF v3)
        else if (arr_type == GGUF_SCALAR_BYTE_BUFFER) {
            for (uint64_t j = 0; j < arr_len && ok; ++j) {
                uint64_t bl = 0;
                if (!read_u64(base, size, off, &bl)) return 0;
                ok = skip_bytes(size, off, bl);
            }
        }

        // Array of numeric scalars
        else {
            size_t elem_size = 0;
            switch (arr_type) {
            case GGUF_SCALAR_BOOL:
            case GGUF_SCALAR_U8:
            case GGUF_SCALAR_I8:
                elem_size = 1; break;

            case GGUF_SCALAR_U16:
            case GGUF_SCALAR_I16:
                elem_size = 2; break;

            case GGUF_SCALAR_U32:
            case GGUF_SCALAR_I32:
            case GGUF_SCALAR_F32:
                elem_size = 4; break;

            case GGUF_SCALAR_U64:
            case GGUF_SCALAR_I64:
            case GGUF_SCALAR_F64:
                elem_size = 8; break;

            case GGUF_SCALAR_BOOL_ARR:
            {
                uint64_t len = 0;
                if (!read_u64(base, size, off, &len)) return 0;
                ok = skip_bytes(size, off, len);   // 1 byte per bool
            } break;

            case GGUF_SCALAR_I8_ARR:
            {
                uint64_t len = 0;
                if (!read_u64(base, size, off, &len)) return 0;
                ok = skip_bytes(size, off, len);   // 1 byte per int8
            } break;


            default:
                fprintf(stderr, "[gguf] unknown KV type=%u at off=%llu\n",
                    type, (unsigned long long) * off);
                return 0; // unknown array element type
            }

            ok = skip_bytes(size, off, (uint64_t)elem_size * arr_len);
        }
    } break;

    // ---------------------------------------------------------
    // Unknown type → fail
    // ---------------------------------------------------------
    default:
        return 0;
    }

    return ok;
}


// -----------------------------------------------------------------------------
// open / close
// -----------------------------------------------------------------------------

engine_gguf_loader_t* engine_gguf_open(const char* path) {
    fprintf(stderr, "[gguf] open '%s'\n", path);

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "[gguf_open] failed to open %s\n", path);
        return NULL;
    }

    // -------------------------------------------------------------
    // 64‑bit file size (Windows-safe)
    // -------------------------------------------------------------
    uint64_t fsz = 0;

#if defined(_WIN32)
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "[gguf_open] _fseeki64 failed\n");
        fclose(fp);
        return NULL;
    }

    __int64 fsz64 = _ftelli64(fp);
    if (fsz64 <= 0) {
        fprintf(stderr, "[gguf_open] file too small or size overflow\n");
        fclose(fp);
        return NULL;
    }

    fsz = (uint64_t)fsz64;

    if (_fseeki64(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "[gguf_open] _fseeki64 reset failed\n");
        fclose(fp);
        return NULL;
    }
#else
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }

    long fsz_long = ftell(fp);
    if (fsz_long <= 0) {
        fprintf(stderr, "[gguf_open] file too small or size overflow\n");
        fclose(fp);
        return NULL;
    }

    fsz = (uint64_t)fsz_long;
    fseek(fp, 0, SEEK_SET);
#endif

    // -------------------------------------------------------------
    // Read entire file into memory
    // -------------------------------------------------------------
    uint8_t* buf = (uint8_t*)malloc((size_t)fsz);
    if (!buf) {
        fprintf(stderr, "[gguf_open] malloc failed (%llu bytes)\n",
            (unsigned long long)fsz);
        fclose(fp);
        return NULL;
    }

    if (fread(buf, 1, (size_t)fsz, fp) != (size_t)fsz) {
        fprintf(stderr, "[gguf_open] fread failed\n");
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    // -------------------------------------------------------------
    // Allocate loader context
    // -------------------------------------------------------------
    engine_gguf_loader_t* ctx =
        (engine_gguf_loader_t*)calloc(1, sizeof(engine_gguf_loader_t));

    if (!ctx) {
        free(buf);
        return NULL;
    }

    ctx->base = buf;
    ctx->size = fsz;

    // -------------------------------------------------------------
    // Parse GGUF header
    // -------------------------------------------------------------
    uint64_t off = 0;

    if (ctx->size < 4 + 4 + 8 + 8) {
        fprintf(stderr, "[gguf_open] file too small for header\n");
        engine_gguf_close(ctx);
        return NULL;
    }

    // magic
    if (memcmp(ctx->base, "GGUF", 4) != 0) {
        fprintf(stderr, "[gguf_open] bad magic\n");
        engine_gguf_close(ctx);
        return NULL;
    }
    // after magic "GGUF"
    off += 4;

    uint32_t version = 0;
    uint64_t n_tensors = 0;
    uint64_t n_kv = 0;
    uint32_t alignment = 0;   // extra field after n_kv

    if (!read_u32(ctx->base, ctx->size, &off, &version) ||
        !read_u64(ctx->base, ctx->size, &off, &n_tensors) ||
        !read_u64(ctx->base, ctx->size, &off, &n_kv) ||
        !read_u32(ctx->base, ctx->size, &off, &alignment)) {

        engine_gguf_close(ctx);
        return NULL;
    }

    ctx->hdr.version = version;
    ctx->hdr.n_tensors = n_tensors;
    ctx->hdr.n_kv = n_kv;

    fprintf(stderr,
        "[gguf] version=%u n_tensors=%llu n_kv=%llu alignment=%u\n",
        version,
        (unsigned long long)n_tensors,
        (unsigned long long)n_kv,
        alignment);




    // -------------------------------------------------------------
    // Skip KV section
    // -------------------------------------------------------------
    for (uint64_t i = 0; i < ctx->hdr.n_kv; ++i) {
        uint64_t key_len = 0;
        uint32_t type = 0;

        if (!read_u64(ctx->base, ctx->size, &off, &key_len)) {
            engine_gguf_close(ctx);
            return NULL;
        }
        if (!skip_bytes(ctx->size, &off, key_len)) {
            engine_gguf_close(ctx);
            return NULL;
        }
        if (!read_u32(ctx->base, ctx->size, &off, &type)) {
            engine_gguf_close(ctx);
            return NULL;
        }
        if (!skip_kv_value(ctx->base, ctx->size, &off, type)) {
            fprintf(stderr, "[gguf_open] KV parse failed at entry %llu (type=%u)\n",
                (unsigned long long)i, type);
            engine_gguf_close(ctx);
            return NULL;
        }
    }


    // -------------------------------------------------------------
    // Tensor metadata
    // -------------------------------------------------------------
    ctx->tensors = (engine_gguf_tensor_info_t*)
        calloc((size_t)ctx->hdr.n_tensors, sizeof(engine_gguf_tensor_info_t));

    if (!ctx->tensors) {
        fprintf(stderr, "[gguf_open] tensor alloc failed\n");
        engine_gguf_close(ctx);
        return NULL;
    }

    for (uint64_t i = 0; i < ctx->hdr.n_tensors; ++i) {
        engine_gguf_tensor_info_t* ti = &ctx->tensors[i];

        uint64_t name_len = 0;
        if (!read_u64(ctx->base, ctx->size, &off, &name_len) ||
            off + name_len > ctx->size) {

            fprintf(stderr, "[gguf_open] tensor name read failed\n");
            engine_gguf_close(ctx);
            return NULL;
        }

        ti->name = (char*)malloc((size_t)name_len + 1);
        memcpy(ti->name, ctx->base + off, (size_t)name_len);
        ti->name[name_len] = 0;
        off += name_len;

        if (!read_u32(ctx->base, ctx->size, &off, &ti->n_dims) ||
            !read_u32(ctx->base, ctx->size, &off, &ti->type)) {

            fprintf(stderr, "[gguf_open] tensor dims/type failed\n");
            engine_gguf_close(ctx);
            return NULL;
        }

        ti->dims = (uint64_t*)calloc(ti->n_dims, sizeof(uint64_t));
        if (!ti->dims) {
            fprintf(stderr, "[gguf_open] dims alloc failed\n");
            engine_gguf_close(ctx);
            return NULL;
        }

        for (uint32_t d = 0; d < ti->n_dims; ++d) {
            if (!read_u64(ctx->base, ctx->size, &off, &ti->dims[d])) {
                fprintf(stderr, "[gguf_open] dim read failed\n");
                engine_gguf_close(ctx);
                return NULL;
            }
        }

        if (!read_u64(ctx->base, ctx->size, &off, &ti->offset)) {
            fprintf(stderr, "[gguf_open] offset read failed\n");
            engine_gguf_close(ctx);
            return NULL;
        }
    }

    ctx->hdr.tensor_data_offset = off;
    return ctx;
}




void engine_gguf_close(engine_gguf_loader_t * ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->tensors) {
        for (uint64_t i = 0; i < ctx->hdr.n_tensors; ++i) {
            engine_gguf_tensor_info_t * ti = &ctx->tensors[i];
            free(ti->name);
            free(ti->dims);
        }
        free(ctx->tensors);
    }

    free(ctx->base);
    free(ctx);
}

// -----------------------------------------------------------------------------
// simple accessors
// -----------------------------------------------------------------------------

void engine_gguf_get_header(engine_gguf_loader_t * ctx, engine_gguf_header_t * out) {
    if (!ctx || !out) {
        return;
    }
    *out = ctx->hdr;
}

void engine_gguf_get_tensors(engine_gguf_loader_t *       ctx,
                             engine_gguf_tensor_info_t ** out_tensors,
                             uint64_t *                   out_count) {
    if (!ctx) {
        return;
    }
    if (out_tensors) {
        *out_tensors = ctx->tensors;
    }
    if (out_count) {
        *out_count = ctx->hdr.n_tensors;
    }
}

size_t engine_gguf_tensor_nbytes(const engine_gguf_tensor_info_t * ti) {
    if (!ti) {
        return 0;
    }
    return engine_ggml_tensor_size(ti->type, ti->dims, ti->n_dims);
}
