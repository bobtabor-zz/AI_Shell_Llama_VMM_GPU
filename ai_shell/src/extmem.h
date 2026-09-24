#ifndef EXTMEM_H
#define EXTMEM_H
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include "vmm.h"

typedef struct extmem_io extmem_io_t;

typedef struct ext_load_stats { size_t bytes; double seconds; double mb_per_s; int threads; } ext_load_stats_t;

typedef enum { DEV_CLASS_UNKNOWN=0, DEV_CLASS_NVME_SSD, DEV_CLASS_SSD, DEV_CLASS_HDD } device_class_t;

typedef struct io_tuning { device_class_t dev_class; int threads; size_t chunk_bytes; int valid; } io_tuning_t;

extmem_io_t* extmem_io_create(int threads, size_t chunk_bytes);
void         extmem_io_destroy(extmem_io_t* io);

int extmem_load_file_into_region_mt(extmem_io_t* io, const char* path, vmm_t* v, vmm_region_t* region, size_t max_bytes, ext_load_stats_t* stats);
int extmem_load_file_range(const char* path, size_t offset, void* dst, size_t bytes);

void        extmem_autotune_enable(int enable);
int         extmem_autotune_enabled(void);
io_tuning_t extmem_get_tuning_for_path(const char* path);
io_tuning_t extmem_refresh_tuning_for_path(const char* path);
void        extmem_print_tunemap(FILE* sink);

#endif
