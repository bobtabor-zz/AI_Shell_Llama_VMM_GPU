#include "pager.h"
#include "util.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct page_slot {
    size_t id;
    unsigned long long age;
    unsigned char* data;
    int valid;
} page_slot_t;

struct pager {
    char* path;
    size_t cache_bytes;
    size_t page_bytes;
    int slots;
    page_slot_t* slot;
    unsigned long long tick;
};

static int read_range(const char* path, size_t offset, void* dst, size_t bytes){
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if(h == INVALID_HANDLE_VALUE) return -1;

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if(!SetFilePointerEx(h, li, NULL, FILE_BEGIN)){
        CloseHandle(h);
        return -2;
    }

    DWORD rd = 0;
    if(!ReadFile(h, dst, (DWORD)bytes, &rd, NULL)){
        CloseHandle(h);
        return -3;
    }

    CloseHandle(h);
    return 0;
#else
    (void)path; (void)offset; (void)dst; (void)bytes;
    return -99;
#endif
}

pager_t* pager_init(const char* path, size_t cache_bytes, size_t page_bytes){
    pager_t* p = (pager_t*)calloc(1, sizeof(*p));
    if(!p) return NULL;

    p->path = _strdup(path);
    p->page_bytes = page_bytes ? page_bytes : (64ULL * 1024 * 1024);
    p->cache_bytes = cache_bytes ? cache_bytes : (8ULL * 1024 * 1024);
    p->slots = (int)(cache_bytes / p->page_bytes);

    if(p->slots < 1) p->slots = 1;

    p->slot = (page_slot_t*)calloc(p->slots, sizeof(page_slot_t));
    for(int i = 0; i < p->slots; i++){
        p->slot[i].data = (unsigned char*)malloc(p->page_bytes);
        p->slot[i].valid = 0;
    }

    p->tick = 0;
    return p;
}

void pager_close(pager_t* p){
    if(!p) return;
    for(int i = 0; i < p->slots; i++){
        free(p->slot[i].data);
    }
    free(p->slot);
    free(p->path);
    free(p);
}

static int ensure_page(pager_t* p, size_t page_id){
    for(int i = 0; i < p->slots; i++){
        if(p->slot[i].valid && p->slot[i].id == page_id){
            p->slot[i].age = ++p->tick;
            return i;
        }
    }

    int victim = 0;
    unsigned long long best_age = ~0ULL;

    for(int i = 0; i < p->slots; i++){
        if(!p->slot[i].valid){
            victim = i;
            best_age = 0;
            break;
        }
        if(p->slot[i].age < best_age){
            best_age = p->slot[i].age;
            victim = i;
        }
    }

    size_t off = page_id * p->page_bytes;
    if(read_range(p->path, off, p->slot[victim].data, p->page_bytes) != 0)
        return -1;

    p->slot[victim].valid = 1;
    p->slot[victim].id = page_id;
    p->slot[victim].age = ++p->tick;

    return victim;
}

int pager_read(pager_t* p, size_t offset, void* dst, size_t len){
    size_t done = 0;

    while(done < len){
        size_t abs = offset + done;
        size_t page_id = abs / p->page_bytes;
        size_t in_page_off = abs % p->page_bytes;
        size_t avail = p->page_bytes - in_page_off;
        size_t to_copy = (len - done < avail) ? (len - done) : avail;

        int idx = ensure_page(p, page_id);
        if(idx < 0) return -1;

        memcpy((unsigned char*)dst + done,
               p->slot[idx].data + in_page_off,
               to_copy);

        done += to_copy;
    }

    return 0;
}

int pager_prefetch(pager_t* p, size_t offset, size_t len){
    (void)p; (void)offset; (void)len;
    return 0;
}

int pager_open_file(pager_t* p, const char* path);
void pager_bind_range(pager_t* p, int handle, void* vaddr, uint64_t file_offset, size_t size);
void* pager_map(pager_t* p, int handle, uint64_t file_offset, size_t size);
