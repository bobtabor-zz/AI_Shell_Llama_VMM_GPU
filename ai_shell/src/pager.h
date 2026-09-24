#ifndef PAGER_H
#define PAGER_H
#include <stddef.h>

typedef struct pager pager_t;

pager_t* pager_init(const char* path, size_t cache_bytes, size_t page_bytes);
void     pager_close(pager_t* p);
int      pager_read(pager_t* p, size_t offset, void* dst, size_t len);
int      pager_prefetch(pager_t* p, size_t offset, size_t len);

#endif
