#include "extmem.h"
#include "util.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#endif
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

struct extmem_io { int threads; size_t chunk_bytes; };

typedef struct { io_tuning_t map[26]; int enabled; } tuning_cache_t;
static tuning_cache_t g_cache = {0};



#ifdef _WIN32
static double now_seconds(void){
  LARGE_INTEGER f,c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart/(double)f.QuadPart;
}

static int get_drive_letter_index(const char* path){
  if(!path) return -1;
  if(!isalpha((unsigned char)path[0]) || path[1] != ':') return -1;
  int idx = toupper((unsigned char)path[0]) - 'A';
  return (idx>=0 && idx<26) ? idx : -1;
}

static int open_volume_for_path(const char* file_path, HANDLE* out_vol){
  char root[MAX_PATH];
  strncpy(root, file_path, sizeof(root)-1);
  root[sizeof(root)-1]='\0';
  PathStripToRootA(root);
  char vol_path[MAX_PATH];
  snprintf(vol_path,sizeof(vol_path),"\\\\.\\%c:",root[0]);
  HANDLE h=CreateFileA(vol_path,0,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
  if(h==INVALID_HANDLE_VALUE) return -1;
  *out_vol=h;
  return 0;
}

static int get_physical_drive_number(HANDLE hvol, DWORD* out_num){
  STORAGE_DEVICE_NUMBER devnum={0};
  DWORD bytes=0;
  BOOL ok=DeviceIoControl(hvol, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                          NULL,0, &devnum, sizeof(devnum),
                          &bytes, NULL);
  if(!ok) return -1;
  *out_num=devnum.DeviceNumber;
  return 0;
}

static HANDLE open_physical_drive(DWORD num){
  char path[64];
  snprintf(path,sizeof(path),"\\\\.\\PhysicalDrive%lu",(unsigned long)num);
  HANDLE h=CreateFileA(path,0,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
  return h;
}

static device_class_t detect_device_class_win(const char* path){
  HANDLE hvol;
  if(open_volume_for_path(path,&hvol)!=0) return DEV_CLASS_UNKNOWN;
  DWORD devnum=0;
  if(get_physical_drive_number(hvol,&devnum)!=0){
    CloseHandle(hvol);
    return DEV_CLASS_UNKNOWN;
  }
  HANDLE hpd = open_physical_drive(devnum);
  CloseHandle(hvol);
  if(hpd==INVALID_HANDLE_VALUE) return DEV_CLASS_UNKNOWN;

  STORAGE_PROPERTY_QUERY qp={StorageDeviceProperty,PropertyStandardQuery,{0}};
  BYTE buf[1024]={0};
  DWORD bytes=0;
  BOOL okDesc=DeviceIoControl(hpd, IOCTL_STORAGE_QUERY_PROPERTY,
                              &qp, sizeof(qp),
                              buf, sizeof(buf),
                              &bytes, NULL);

  STORAGE_PROPERTY_QUERY qsp={StorageDeviceSeekPenaltyProperty,PropertyStandardQuery,{0}};
  DEVICE_SEEK_PENALTY_DESCRIPTOR seek={0};
  DWORD bytes2=0;
  BOOL okSeek=DeviceIoControl(hpd, IOCTL_STORAGE_QUERY_PROPERTY,
                              &qsp, sizeof(qsp),
                              &seek, sizeof(seek),
                              &bytes2, NULL);

  CloseHandle(hpd);

  device_class_t dc=DEV_CLASS_UNKNOWN;
  if(okDesc){
    STORAGE_DEVICE_DESCRIPTOR* d=(STORAGE_DEVICE_DESCRIPTOR*)buf;
    if(d->BusType==BusTypeNvme) dc=DEV_CLASS_NVME_SSD;
  }
  if(dc==DEV_CLASS_UNKNOWN && okSeek){
    dc = (seek.IncursSeekPenalty)?DEV_CLASS_HDD:DEV_CLASS_SSD;
  }
  return dc;
}
#endif

static void choose_tuning(device_class_t dc, int* threads_out, size_t* chunk_out){
  int th=4;
  size_t ch=64ULL*1024*1024;
  switch(dc){
    case DEV_CLASS_NVME_SSD: th=8; ch=128ULL*1024*1024; break;
    case DEV_CLASS_SSD:      th=4; ch=64ULL*1024*1024;  break;
    case DEV_CLASS_HDD:      th=2; ch=16ULL*1024*1024;  break;
    default:                 th=4; ch=64ULL*1024*1024;  break;
  }
  if(threads_out) *threads_out=th;
  if(chunk_out) *chunk_out=ch;
}

void extmem_autotune_enable(int enable){ g_cache.enabled = enable?1:0; }
int  extmem_autotune_enabled(void){ return g_cache.enabled; }

io_tuning_t extmem_refresh_tuning_for_path(const char* path){
  io_tuning_t t;
  memset(&t,0,sizeof(t));
#ifdef _WIN32
  int idx=get_drive_letter_index(path);
  device_class_t dc = detect_device_class_win(path);
  choose_tuning(dc, &t.threads, &t.chunk_bytes);
  t.dev_class=dc;
  t.valid=1;
  if(idx>=0) g_cache.map[idx]=t;
#else
  t.dev_class=DEV_CLASS_UNKNOWN;
  t.threads=4;
  t.chunk_bytes=64ULL*1024*1024;
  t.valid=1;
#endif
  return t;
}

io_tuning_t extmem_get_tuning_for_path(const char* path){
  io_tuning_t t;
  memset(&t,0,sizeof(t));
#ifdef _WIN32
  int idx=get_drive_letter_index(path);
  if(!g_cache.enabled || idx<0){
    device_class_t dc = detect_device_class_win(path);
    choose_tuning(dc, &t.threads, &t.chunk_bytes);
    t.dev_class=dc;
    t.valid=1;
    return t;
  }
  if(g_cache.map[idx].valid) return g_cache.map[idx];
  return extmem_refresh_tuning_for_path(path);
#else
  t.dev_class=DEV_CLASS_UNKNOWN;
  t.threads=4;
  t.chunk_bytes=64ULL*1024*1024;
  t.valid=1;
  return t;
#endif
}

void extmem_print_tunemap(FILE* sink){
  FILE* s = sink?sink:stdout;
  fprintf(s,"Drive auto-tuning map (enabled=%d):\n",g_cache.enabled);
  for(int i=0;i<26;i++){
    if(!g_cache.map[i].valid) continue;
    const char* cls=
      (g_cache.map[i].dev_class==DEV_CLASS_NVME_SSD?"NVMe SSD":
       g_cache.map[i].dev_class==DEV_CLASS_SSD?"SSD":
       g_cache.map[i].dev_class==DEV_CLASS_HDD?"HDD":"Unknown");
    fprintf(s," %c: class=%s threads=%d chunk=%llu\n",
      'A'+i, cls,
      g_cache.map[i].threads,
      (unsigned long long)g_cache.map[i].chunk_bytes);
  }
}

#ifdef _WIN32
typedef struct thread_job {
  const char* path;
  size_t offset;
  size_t bytes;
  uint8_t* dst;
  int rc;
} thread_job_t;

typedef struct thread_ctx { thread_job_t* job; } thread_ctx_t;

static DWORD WINAPI ext_worker_fn(LPVOID param){
  thread_ctx_t* ctx=(thread_ctx_t*)param;
  thread_job_t* j=ctx->job;

  HANDLE hf=CreateFileA(j->path, GENERIC_READ, FILE_SHARE_READ,
                        NULL, OPEN_EXISTING,
                        FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  if(hf==INVALID_HANDLE_VALUE){ j->rc=-1; return 0;}

  LARGE_INTEGER li;
  li.QuadPart=(LONGLONG)j->offset;
  if(!SetFilePointerEx(hf, li, NULL, FILE_BEGIN)){
    CloseHandle(hf);
    j->rc=-2;
    return 0;
  }

  const DWORD CHUNK_IO = 16*1024*1024;
  size_t remaining=j->bytes;
  uint8_t* dst=j->dst;

  while(remaining>0){
    DWORD toRead=(DWORD)(remaining>CHUNK_IO?CHUNK_IO:remaining);
    DWORD readNow=0;
    if(!ReadFile(hf, dst, toRead, &readNow, NULL)){
      CloseHandle(hf);
      j->rc=-3;
      return 0;
    }
    if(readNow==0) break;
    dst+=readNow;
    remaining-=readNow;
  }

  CloseHandle(hf);
  j->rc=0;
  return 0;
}
#endif

int extmem_load_file_into_region_mt(extmem_io_t* io,
                                    const char* path,
                                    vmm_t* v,
                                    vmm_region_t* region,
                                    size_t max_bytes,
                                    ext_load_stats_t* stats){
#ifdef _WIN32
  HANDLE h=CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                       NULL, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  if(h==INVALID_HANDLE_VALUE){
    log_msg(LOG_ERROR, "Open failed: %s", path);
    return -1;
  }

  LARGE_INTEGER sz;
  GetFileSizeEx(h, &sz);
  CloseHandle(h);

  size_t bytes = (size_t)((sz.QuadPart>(LONGLONG)max_bytes)?max_bytes:sz.QuadPart);
  if(bytes > region->size){
    log_msg(LOG_ERROR, "Region too small");
    return -2;
  }

  int threads = io?io->threads:1;
  size_t chunk = io?io->chunk_bytes:(64ULL*1024*1024);
  if(threads<1) threads=1;
  if(chunk==0) chunk=64ULL*1024*1024;

  int jobs = (int)((bytes + chunk - 1)/chunk);
  if(jobs < threads) threads = jobs;

  thread_job_t* arr = (thread_job_t*)calloc(jobs, sizeof(thread_job_t));
  HANDLE* th = (HANDLE*)calloc(threads, sizeof(HANDLE));
  thread_ctx_t* tc = (thread_ctx_t*)calloc(threads, sizeof(thread_ctx_t));

  double t0 = now_seconds();

  for(int i=0;i<jobs;i++){
    arr[i].path = path;
    arr[i].offset = (size_t)i * chunk;
    size_t remain = bytes - arr[i].offset;
    arr[i].bytes = (remain>chunk?chunk:remain);
    arr[i].dst = (uint8_t*)region->ptr + arr[i].offset;
    arr[i].rc = -999;
  }

  int next=0, running=0;

  while(next<jobs || running>0){
    while(running<threads && next<jobs){
      tc[running].job = &arr[next];
      th[running] = CreateThread(NULL,0,ext_worker_fn,&tc[running],0,NULL);
      if(!th[running]){
        free(arr); free(th); free(tc);
        return -3;
      }
      running++;
      next++;
    }

    DWORD idx = WaitForMultipleObjects(running, th, FALSE, INFINITE);
    if(idx>=WAIT_OBJECT_0 && idx<WAIT_OBJECT_0+(DWORD)running){
      int fin = (int)(idx - WAIT_OBJECT_0);
      CloseHandle(th[fin]);
      for(int k=fin;k<running-1;k++){
        th[k]=th[k+1];
        tc[k]=tc[k+1];
      }
      running--;
    } else {
      log_msg(LOG_ERROR, "WaitForMultipleObjects failed");
      break;
    }
  }

  double t1 = now_seconds();

  vmm_prefetch(v, region->ptr, bytes);
  vmm_pin(v, region);

  int rc=0;
  for(int i=0;i<jobs;i++){
    if(arr[i].rc!=0){
      rc=arr[i].rc;
      break;
    }
  }

  if(stats){
    stats->bytes = bytes;
    stats->seconds = (t1 - t0);
    stats->mb_per_s = (stats->seconds>0.0)
      ? ((double)bytes/(1024.0*1024.0))/stats->seconds
      : 0.0;
    stats->threads = threads;
  }

  free(arr);
  free(th);
  free(tc);
  return rc;
#else
  (void)io; (void)path; (void)v; (void)region; (void)max_bytes; (void)stats;
  return -99;
#endif
}


int extmem_load_file_range(const char* path, size_t offset, void* dst, size_t bytes) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) {
        CloseHandle(h);
        return -2;
    }

    uint8_t* out = (uint8_t*)dst;
    size_t remaining = bytes;

    while (remaining > 0) {
        DWORD chunk = (remaining > (1u << 30)) ? (1u << 30) : (DWORD)remaining;
        DWORD readNow = 0;

        if (!ReadFile(h, out, chunk, &readNow, NULL) || readNow != chunk) {
            CloseHandle(h);
            return -3;
        }

        out += chunk;
        remaining -= chunk;
    }

    CloseHandle(h);
    return 0;
#else
    return -99;
#endif
}



extmem_io_t* extmem_io_create(int threads, size_t chunk_bytes){
  if(threads<=0) threads=1;
  if(chunk_bytes==0) chunk_bytes=64ULL*1024*1024;
  extmem_io_t* io=(extmem_io_t*)calloc(1, sizeof(*io));
  io->threads=threads;
  io->chunk_bytes=chunk_bytes;
  return io;
}

void extmem_io_destroy(extmem_io_t* io){
  free(io);
}
