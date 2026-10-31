/* PS4 virtual memory: direct (physical) pool, flexible memory, reservations,
 * protection changes and queries. One sparse memfd backs the whole direct
 * pool, so any mapping of any physical range aliases the same storage.
 * Windows: a pagefile-backed section plays the memfd's part, and the PS4 user range is
 * reserved as one placeholder that views of the section replace (VirtualAlloc2/MapViewOfFile3,
 * as shadPS4 does); see the "host mappings" section below.
 * Guest addresses are placed below 1 TiB (PS4 user range): GPU descriptors
 * encode 40-bit addresses, so host-default 0x7f... addresses would not fit. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu_process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
/* sceKernelGetDirectMemorySize on retail PS4: 5056 MiB. TLG_DMEM_MB raises it (the resolution
 * patches above 1080p need about 4 GiB more; run.sh sets it). */
static uint64_t pool_size_bytes(void) {
    static uint64_t size;
    if (!size) {
        const char *env = getenv("TLG_DMEM_MB");
        uint64_t mb = env ? strtoull(env, NULL, 10) : 0;
        if (mb < 5056 || mb > 16384) mb = 5056;
        size = mb * 1024 * 1024;
    }
    return size;
}
#define POOL_SIZE pool_size_bytes()
#define FLEXIBLE_SIZE (UINT64_C(448) * 1024 * 1024)
#define PAGE UINT64_C(16384)
#ifdef __APPLE__
/* Apple Silicon keeps [0xfc0000000, 0x7000000000) as a permanent no-access hole in every process
 * (Rosetta too): mmap there fails, so the guest range starts above it (as bloodborne_mac_port).
 * Still below 2^40, as PS4 code packing pointers into 40-bit fields needs. */
#define USER_MIN UINT64_C(0x7000000000)
#define LOW_LIMIT UINT64_C(0x0fc0000000) /* low maps stop at the hole */
#else
#define USER_MIN UINT64_C(0x1000000000)
#define LOW_LIMIT USER_MIN
#endif
#define USER_MAX UINT64_C(0xfc00000000)
/* System-reserved window (shim AddressSpace::SystemReservedVirtualBase/Size). Gnm places fixed
 * objects there, e.g. the tessellation factor ring at 0x7f0000000 that The Last Guardian maps by
 * hint and then verifies; an exact free hint in this window is honoured. */
#define SYSTEM_RESERVED_MIN UINT64_C(0x700000000)
#define SYSTEM_RESERVED_MAX UINT64_C(0x800000000)
#define LIMIT 4096
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define ACCESS ((int32_t)UINT32_C(0x8002000d))
#define NO_SPACE ((int32_t)UINT32_C(0x80020023))
#define MAP_FIXED_FLAG 0x10
#define MAP_NO_OVERWRITE 0x80
enum { KIND_RESERVED=1, KIND_DIRECT, KIND_FLEXIBLE };
typedef struct { uint64_t start, size; int type, used; } Block;
typedef struct { uintptr_t start, end; int kind, prot, type; uint64_t phys; } Vma;
typedef struct {
    uintptr_t start, end; uint64_t offset; int32_t protection, memory_type;
    uint32_t flags; char name[32];
} VirtualQueryInfo; /* flags: flexible, direct, stack, pooled, committed */
typedef struct { void *start; uint64_t offset, length; int8_t prot, type; int16_t reserved; int32_t operation; } BatchEntry;
_Static_assert(sizeof(VirtualQueryInfo)==72,"PS4 virtual query info layout");
_Static_assert(sizeof(BatchEntry)==32,"PS4 batch map entry layout");

#include "rwlock_rp.h"
/* Region table lock. Lookups (GPU per-draw queries, VirtualQuery, GPU page
 * protection) share it; map/unmap/protect take it exclusively. Both nest: code
 * under the lock may write guest memory, and a GPU-tracked page then faults into
 * runtime_memory_gpu_protect on the same thread. Readers nest because the lock prefers readers
 * (glibc's default rwlock; rwlock_rp.h on Windows); a thread holding it exclusively skips relocking. */
/* Windows: winpthreads' rwlock blocks new readers behind a waiting writer, so a nested reader
 * deadlocks with it; rwlock_rp.h has glibc's reader preference. */
static pthread_rwlock_t lock=PTHREAD_RWLOCK_INITIALIZER;
#define table_rdlock() pthread_rwlock_rdlock(&lock)
#define table_wrlock() pthread_rwlock_wrlock(&lock)
#define table_rdunlock() pthread_rwlock_unlock(&lock)
#define table_wrunlock() pthread_rwlock_unlock(&lock)
#define table_tryrdlock() pthread_rwlock_tryrdlock(&lock)
#define table_trywrlock() pthread_rwlock_trywrlock(&lock)
static __thread unsigned exclusive_depth;
/* Odd while a writer holds the lock; readers' region caches are valid for one even value. */
static uint64_t table_generation;
/* Contended acquisitions are marked for the freeze report (the uncontended path only adds a try). */
static void write_lock(void) {
    if (!exclusive_depth++) {
        if (table_trywrlock()) {
            runtime_wait_begin("runtime memory map lock (write)",&lock,NULL,NULL,0);
            table_wrlock();
            runtime_wait_end();
        }
        __atomic_add_fetch(&table_generation,1,__ATOMIC_ACQ_REL);
    }
}
static void write_unlock(void) {
    if (!--exclusive_depth) { __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE); table_wrunlock(); }
}
static void read_lock(void) {
    if (exclusive_depth || !table_tryrdlock()) return;
    runtime_wait_begin("runtime memory map lock (read)",&lock,NULL,NULL,0);
    table_rdlock();
    runtime_wait_end();
}
static void read_unlock(void) { if (!exclusive_depth) table_rdunlock(); }
static Block blocks[LIMIT];
static Vma *vmas; static size_t vma_count, vma_capacity;
#if !defined(__APPLE__)
static int pool_fd=-1;
#endif
static unsigned char *backing_base; /* second view of the pool: host writes bypass guest/GPU page protection */
#define FLEX_SPAN (UINT64_C(1024) * 1024 * 1024)
static uint64_t flex_bitmap[FLEX_SPAN/PAGE/64];
/* GPU hooks (tlggpu): notified outside the lock, in order, after each operation. */
typedef void (*GpuRange)(uintptr_t address, uint64_t size);
static GpuRange hook_map, hook_unmap, hook_invalidate;
enum { HOOK_MAP, HOOK_UNMAP, HOOK_INVALIDATE };
typedef struct { int kind; uintptr_t address; uint64_t size; } PendingHook;
static PendingHook pending[64]; static size_t pending_count;
static size_t allocations, maps, flexible_maps, protects, queries;
static uint64_t live_bytes, flexible_bytes;

static int valid_alignment(uint64_t a) { return a >= PAGE && a <= POOL_SIZE && !(a & (a-1)); }
static uint64_t align_up(uint64_t n, uint64_t a) { return (n+a-1) & ~(a-1); }
static int host_prot(int prot) {
    /* GPU read/write bits (0x10/0x20) need host access for the future GPU backend. */
    return ((prot & 0x11) ? PROT_READ : 0) | ((prot & 0x22) ? PROT_WRITE|PROT_READ : 0) | ((prot & 4) ? PROT_EXEC|PROT_READ : 0);
}
#if   defined(__APPLE__)
/* macOS (as bloodborne_mac_port): no memfd or fallocate, and a shm object cannot drop pages. The
 * pool is one anonymous region; guest views are mach_vm_remap aliases of it (shared, not copied)
 * and a discard maps fresh zero pages over its range (the old pages go with their last view:
 * callers unmap views first). The guest range and the system window are reserved (no access)
 * up front, because MAP_FIXED silently replaces whatever host library got there first (x86-64
 * mmap(NULL) under Rosetta hands out low addresses); unmaps give the reservation back. */
static int guest_range_held, system_window_held;
static void *reserve_host_range(uint64_t start, uint64_t end) {
    void *p=mmap((void *)start,end-start,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE,-1,0);
    if (p==(void *)start) return p;
    if (p!=MAP_FAILED) munmap(p,end-start);
    return NULL;
}
void runtime_memory_reserve(void) {
    static int done;
    if (done) return;
    done=1;
    guest_range_held = reserve_host_range(USER_MIN,USER_MAX)!=NULL;
    if (!guest_range_held)
        fprintf(stderr,"Runtime: guest range 0x%" PRIx64 "-0x%" PRIx64 " is taken\n",USER_MIN,USER_MAX);
    system_window_held = reserve_host_range(SYSTEM_RESERVED_MIN,SYSTEM_RESERVED_MAX)!=NULL;
    if (!system_window_held)
        fprintf(stderr,"Runtime: system-reserved window %p unavailable; hinted maps there move to the user range\n",
                (void *)SYSTEM_RESERVED_MIN);
}
static int system_window_reserved(void) { return system_window_held; }
static int in_reservation(uintptr_t address, uint64_t size) {
    return (guest_range_held && address>=USER_MIN && address+size<=USER_MAX) ||
           (system_window_held && address>=SYSTEM_RESERVED_MIN && address+size<=SYSTEM_RESERVED_MAX);
}
/* M1 of docs/macos/GPU_PROCESS_PLAN.md (as bloodborne_mac_port): the pool is a Mach named memory
 * entry, which a native arm64 GPU process can map at the same addresses; guest views map the
 * entry, and a discard zeroes in place (fresh pages over the backing view would leave another
 * process on the old ones). TLG_SHARED_GUEST_MEMORY=0 keeps the private anonymous pool. */
static mach_port_t pool_entry=MACH_PORT_NULL;
static int shared_pool(void) {
    static int value=-1;
    if (value<0) { const char *env=getenv("TLG_SHARED_GUEST_MEMORY"); value=!(env && env[0]=='0'); }
    return value;
}
/* The pool's memory entry (MACH_PORT_NULL before the first guest map or without sharing). */
mach_port_t runtime_memory_pool_entry(void) { return pool_entry; }
uint64_t runtime_memory_pool_entry_size(void) { return POOL_SIZE+FLEX_SPAN; }
/* M3: guest view changes forwarded to the native GPU process (gpu_process.c), under the lock. */
static RuntimeMemoryMirror mirror;
#define MIRROR(op,address,size,phys,prot) do { if (mirror) mirror(op,address,size,phys,prot); } while (0)
/* Direct memory occupies [0,POOL_SIZE) of the region, flexible memory [POOL_SIZE,+FLEX_SPAN);
 * placed above 1 TiB, out of the way of the guest and low ranges. */
static int pool(void) {
    if (backing_base) return 0;
    runtime_memory_reserve();
    if (!guest_range_held) return -1;
    if (shared_pool()) {
        /* Pages are created on first touch, as for anonymous memory. */
        memory_object_size_t size=POOL_SIZE+FLEX_SPAN;
        if (mach_make_memory_entry_64(mach_task_self(),&size,0,
                                      MAP_MEM_NAMED_CREATE|VM_PROT_READ|VM_PROT_WRITE|VM_PROT_EXECUTE,
                                      &pool_entry,MACH_PORT_NULL)!=KERN_SUCCESS || size<POOL_SIZE+FLEX_SPAN) {
            fputs("Runtime: cannot create the shared guest memory entry\n",stderr);
            return -1;
        }
        mach_vm_address_t at=UINT64_C(1)<<40;
        if (mach_vm_map(mach_task_self(),&at,POOL_SIZE+FLEX_SPAN,0,VM_FLAGS_FIXED,pool_entry,0,FALSE,
                        VM_PROT_READ|VM_PROT_WRITE,VM_PROT_READ|VM_PROT_WRITE,VM_INHERIT_NONE)!=KERN_SUCCESS)
            return -1;
        backing_base=(unsigned char *)(uintptr_t)at;
        return 0;
    }
    void *view=mmap((void *)(UINT64_C(1)<<40),POOL_SIZE+FLEX_SPAN,PROT_READ|PROT_WRITE,
                    MAP_PRIVATE|MAP_ANON|MAP_NORESERVE,-1,0);
    if (view==MAP_FAILED) return -1;
    backing_base=view;
    return 0;
}
static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    if (reserved)
        return mmap((void *)address,size,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE|MAP_FIXED,-1,0)==MAP_FAILED ? -1 : 0;
    mach_vm_address_t at=address; vm_prot_t current, maximum;
    const kern_return_t kr = shared_pool()
        ? mach_vm_map(mach_task_self(),&at,size,0,VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,pool_entry,phys,FALSE,
                      VM_PROT_READ|VM_PROT_WRITE,VM_PROT_ALL,VM_INHERIT_SHARE)
        : mach_vm_remap(mach_task_self(),&at,size,0,VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,mach_task_self(),
                        (mach_vm_address_t)(uintptr_t)(backing_base+phys),FALSE,&current,&maximum,VM_INHERIT_SHARE);
    if (kr!=KERN_SUCCESS) return -1;
    return mprotect((void *)address,size,prot);
}
static int host_unmap(uintptr_t address, uint64_t size) {
    if (in_reservation(address,size)) /* back to reserved */
        return mmap((void *)address,size,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE|MAP_FIXED,-1,0)==MAP_FAILED ? -1 : 0;
    return munmap((void *)address,size);
}
static int host_protect(uintptr_t address, uint64_t size, int prot) { return mprotect((void *)address,size,prot); }
static void host_discard(uint64_t phys, uint64_t size) {
    if (shared_pool()) {
        /* Zero in place (MADV_ZERO is ENOTSUP under Rosetta), only the pages that may hold data
         * (resident, compressed or paged out), so untouched ones stay uncommitted. */
        const size_t host_page=(size_t)sysconf(_SC_PAGESIZE), chunk=UINT64_C(64)<<20;
        static char vec[(UINT64_C(64)<<20)/4096];
        for (uint64_t off=0; off<size; off+=chunk) {
            const size_t n=(size_t)(size-off<chunk ? size-off : chunk);
            unsigned char *at=backing_base+phys+off;
            if (mincore(at,n,vec)) { memset(at,0,n); continue; }
            for (size_t page=0; page*host_page<n; ++page)
                if (vec[page]) memset(at+page*host_page,0,host_page);
        }
        return;
    }
    mmap(backing_base+phys,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE|MAP_FIXED,-1,0);
}
#else
/* Direct memory occupies [0,POOL_SIZE) of the memfd, flexible memory [POOL_SIZE,+FLEX_SPAN). */
static int pool(void) {
    if (pool_fd>=0) return 0;
    pool_fd=memfd_create("tlg-guest-memory", MFD_CLOEXEC);
    if (pool_fd<0 || ftruncate(pool_fd,(off_t)(POOL_SIZE+FLEX_SPAN))) return -1;
    void *view=mmap(NULL,POOL_SIZE+FLEX_SPAN,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_NORESERVE,pool_fd,0);
    if (view==MAP_FAILED) return -1;
    backing_base=view;
    return 0;
}
void runtime_memory_reserve(void) {}
static int system_window_reserved(void) { return 1; }
static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    void *mapped = !reserved
        ? mmap((void *)address,size,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)phys)
        : mmap((void *)address,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED,-1,0);
    return mapped==MAP_FAILED ? -1 : 0;
}
static int host_unmap(uintptr_t address, uint64_t size) { return munmap((void *)address,size); }
static int host_protect(uintptr_t address, uint64_t size, int prot) { return mprotect((void *)address,size,prot); }
static void host_discard(uint64_t phys, uint64_t size) {
    fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)phys,(off_t)size);
}
#endif
static void queue_hook(int kind, uintptr_t address, uint64_t size) {
    GpuRange hook = kind==HOOK_MAP ? hook_map : kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
    if (!hook) return;
    if (pending_count==sizeof(pending)/sizeof(*pending)) { fputs("STOP: GPU hook queue overflow\n",stderr); exit(21); }
    pending[pending_count++]=(PendingHook){kind,address,size};
}
/* Runs queued GPU notifications; callers invoke it after dropping the lock. */
static void flush_hooks(void) {
    for (;;) {
        write_lock();
        if (!pending_count) { write_unlock(); return; }
        PendingHook h=pending[0];
        memmove(pending,pending+1,--pending_count*sizeof(*pending));
        GpuRange hook = h.kind==HOOK_MAP ? hook_map : h.kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
        write_unlock();
        if (hook) hook(h.address,h.size);
    }
}
static int flex_test(uint64_t page) { return (int)((flex_bitmap[page/64]>>(page%64))&1); }
static void flex_set(uint64_t first, uint64_t count, int used) {
    for (uint64_t p=first;p<first+count;++p) {
        if (used) flex_bitmap[p/64]|=UINT64_C(1)<<(p%64); else flex_bitmap[p/64]&=~(UINT64_C(1)<<(p%64));
    }
}
static uint64_t flex_alloc(uint64_t size) {
    uint64_t pages=size/PAGE, total=FLEX_SPAN/PAGE, run=0;
    for (uint64_t p=0;p<total;++p) {
        run = flex_test(p) ? 0 : run+1;
        if (run==pages) { flex_set(p+1-pages,pages,1); return POOL_SIZE+(p+1-pages)*PAGE; }
    }
    return UINT64_MAX;
}
static void flex_free(uint64_t phys, uint64_t size) {
    flex_set((phys-POOL_SIZE)/PAGE,size/PAGE,0);
    host_discard(phys,size);
}
static size_t vma_index(uintptr_t a) { /* first VMA with end > a */
    size_t lo=0, hi=vma_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (vmas[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static int vma_insert(size_t at, Vma v) {
    if (vma_count==vma_capacity) {
        size_t capacity=vma_capacity ? vma_capacity*2 : 256;
        Vma *next=realloc(vmas,capacity*sizeof(*vmas));
        if (!next) return -1;
        vmas=next; vma_capacity=capacity;
    }
    memmove(vmas+at+1,vmas+at,(vma_count-at)*sizeof(*vmas));
    vmas[at]=v; ++vma_count; return 0;
}
static void vma_erase(size_t at, size_t n) {
    memmove(vmas+at,vmas+at+n,(vma_count-at-n)*sizeof(*vmas)); vma_count-=n;
}
static int split_at(uintptr_t a) {
    size_t i=vma_index(a);
    if (i==vma_count || vmas[i].start>=a) return 0;
    Vma right=vmas[i];
    right.start=a;
    if (right.kind==KIND_DIRECT) right.phys+=a-vmas[i].start;
    vmas[i].end=a;
    return vma_insert(i+1,right);
}
/* Range [start,end) split on its edges; returns the first index inside it. */
static size_t carve(uintptr_t start, uintptr_t end, int *error) {
    *error = split_at(start) || split_at(end);
    return vma_index(start);
}
static int covered(uintptr_t start, uintptr_t end, int allow_reserved) {
    size_t i=vma_index(start); uintptr_t at=start;
    for (; at<end; ++i) {
        if (i==vma_count || vmas[i].start>at || (!allow_reserved && vmas[i].kind==KIND_RESERVED)) return 0;
        at=vmas[i].end;
    }
    return 1;
}
static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (!ignore_reserved || vmas[i].kind!=KIND_RESERVED) return 1;
    return 0;
}
/* First-fit search in the PS4 user range, starting from the hint. */
static int range_free(uintptr_t start, uint64_t size) {
    size_t i=vma_index(start);
    return i==vma_count || vmas[i].start>=start+size;
}
static uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    if (hint>=SYSTEM_RESERVED_MIN && hint+size<=SYSTEM_RESERVED_MAX && !(hint%alignment) && system_window_reserved() && range_free(hint,size))
        return hint;
    uintptr_t at=align_up(hint<USER_MIN ? USER_MIN : hint, alignment);
    for (size_t i=vma_index(at); at+size<=USER_MAX; ++i) {
        if (i==vma_count || vmas[i].start>=at+size) return at;
        at=align_up(vmas[i].end,alignment);
    }
    return 0;
}
static void drop_range(uintptr_t start, uintptr_t end) {
    int error; size_t i=carve(start,end,&error), n=0;
    while (i+n<vma_count && vmas[i+n].end<=end && vmas[i+n].start>=start) {
        if (vmas[i+n].kind==KIND_FLEXIBLE) {
            flexible_bytes-=vmas[i+n].end-vmas[i+n].start;
            flex_free(vmas[i+n].phys,vmas[i+n].end-vmas[i+n].start);
        }
        ++n;
    }
    vma_erase(i,n);
}
/* Places a host mapping; with MAP_FIXED it replaces what the guest had there. */
static int32_t place(void **inout, uint64_t size, int prot, int flags, uint64_t alignment,
                     int kind, int type, uint64_t phys) {
    uintptr_t address=(uintptr_t)*inout;
    if (flags & MAP_FIXED_FLAG) {
        if (!address || address%PAGE) return INVALID;
        if ((flags & MAP_NO_OVERWRITE) && overlaps(address,address+size,1)) return NO_MEMORY;
    } else {
        address=find_free(address,size,alignment);
        if (!address) return NO_MEMORY;
    }
    if (host_map(address,size,host_prot(prot),kind==KIND_RESERVED,phys)) return NO_MEMORY;
    void *mapped=(void *)address;
    drop_range(address,address+size);
    if (vma_insert(vma_index(address),(Vma){address,address+size,kind,prot,type,phys})) return NO_MEMORY;
    if (kind==KIND_FLEXIBLE) flexible_bytes+=size;
    if (kind!=KIND_RESERVED) queue_hook(HOOK_MAP,address,size);
    if (kind!=KIND_RESERVED) MIRROR(RUNTIME_MIRROR_MAP,address,size,phys,prot);
    else MIRROR(RUNTIME_MIRROR_UNMAP,address,size,0,0);
    *inout=mapped;
    return 0;
}
static int block_type(uint64_t phys) {
    for (int i=0;i<LIMIT;++i)
        if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) return blocks[i].type;
    return -1;
}
static int direct_range_allocated(uint64_t phys, uint64_t size) {
    for (uint64_t at=phys; at<phys+size;) {
        int found=0;
        for (int i=0;i<LIMIT;++i) if (blocks[i].used && at>=blocks[i].start && at-blocks[i].start<blocks[i].size) {
            at=blocks[i].start+blocks[i].size; found=1; break;
        }
        if (!found) return 0;
    }
    return 1;
}

static ABI uint64_t direct_size(void) { return POOL_SIZE; }
static ABI int32_t direct_allocate(int64_t low, int64_t high, uint64_t size,
                                  uint64_t alignment, int type, int64_t *out) {
    if (!alignment) alignment = PAGE;
    if (!out || low < 0 || high < 0 || !size || size % PAGE || !valid_alignment(alignment) || type < 0 || type > 10)
        return INVALID;
    uint64_t end = (uint64_t)high < POOL_SIZE ? (uint64_t)high : POOL_SIZE;
    if ((uint64_t)low >= end || size > end - (uint64_t)low) return NO_SPACE;
    write_lock();
    if (pool()) { write_unlock(); return NO_MEMORY; }
    uint64_t start = align_up((uint64_t)low, alignment);
    int slot = -1;
    for (int i=0; i<LIMIT; ++i) if (!blocks[i].used) { slot=i; break; }
    for (;;) {
        if (slot<0 || start > end || size > end-start) { write_unlock(); return NO_SPACE; }
        int overlap = 0;
        for (int i=0; i<LIMIT; ++i) {
            Block b = blocks[i];
            if (b.used && start < b.start+b.size && b.start < start+size) {
                start = align_up(b.start+b.size, alignment); overlap = 1; break;
            }
        }
        if (!overlap) break;
    }
    blocks[slot] = (Block){start, size, type, 1}; *out = (int64_t)start;
    ++allocations; live_bytes += size;
    write_unlock();
    printf("Runtime: direct allocation offset=0x%" PRIx64 ", size=%" PRIu64 " bytes, type=%d\n", start, size, type);
    return 0;
}
static int32_t map_direct_locked(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!out || physical < 0 || (uint64_t)physical % PAGE || !size || size % PAGE || !valid_alignment(alignment) ||
        (prot & ~0x37)) return INVALID;
    if (!direct_range_allocated((uint64_t)physical,size)) return INVALID;
    int32_t result=place(out,size,prot,flags,alignment,KIND_DIRECT,block_type((uint64_t)physical),(uint64_t)physical);
    if (!result) ++maps;
    return result;
}
static ABI int32_t direct_map(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    write_lock();
    int32_t result=map_direct_locked(out,size,prot,flags,physical,alignment);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped direct memory %p, size=%" PRIu64 ", prot=0x%x\n", *out, size, prot);
    return result;
}
static ABI int32_t direct_map_named(void **out, uint64_t size, int prot, int flags, int64_t physical,
                                    uint64_t alignment, const char *name) {
    (void)name; return direct_map(out,size,prot,flags,physical,alignment);
}
static int32_t unmap_locked(uintptr_t start, uint64_t size) {
    if (start%PAGE || !size) return INVALID;
    uint64_t end=start+align_up(size,PAGE);
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (vmas[i].kind!=KIND_RESERVED) {
            uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
            queue_hook(HOOK_UNMAP,a,b-a);
        }
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i) {
        uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
        if (host_unmap(a,b-a)) return INVALID;
    }
    drop_range(start,end);
    MIRROR(RUNTIME_MIRROR_UNMAP,start,end-start,0,0);
    return 0;
}
static ABI int32_t direct_unmap(void *address, uint64_t size) {
    /* The GPU forgets the range before it disappears (as shadPS4's early GPU unmap). */
    if (hook_unmap && !((uintptr_t)address%PAGE) && size) hook_unmap((uintptr_t)address,align_up(size,PAGE));
    write_lock();
    size_t before=pending_count;
    int32_t result=unmap_locked((uintptr_t)address,size);
    pending_count=before; /* already notified above */
    write_unlock();
    return result;
}
static ABI int32_t direct_release(uint64_t start, uint64_t size) {
    if (start % PAGE || size % PAGE) return INVALID;
    if (!size) return 0;
    write_lock();
    if (!direct_range_allocated(start,size)) { write_unlock(); return INVALID; }
    /* Mappings of released physical pages disappear with them. */
    for (size_t i=0; i<vma_count;) {
        Vma v=vmas[i];
        uint64_t length=v.end-v.start;
        if (v.kind==KIND_DIRECT && v.phys<start+size && start<v.phys+length) {
            uint64_t a=v.phys>start ? v.phys : start, b=v.phys+length<start+size ? v.phys+length : start+size;
            unmap_locked(v.start+(a-v.phys),b-a);
            i=vma_index(v.start);
            continue;
        }
        ++i;
    }
    for (int i=0; i<LIMIT; ++i) {
        Block *b=&blocks[i];
        if (!b->used || b->start>=start+size || start>=b->start+b->size) continue;
        uint64_t a=b->start>start ? b->start : start, e=b->start+b->size<start+size ? b->start+b->size : start+size;
        Block left=*b, right=*b;
        left.size=a-b->start; right.start=e; right.size=b->start+b->size-e;
        b->used=0;
        if (left.size) *b=left;
        if (right.size) for (int j=0;j<LIMIT;++j) if (!blocks[j].used) { blocks[j]=right; break; }
        live_bytes-=e-a;
    }
    host_discard(start,size); /* zero on reuse */
    write_unlock();
    flush_hooks();
    return 0;
}
static int32_t map_flexible_locked(void **inout, uint64_t size, int prot, int flags) {
    if (!inout || !size || size%PAGE || (prot & ~0x37)) return INVALID;
    if (flexible_bytes+size>FLEXIBLE_SIZE || pool()) return NO_MEMORY;
    uint64_t phys=flex_alloc(size);
    if (phys==UINT64_MAX) return NO_MEMORY;
    int32_t result=place(inout,size,prot,flags,PAGE,KIND_FLEXIBLE,0,phys);
    if (result) flex_free(phys,size); else ++flexible_maps;
    return result;
}
static ABI int32_t map_flexible(void **inout, uint64_t size, int prot, int flags) {
    write_lock();
    int32_t result=map_flexible_locked(inout,size,prot,flags);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped flexible memory %p, size=%" PRIu64 ", prot=0x%x\n", *inout, size, prot);
    else fprintf(stderr,"Runtime: flexible map of %" PRIu64 " bytes failed (0x%x), in use=%" PRIu64 "\n",
                 size, (unsigned)result, flexible_bytes);
    return result;
}
static ABI int32_t map_flexible_named(void **inout, uint64_t size, int prot, int flags, const char *name) {
    (void)name; return map_flexible(inout,size,prot,flags);
}
static ABI int32_t release_flexible(void *address, uint64_t size) { return direct_unmap(address,size); }
/* POSIX mmap as libc uses it: anonymous memory from the flexible pool. File mappings fail with
 * ENODEV (no guest file descriptor can be mapped). */
static ABI void *posix_mmap(void *address, uint64_t size, int prot, int flags, int fd, int64_t offset) {
    if (!(flags & 0x1000) || fd!=-1 || offset) { *runtime_errno()=19; return (void *)-1; }
    void *out=address;
    int32_t r=map_flexible(&out,(size+PAGE-1)&~(PAGE-1),prot,flags & 0x10);
    if (r) { *runtime_errno()=12; return (void *)-1; }
    return out;
}
static ABI int32_t posix_msync(void *address, uint64_t size, int flags) { (void)address; (void)size; (void)flags; return 0; }
static ABI int32_t reserve_range(void **inout, uint64_t size, int flags, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!inout || !size || size%PAGE || !valid_alignment(alignment)) return INVALID;
    write_lock();
    if ((flags & MAP_FIXED_FLAG) && overlaps((uintptr_t)*inout,(uintptr_t)*inout+size,0)) {
        write_unlock(); return NO_MEMORY;
    }
    int32_t result=place(inout,size,0,flags,alignment,KIND_RESERVED,0,0);
    write_unlock();
    if (!result) printf("Runtime: reserved virtual range %p, size=%" PRIu64 "\n", *inout, size);
    return result;
}
static int32_t protect_locked(uintptr_t start, uint64_t size, int prot, int type) {
    uintptr_t end=start+align_up(size,PAGE);
    start&=~(uintptr_t)(PAGE-1);
    if ((prot & ~0x37) || !covered(start,end,0)) return INVALID;
    if (host_protect(start,end-start,host_prot(prot))) return INVALID;
    int error; size_t i=carve(start,end,&error);
    if (error) return NO_MEMORY;
    for (; i<vma_count && vmas[i].start<end; ++i) { vmas[i].prot=prot; if (type>=0) vmas[i].type=type; }
    MIRROR(RUNTIME_MIRROR_PROTECT,start,end-start,0,prot);
    queue_hook(HOOK_INVALIDATE,start,end-start);
    ++protects;
    return 0;
}
static ABI int32_t kernel_mprotect(const void *address, uint64_t size, int prot) {
    write_lock();
    int32_t result=protect_locked((uintptr_t)address,size,prot,-1);
    write_unlock();
    flush_hooks();
    return result;
}
static ABI int32_t query_protection(void *address, void **start, void **end, uint32_t *prot) {
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || vmas[i].start>(uintptr_t)address || vmas[i].kind==KIND_RESERVED) {
        read_unlock(); return ACCESS;
    }
    /* Report the whole contiguous run with identical protection. */
    size_t first=i, last=i;
    while (first && vmas[first-1].end==vmas[first].start && vmas[first-1].kind!=KIND_RESERVED && vmas[first-1].prot==vmas[i].prot) --first;
    while (last+1<vma_count && vmas[last+1].start==vmas[last].end && vmas[last+1].kind!=KIND_RESERVED && vmas[last+1].prot==vmas[i].prot) ++last;
    if (start) *start=(void *)vmas[first].start;
    if (end) *end=(void *)vmas[last].end;
    if (prot) *prot=(uint32_t)vmas[i].prot;
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t virtual_query(const void *address, int flags, VirtualQueryInfo *info, uint64_t info_size) {
    if (!info || info_size!=sizeof(*info)) return INVALID;
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || (vmas[i].start>(uintptr_t)address && !(flags & 1))) { read_unlock(); return ACCESS; }
    Vma v=vmas[i];
    memset(info,0,sizeof(*info));
    info->start=v.start; info->end=v.end;
    info->offset=v.kind==KIND_DIRECT ? v.phys : 0;
    info->protection=v.prot; info->memory_type=v.type;
    info->flags=(v.kind==KIND_FLEXIBLE ? 1u : 0) | (v.kind==KIND_DIRECT ? 2u : 0) | (v.kind!=KIND_RESERVED ? 16u : 0);
    strcpy(info->name, v.kind==KIND_RESERVED ? "reserved" : v.kind==KIND_DIRECT ? "direct" : "flexible");
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t direct_memory_type(uint64_t phys, int *type, void **start, void **end) {
    read_lock();
    for (int i=0;i<LIMIT;++i) if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) {
        if (type) *type=blocks[i].type;
        if (start) *start=(void *)(uintptr_t)blocks[i].start;
        if (end) *end=(void *)(uintptr_t)(blocks[i].start+blocks[i].size);
        read_unlock(); return 0;
    }
    read_unlock();
    return ACCESS;
}
static ABI int32_t batch_map2(BatchEntry *entries, int count, int *processed, int flags) {
    if (!entries || count<0) return INVALID;
    int32_t result=0; int done=0;
    write_lock();
    for (; done<count; ++done) {
        BatchEntry *e=&entries[done];
        switch (e->operation) {
        case 0: result=map_direct_locked(&e->start,e->length,(uint8_t)e->prot,flags,(int64_t)e->offset,0); break;
        case 1: result=unmap_locked((uintptr_t)e->start,e->length); break;
        case 2: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,-1); break;
        case 3: result=map_flexible_locked(&e->start,e->length,(uint8_t)e->prot,flags); break;
        case 4: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,e->type); break;
        default: result=INVALID;
        }
        if (result) break;
    }
    write_unlock();
    flush_hooks();
    if (processed) *processed=done;
    return result;
}
static ABI int32_t batch_map(BatchEntry *entries, int count, int *processed) {
    return batch_map2(entries,count,processed,MAP_FIXED_FLAG);
}
static const RuntimeExport exports[]={
    {"mmap",posix_mmap}, {"msync",posix_msync},
    {"sceKernelGetDirectMemorySize",direct_size}, {"sceKernelAllocateDirectMemory",direct_allocate},
    {"sceKernelMapDirectMemory",direct_map}, {"sceKernelMapNamedDirectMemory",direct_map_named},
    {"sceKernelReleaseDirectMemory",direct_release}, {"sceKernelMunmap",direct_unmap}, {"munmap",direct_unmap},
    {"sceKernelMapFlexibleMemory",map_flexible}, {"sceKernelMapNamedFlexibleMemory",map_flexible_named},
    {"sceKernelReleaseFlexibleMemory",release_flexible}, {"sceKernelReserveVirtualRange",reserve_range},
    {"sceKernelMprotect",kernel_mprotect}, {"sceKernelQueryMemoryProtection",query_protection},
    {"sceKernelVirtualQuery",virtual_query}, {"sceKernelGetDirectMemoryType",direct_memory_type},
    {"sceKernelBatchMap",batch_map}, {"sceKernelBatchMap2",batch_map2},
};
uintptr_t runtime_memory_resolve(const char *name) {
    /* Direct-memory NIDs are exercised by test_runtime.c before names are loaded. */
    if (!strcmp(name, "pO96TwzOm5E#libkernel")) return (uintptr_t)direct_size;
    if (!strcmp(name, "rTXw65xmLIA#libkernel")) return (uintptr_t)direct_allocate;
    if (!strcmp(name, "L-Q3LEjIbgA#libkernel")) return (uintptr_t)direct_map;
    if (!strcmp(name, "MBuItvba6z8#libkernel")) return (uintptr_t)direct_release;
    if (!strcmp(name, "cQke9UuBQOk#libkernel")) return (uintptr_t)direct_unmap;
    return RUNTIME_LOOKUP(exports,name);
}
/* Host-owned memory the guest can see (image, stacks, trampolines) lives in
 * [LOW_MIN, USER_MIN): PS4 code packs pointers into 40-bit fields. */
#define LOW_MIN UINT64_C(0x0800000000)
static uintptr_t low_next=LOW_MIN;
void *runtime_low_map(size_t size, int prot) {
    size=align_up(size,PAGE);
    write_lock();
#if   defined(__APPLE__)
    /* No MAP_FIXED_NOREPLACE: a hint is taken only when free, else the kernel returns the next
     * free range above it, where the search resumes. Stops below the hole (LOW_LIMIT). */
    void *p=MAP_FAILED;
    while (low_next+size<=LOW_LIMIT) {
        p=mmap((void *)low_next,size,prot,MAP_PRIVATE|MAP_ANON,-1,0);
        if (p==(void *)low_next) { low_next+=size+PAGE; break; } /* unmapped gap catches overruns */
        if (p!=MAP_FAILED) munmap(p,size);
        low_next = p!=MAP_FAILED && (uintptr_t)p>low_next ? align_up((uintptr_t)p,PAGE) : low_next+size+PAGE;
        p=MAP_FAILED;
    }
    write_unlock();
    return p==MAP_FAILED ? NULL : p;
#else
    void *p=MAP_FAILED;
    while (low_next+size<=USER_MIN) {
        p=mmap((void *)low_next,size,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        low_next+=size+PAGE; /* unmapped gap catches overruns */
        if (p!=MAP_FAILED) break;
    }
    write_unlock();
    return p==MAP_FAILED ? NULL : p;
#endif
}
/* ---- GPU library interface (gpu/shim/tlggpu.cpp) ---- */
/* Optimizations switched off at run time (diagnostics): the number in the file named by
 * TLG_TOGGLE_FILE, re-read every 250 ms. Bits: 1 region cache, 2 fetch shader cache,
 * 4 page tracking early exit, 8 pending-op poll limit, 16 threaded Vulkan recording,
 * 32 texture descriptor cache, 64 lock-free upload check, 128 image lookup cache,
 * 256 buffer uploads on the recording thread, 512 barrier tracker insert memo,
 * 1024 texture binding memo, 2048 page-granular read tracking for barriers,
 * 4096 resource sharps read by the draw-preparation workers,
 * 8192 prepared draws from the draw-preparation workers,
 * 16384 small read-only buffer copies on the recording thread,
 * 32768 hot pages (opt-in with TLG_HOT_PAGES=1),
 * 65536 unprotect the 256 KiB window around a guest write fault (TLG_FAULT_WINDOW KiB),
 * 131072 large guest memory copies split across copy threads (TLG_COPY_THREADS),
 * 262144 with TLG_ASYNC_FENCES=1: wait for guest copies at fences again,
 * 524288 small guest copies batched for the copy threads instead of the recording thread,
 * 1073741824 the lock-free UpdateImage path for clean, tracked images. */
/* Bits 32 and up: the draw pipeline and related GPU thread work (gpu/shim/tlg_toggles.h). */
uint64_t runtime_disabled_optimizations;
/* Speculative readers of guest memory (GPU draw-preparation workers) register a recovery
 * point: a fault on that thread jumps back to it instead of terminating (probe.c). */
__thread sigjmp_buf *runtime_fault_recover;
static void *toggle_watcher(void *path) {
    /* "Not read yet" is a separate flag, not a sentinel value: a sentinel makes the mask
     * that equals it (all bits set, every optimization disabled) impossible to apply. */
    unsigned long long last=0;
    int read_once=0;
    for (;;) {
        FILE *f=fopen(path,"r");
        unsigned long long value=0;
        if (f) { if (fscanf(f,"%llu",&value)!=1) value=0; fclose(f); }
        if (!read_once || value!=last) {
            read_once=1;
            __atomic_store_n(&runtime_disabled_optimizations,(uint64_t)value,__ATOMIC_RELEASE);
            printf("Runtime: disabled optimizations mask=%llu\n",value);
            last=value;
        }
        struct timespec t={0,250000000}; nanosleep(&t,NULL);
    }
    return NULL;
}
void runtime_memory_set_gpu_hooks(GpuRange map, GpuRange unmap, GpuRange invalidate) {
    const char *toggles=getenv("TLG_TOGGLE_FILE");
    static pthread_t watcher;
    if (toggles && !watcher) pthread_create(&watcher,NULL,toggle_watcher,(void *)toggles);
    write_lock();
    hook_map=map; hook_unmap=unmap; hook_invalidate=invalidate;
    for (size_t i=0;i<vma_count;++i) if (vmas[i].kind!=KIND_RESERVED) queue_hook(HOOK_MAP,vmas[i].start,vmas[i].end-vmas[i].start);
    write_unlock();
    flush_hooks();
}
/* Writes through the backing view; 0 when part of the range has no backing. */
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    read_lock();
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        memcpy(backing_base+vmas[i].phys+(at-vmas[i].start),(const unsigned char *)data+(at-address),n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* Fills through the backing view (one walk of the VMAs, where a memset goes one chunk at a
 * time); 0 when part of the range has no backing. */
int runtime_memory_fill_backing(uintptr_t address, int value, uint64_t size) {
    read_lock();
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        memset(backing_base+vmas[i].phys+(at-vmas[i].start),value,n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* Reads through the backing view, bypassing GPU page protection; 0 when part of the range has no
 * backing (the caller then reads the guest address directly). GPU-internal threads use it so a
 * read of a page protected for readback (TLG_READBACKS=2) cannot fault into a readback that waits
 * for the same thread (the Vulkan recorder). */
int runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size) {
    read_lock();
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        memcpy((unsigned char *)data+(at-address),backing_base+vmas[i].phys+(at-vmas[i].start),n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* tlg: TLG_UNIFIED_MEMORY: the backing view of [address,address+size) when it is direct memory
 * with contiguous physical pages (one mapping, possibly split by protections), else NULL. The
 * GPU imports it and reads and writes the guest's bytes in place. */
void *runtime_memory_backing_view(uintptr_t address, uint64_t size) {
    read_lock();
    unsigned char *view=NULL;
    uint64_t phys=0;
    int ok=size!=0;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind!=KIND_DIRECT) { ok=0; break; }
        uint64_t here=vmas[i].phys+(at-vmas[i].start);
        if (at==address) phys=here; else if (here!=phys+(at-address)) { ok=0; break; }
        at=vmas[i].end<end ? vmas[i].end : end;
    }
    if (ok) view=backing_base+phys;
    read_unlock();
    return view;
}
/* tlg: renderer v2's host interface (gpu_v2): direct mappings with their backing
 * view, the mapping table's generation, and a shared hold on the table while it publishes. */
void runtime_memory_visit(void (*visit)(uint64_t address, uint64_t size, uint8_t *backing)) {
    read_lock();
    size_t n=vma_count;
    Vma *copy=malloc((n ? n : 1)*sizeof(Vma));
    if (copy) memcpy(copy,vmas,n*sizeof(Vma));
    read_unlock();
    if (!copy) return;
    for (size_t i=0;i<n;++i)
        if (copy[i].kind==KIND_DIRECT) visit(copy[i].start,copy[i].end-copy[i].start,backing_base+copy[i].phys);
    free(copy);
}
uint64_t runtime_memory_epoch(void) { return __atomic_load_n(&table_generation,__ATOMIC_ACQUIRE); }
uint64_t runtime_memory_lock_shared(void) { read_lock(); return __atomic_load_n(&table_generation,__ATOMIC_ACQUIRE); }
void runtime_memory_unlock_shared(void) { read_unlock(); }
/* Per-thread cache of recently found regions for the GPU's per-draw queries:
 * entries hold for as long as the table generation they were read at. */
typedef struct { uint64_t generation; uintptr_t start, end; int kind; } CachedRegion;
static __thread CachedRegion region_cache[8];
static __thread unsigned region_cache_next;
static const CachedRegion *cached_region(uintptr_t address) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (generation & 1) return NULL;
    for (unsigned i=0;i<8;++i) {
        const CachedRegion *c=&region_cache[i];
        if (c->generation==generation && c->end && c->start<=address && address<c->end) return c;
    }
    return NULL;
}
/* Called under the read lock after a lookup of address. */
static void cache_region(size_t i) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (i==vma_count || (generation & 1)) return;
    region_cache[region_cache_next++ % 8]=(CachedRegion){generation,vmas[i].start,vmas[i].end,vmas[i].kind};
}
/* Mapped length from address, up to size, across adjacent mappings. */
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return size;
    read_lock();
    { size_t i=vma_index(address); if (i<vma_count && vmas[i].start<=address) cache_region(i); }
    uint64_t length=0;
    for (size_t i=vma_index(address); i<vma_count && length<size; ++i) {
        if (vmas[i].start>address+length || vmas[i].kind==KIND_RESERVED) break;
        length=vmas[i].end-address;
    }
    read_unlock();
    return length<size ? length : size;
}
/* Region around address: [start,end) mapped or a gap; 0 past the last mapping. */
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c) { *start=c->start; *end=c->end; *mapped=c->kind!=KIND_RESERVED; return 1; }
    read_lock();
    size_t i=vma_index(address);
    int found=i<vma_count;
    if (found && vmas[i].start<=address) {
        cache_region(i);
        *start=vmas[i].start; *end=vmas[i].end; *mapped=vmas[i].kind!=KIND_RESERVED;
    } else if (found) {
        *start=i ? vmas[i-1].end : 0; *end=vmas[i].start; *mapped=0;
    }
    read_unlock();
    return found;
}
/* GPU page tracking: restrict host access, never beyond the guest's own protection. */
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    read_lock();
    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size; ++i) {
        if (vmas[i].kind==KIND_RESERVED) continue;
        uintptr_t a=vmas[i].start>address ? vmas[i].start : address;
        uintptr_t b=vmas[i].end<address+size ? vmas[i].end : address+size;
        int guest=host_prot(vmas[i].prot);
        int want=(read ? PROT_READ : 0)|(write ? PROT_WRITE|PROT_READ : 0)|(guest & PROT_EXEC);
        host_protect(a,b-a,guest & want);
    }
    read_unlock();
}
/* Host view of a guest range, for the GPU backend: the range must be mapped. */
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return 1;
    read_lock();
    int result=covered(address,address+size,0);
    read_unlock();
    return result;
}
void runtime_memory_report(void) {
    printf("Runtime: direct memory allocations=%zu, maps=%zu, live=%" PRIu64 ", budget=%" PRIu64 "\n",
           allocations, maps, live_bytes, POOL_SIZE);
    printf("Runtime: flexible maps=%zu, in use=%" PRIu64 ", protects=%zu, queries=%zu, regions=%zu\n",
           flexible_maps, flexible_bytes, protects, queries, vma_count);
}


#ifdef __APPLE__
/* M3 (docs/macos/GPU_PROCESS_PLAN.md, as bloodborne_mac_port): the pool exists before the GPU
 * process starts (its memory entry is handed over), and every guest view change is mirrored. */
int runtime_memory_create_pool(void) {
    write_lock();
    int result=pool();
    write_unlock();
    return result;
}
void runtime_memory_set_mirror(RuntimeMemoryMirror fn) {
    write_lock();
    mirror=fn;
    if (fn) for (size_t i=0;i<vma_count;++i)
        if (vmas[i].kind!=KIND_RESERVED) fn(RUNTIME_MIRROR_MAP,vmas[i].start,vmas[i].end-vmas[i].start,vmas[i].phys,vmas[i].prot);
    write_unlock();
}
/* Checksum through the backing view (never protected), streamed across adjacent views. */
int runtime_memory_mirror_checksum(uintptr_t address, uint64_t size, uint64_t *out) {
    read_lock();
    uint64_t h=TLGGPU_CHECKSUM_SEED;
    int ok=backing_base!=NULL;
    for (uintptr_t at=address, end=address+size; ok && at<end;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        h=tlggpu_checksum(h,backing_base+vmas[i].phys+(at-vmas[i].start),n);
        at+=n;
    }
    read_unlock();
    if (ok) *out=h;
    return ok;
}
/* Mirror verification samples are not taken here yet (gpu_process.c treats none as nothing to
 * compare). */
void runtime_memory_mirror_snapshot(unsigned rotate, uint64_t max_sample, void (*emit)(const RuntimeMirrorSnapshot *)) {
    (void)rotate; (void)max_sample;
    RuntimeMirrorSnapshot s={0};
    read_lock();
    for (size_t i=0;i<vma_count;++i) if (vmas[i].kind!=KIND_RESERVED) { ++s.views; s.bytes+=vmas[i].end-vmas[i].start; }
    read_unlock();
    emit(&s);
}
#endif
