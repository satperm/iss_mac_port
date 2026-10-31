/* Freeze report: when the game has not shown a new frame for TLG_FREEZE_REPORT_SECONDS
 * (default 10, 0 = off) a monitor thread writes, once per freeze, every thread of the process to
 * the log: its name, what it is blocked on (the wait slots of runtime_thread.c, marked by the
 * runtime's and the GPU library's blocking calls), where it is now (RIP) and its frame chain,
 * in guest offsets inside the game image. The game keeps running; the next report needs frames
 * to resume and stop again.
 * Registers: Linux sends SIGURG to each thread (the handler only copies its registers and frame
 * chain); Windows suspends each thread for GetThreadContext; macOS suspends each Mach thread for
 * thread_get_state (signals to guest threads disturb the game there, as bloodborne_mac_port
 * found). Memory is read with process_vm_readv / ReadProcessMemory / mach_vm_read_overwrite, so a
 * bad frame pointer cannot fault. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/tlggpu.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

#define MAX_THREADS 1024
#define MAX_FRAMES 12

static uintptr_t image_base;
static uint64_t image_bytes;

/* Reads that fail instead of faulting (other threads' stacks, freed objects). */
static int safe_read(void *out, uintptr_t address, size_t size) {
    if (!address) return 0;
#if   defined(__APPLE__)
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size, (mach_vm_address_t)(uintptr_t)out, &got) ==
               KERN_SUCCESS && got == size;
#else
    struct iovec local = {out, size}, remote = {(void *)address, size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
#endif
}

/* Return addresses along a frame-pointer chain starting at frame (guest code keeps frame pointers). */
static unsigned walk_frames(uintptr_t frame, uintptr_t *out, unsigned max) {
    unsigned n = 0;
    while (n < max && frame) {
        uintptr_t pair[2];
        if (!safe_read(pair, frame, sizeof(pair)) || !pair[1]) break;
        out[n++] = pair[1];
        if (pair[0] <= frame || pair[0] - frame > (64u << 20)) break;
        frame = pair[0];
    }
    return n;
}

static void describe_address(char *out, size_t size, uintptr_t address) {
    if (image_base && address - image_base < image_bytes) {
        snprintf(out, size, "guest+0x%llx", (unsigned long long)(address - image_base));
        return;
    }
    Dl_info where;
    if (dladdr((void *)address, &where) && where.dli_fname) {
        const char *base = strrchr(where.dli_fname, '/');
        snprintf(out, size, "%s+0x%llx%s%s%s", base ? base + 1 : where.dli_fname,
                 (unsigned long long)(address - (uintptr_t)where.dli_fbase),
                 where.dli_sname ? " (" : "", where.dli_sname ? where.dli_sname : "", where.dli_sname ? ")" : "");
        return;
    }
    snprintf(out, size, "0x%llx", (unsigned long long)address);
}

/* ---- Text that grows (the whole report is written at once) ---- */
typedef struct { char *data; size_t length, capacity; } Text;
static void add(Text *t, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void add(Text *t, const char *format, ...) {
    for (;;) {
        va_list args;
        va_start(args, format);
        size_t room = t->capacity - t->length;
        int n = vsnprintf(t->data ? t->data + t->length : NULL, t->data ? room : 0, format, args);
        va_end(args);
        if (n < 0) return;
        if ((size_t)n < room) { t->length += (size_t)n; return; }
        size_t capacity = t->capacity ? t->capacity * 2 : 16384;
        while (capacity - t->length <= (size_t)n) capacity *= 2;
        char *grown = realloc(t->data, capacity);
        if (!grown) return;
        t->data = grown; t->capacity = capacity;
    }
}

/* ---- Wait description (unit-tested) ---- */
static const RuntimeThreadInfo *find_record(const RuntimeThreadInfo *threads, size_t count, const void *record) {
    for (size_t i = 0; i < count; ++i) if (threads[i].record && threads[i].record == record) return &threads[i];
    return NULL;
}
static void describe_level(Text *t, const RuntimeWaitLevel *l, uint64_t now_ms,
                           const RuntimeThreadInfo *threads, size_t count) {
    uint64_t ms = now_ms >= l->since_ms ? now_ms - l->since_ms : 0;
    add(t, "%s", l->kind);
    if (l->name[0]) add(t, " '%s'", l->name);
    if (!strncmp(l->kind, "sleep", 5)) add(t, " of %llu us", (unsigned long long)(uintptr_t)l->object);
    else if (!strncmp(l->kind, "file", 4) && (uintptr_t)l->object < 0x10000) add(t, " (fd %u)", (unsigned)(uintptr_t)l->object);
    else if (!strncmp(l->kind, "semaphore", 9)) add(t, " id %llu", (unsigned long long)(uintptr_t)l->object);
    else if (l->object) add(t, " 0x%llx", (unsigned long long)(uintptr_t)l->object);
    add(t, " for %llu ms", (unsigned long long)ms);
    if (l->owner) {
        const void *holder = NULL;
        if (!safe_read(&holder, (uintptr_t)l->owner, sizeof(holder))) add(t, ", holder unknown");
        else if (!holder) add(t, ", not held now");
        else if (holder == (const void *)1) add(t, ", held by a host thread");
        else {
            const RuntimeThreadInfo *h = find_record(threads, count, holder);
            if (h) add(t, ", held by '%s' (tid %llu)", h->name, (unsigned long long)h->os_tid);
            else add(t, ", held by thread record 0x%llx", (unsigned long long)(uintptr_t)holder);
        }
    }
}
static void describe_wait(Text *t, const RuntimeWait *w, uint64_t now_ms, const RuntimeThreadInfo *threads, size_t count) {
    unsigned depth = w->depth < RUNTIME_WAIT_LEVELS ? w->depth : RUNTIME_WAIT_LEVELS;
    int shown = 0;
    /* Innermost first: what the thread is blocked on right now, then what it was doing. */
    for (unsigned i = depth; i-- > 0;) {
        const RuntimeWaitLevel *l = &w->level[i];
        if (!l->kind) continue;
        add(t, shown ? " (inside " : "waiting on ");
        describe_level(t, l, now_ms, threads, count);
        if (shown) add(t, ")");
        shown = 1;
    }
    if (!shown) add(t, "running (not in a marked wait)");
    if (w->depth > RUNTIME_WAIT_LEVELS) add(t, " (+%u nested waits)", w->depth - RUNTIME_WAIT_LEVELS);
}
void runtime_freeze_describe_wait(char *out, size_t size, const RuntimeWait *wait, uint64_t now_ms,
                                  const RuntimeThreadInfo *threads, size_t count) {
    Text t = {0};
    describe_wait(&t, wait, now_ms, threads, count);
    snprintf(out, size, "%s", t.data ? t.data : "");
    free(t.data);
}

int runtime_freeze_step(FreezeMonitor *m, uint64_t frames, uint64_t now_ms) {
    if (frames != m->last_frames) {
        const int resumed = m->reported;
        m->freeze_ms = now_ms - m->last_change_ms;
        m->last_frames = frames; m->last_change_ms = now_ms; m->reported = 0;
        return resumed ? 2 : 0;
    }
    /* Armed by the first frame: loading before it is not a freeze. */
    if (!frames || !m->seconds || m->reported) return 0;
    if (now_ms - m->last_change_ms < m->seconds * 1000) return 0;
    m->reported = 1;
    return 1;
}

/* ---- Registers of every thread ---- */
typedef struct {
    uint64_t tid;
    int captured;
    uintptr_t rip, rsp, rbp;
    unsigned frames;
    uintptr_t chain[MAX_FRAMES];
    char name[64];
} Capture;

static uint64_t own_tid(void) {
    return (uint64_t)gettid();
}

#ifdef __APPLE__
/* The Mach thread id (THREAD_IDENTIFIER_INFO), the same number as gettid() in compat_darwin.h. */
static uint64_t mach_thread_id(thread_act_t thread) {
    thread_identifier_info_data_t id;
    mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
    return thread_info(thread, THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &count) == KERN_SUCCESS ? id.thread_id : 0;
}
static size_t list_threads(Capture *out, size_t max) {
    thread_act_array_t list = NULL;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return 0;
    size_t n = 0;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        if (n < max) {
            out[n] = (Capture){.tid = mach_thread_id(list[i])};
            pthread_t thread = pthread_from_mach_thread_np(list[i]);
            if (thread) pthread_getname_np(thread, out[n].name, sizeof(out[n].name));
            if (out[n].tid) ++n;
        }
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(*list));
    return n;
}
/* runtime_thread.c's thread_alive: any thread of this task with that Mach thread id. Only runs when
 * every wait slot is taken. (Here because <mach/mach.h> declares a thread_create of its own.) */
int runtime_darwin_thread_alive(uint64_t tid) {
    thread_act_array_t list = NULL;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return 1; /* pessimistic */
    int alive = 0;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        if (!alive && mach_thread_id(list[i]) == tid) alive = 1;
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(*list));
    return alive;
}
/* Suspended only for thread_get_state and the frame reads (no locks are taken meanwhile). */
static void capture(Capture *c) {
    thread_act_array_t list = NULL;
    mach_msg_type_number_t count = 0;
    if (c->tid == own_tid() || task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        if (!c->captured && mach_thread_id(list[i]) == c->tid && thread_suspend(list[i]) == KERN_SUCCESS) {
            x86_thread_state64_t state;
            mach_msg_type_number_t words = x86_THREAD_STATE64_COUNT;
            if (thread_get_state(list[i], x86_THREAD_STATE64, (thread_state_t)&state, &words) == KERN_SUCCESS) {
                c->rip = (uintptr_t)state.__rip; c->rsp = (uintptr_t)state.__rsp; c->rbp = (uintptr_t)state.__rbp;
                c->frames = walk_frames(c->rbp, c->chain, MAX_FRAMES);
                c->captured = 1;
            }
            thread_resume(list[i]);
        }
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(*list));
}
#else
static pid_t capture_tid;
static Capture *capture_out;
static int capture_done;
/* Async-signal-safe: registers and the frame chain into the capture the monitor waits on. */
static void capture_signal(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    int saved = errno;
    if (__atomic_load_n(&capture_tid, __ATOMIC_ACQUIRE) == gettid()) {
        Capture *c = __atomic_load_n(&capture_out, __ATOMIC_ACQUIRE);
        const greg_t *g = ((ucontext_t *)context)->uc_mcontext.gregs;
        c->rip = (uintptr_t)g[REG_RIP]; c->rsp = (uintptr_t)g[REG_RSP]; c->rbp = (uintptr_t)g[REG_RBP];
        c->frames = walk_frames(c->rbp, c->chain, MAX_FRAMES);
        c->captured = 1;
        __atomic_store_n(&capture_done, 1, __ATOMIC_RELEASE);
    }
    errno = saved;
}
static size_t list_threads(Capture *out, size_t max) {
    size_t n = 0;
    DIR *dir = opendir("/proc/self/task");
    struct dirent *e;
    while (dir && n < max && (e = readdir(dir))) {
        long tid = strtol(e->d_name, NULL, 10);
        if (tid <= 0) continue;
        out[n] = (Capture){.tid = (uint64_t)tid};
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/task/%ld/comm", tid);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            ssize_t got = read(fd, out[n].name, sizeof(out[n].name) - 1);
            close(fd);
            if (got > 0) out[n].name[got - (out[n].name[got - 1] == '\n')] = 0;
        }
        ++n;
    }
    if (dir) closedir(dir);
    return n;
}
static void capture(Capture *c) {
    __atomic_store_n(&capture_done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&capture_out, c, __ATOMIC_RELEASE);
    __atomic_store_n(&capture_tid, (pid_t)c->tid, __ATOMIC_RELEASE);
    if (!syscall(SYS_tgkill, getpid(), (pid_t)c->tid, SIGURG)) {
        /* A thread that blocks SIGURG never answers: 30 ms each at most. */
        for (int i = 0; i < 60 && !__atomic_load_n(&capture_done, __ATOMIC_ACQUIRE); ++i) {
            struct timespec nap = {0, 500000};
            nanosleep(&nap, NULL);
        }
    }
    __atomic_store_n(&capture_tid, 0, __ATOMIC_RELEASE);
}
#endif

static void add_chain(Text *t, const uintptr_t *chain, unsigned frames) {
    char where[320];
    for (unsigned i = 0; i < frames; ++i) {
        describe_address(where, sizeof(where), chain[i]);
        add(t, "%s%s", i ? " < " : "", where);
    }
}
static void add_thread(Text *t, const Capture *c, const RuntimeThreadInfo *info, uint64_t now_ms,
                       const RuntimeThreadInfo *threads, size_t count) {
    char where[320];
    if (info && info->record) add(t, "  guest '%s' (tid %llu): ", info->name, (unsigned long long)c->tid);
    else add(t, "  host '%s' (tid %llu): ", c->name[0] ? c->name : "?", (unsigned long long)c->tid);
    if (info) describe_wait(t, &info->wait, now_ms, threads, count);
    else add(t, "no marked wait");
    add(t, "\n");
    if (c->captured) {
        describe_address(where, sizeof(where), c->rip);
        add(t, "      now at %s, rsp=0x%llx", where, (unsigned long long)c->rsp);
        if (c->frames) { add(t, "; frames: "); add_chain(t, c->chain, c->frames); }
        add(t, "\n");
    } else {
        add(t, "      registers not captured (the thread did not answer)\n");
    }
    /* The guest frames that called the marked wait (the import's own frame starts the chain). */
    if (info && info->wait.depth && info->wait.level[0].kind && info->wait.level[0].frame) {
        uintptr_t chain[MAX_FRAMES];
        unsigned frames = walk_frames(info->wait.level[0].frame, chain, MAX_FRAMES);
        if (frames) { add(t, "      wait called from "); add_chain(t, chain, frames); add(t, "\n"); }
    }
}

static TlgGpuFrameCounters counters_at_last_frame;
static void add_counters(Text *t) {
    TlgGpuFrameCounters now;
    tlggpu_frame_counters(&now);
    const TlgGpuFrameCounters *then = &counters_at_last_frame;
    add(t, "  frames: %llu flips submitted by the game (+%llu since the last shown frame), %llu shown, "
           "%llu presented (+%llu), %llu vblank ticks (+%llu); page faults being handled now: %llu (%llu handled, +%llu)\n",
        (unsigned long long)now.submitted, (unsigned long long)(now.submitted - then->submitted),
        (unsigned long long)now.shown, (unsigned long long)now.presented,
        (unsigned long long)(now.presented - then->presented), (unsigned long long)now.vblanks,
        (unsigned long long)(now.vblanks - then->vblanks), (unsigned long long)now.faults_in_progress,
        (unsigned long long)now.faults_handled, (unsigned long long)(now.faults_handled - then->faults_handled));
}

static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
void runtime_freeze_report(FILE *out, const char *head) {
    pthread_mutex_lock(&report_lock);
    Capture *caps = calloc(MAX_THREADS, sizeof(*caps));
    RuntimeThreadInfo *infos = calloc(MAX_THREADS, sizeof(*infos));
    Text t = {0};
    if (!caps || !infos) { free(caps); free(infos); pthread_mutex_unlock(&report_lock); return; }
    const size_t threads = list_threads(caps, MAX_THREADS);
    const uint64_t self = own_tid();
    for (size_t i = 0; i < threads; ++i) if (caps[i].tid != self) capture(&caps[i]);
    /* Wait slots after the registers: the waits shown are at least as old as the RIPs. */
    const uint64_t now_ms = runtime_wait_now_ms();
    const size_t records = runtime_thread_snapshot(infos, MAX_THREADS);
    add(&t, "%s", head);
    add_counters(&t);
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < threads; ++i) {
            if (caps[i].tid == self) continue;
            /* A live guest record with this OS thread id, else a host wait slot (ids are reused). */
            const RuntimeThreadInfo *info = NULL;
            for (size_t r = 0; r < records && !info; ++r)
                if (infos[r].record && !infos[r].finished && infos[r].os_tid == caps[i].tid) info = &infos[r];
            for (size_t r = 0; r < records && !info; ++r)
                if (!infos[r].record && infos[r].os_tid == caps[i].tid) info = &infos[r];
            const int guest = info && info->record;
            if (guest == (pass == 0)) add_thread(&t, &caps[i], info, now_ms, infos, records);
        }
    }
    add(&t, "FREEZE: end of report (%zu threads; the game keeps running)\n", threads);
    if (t.data) { fwrite(t.data, 1, t.length, out); fflush(out); }
    free(t.data); free(caps); free(infos);
    pthread_mutex_unlock(&report_lock);
}

/* ---- Monitor ---- */
/* ---- Thread profile (TLG_THREAD_PROFILE_MS=<n>, macOS) ----
 * Every n ms each thread of the process is suspended for its registers (as the freeze report);
 * every 10 s a summary: per thread, the share of samples in a marked wait (by object) or running,
 * running ones by their innermost guest code location (rip, or the first guest return address
 * when it is in a host library). Shows where the game's threads spend a frame without signals. */
#ifdef __APPLE__
static void sleep_ms(unsigned ms);
#define PROFILE_KEYS 4096
typedef struct { char thread[32]; char where[120]; uint32_t count; } ProfileKey;
static ProfileKey *profile_keys;
static uint32_t profile_samples;
static void profile_count(const char *thread, const char *where) {
    uint64_t h = 1469598103934665603ull;
    for (const char *p = thread; *p; ++p) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    for (const char *p = where; *p; ++p) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    for (uint32_t i = 0; i < PROFILE_KEYS; ++i) {
        ProfileKey *k = &profile_keys[(h + i) % PROFILE_KEYS];
        if (!k->count) {
            snprintf(k->thread, sizeof(k->thread), "%s", thread);
            snprintf(k->where, sizeof(k->where), "%s", where);
        } else if (strcmp(k->thread, thread) || strcmp(k->where, where)) {
            continue;
        }
        ++k->count;
        return;
    }
}
static int profile_by_count(const void *a, const void *b) {
    const ProfileKey *x = a, *y = b;
    int t = strcmp(x->thread, y->thread);
    if (t) return t;
    return (int)y->count - (int)x->count;
}
static void profile_report(void) {
    qsort(profile_keys, PROFILE_KEYS, sizeof(*profile_keys), profile_by_count);
    printf("Thread profile (%u samples a thread over the last 10 s, TLG_THREAD_PROFILE_MS):\n", profile_samples);
    const char *current = NULL;
    uint32_t shown = 0, total = 0;
    for (uint32_t i = 0; i < PROFILE_KEYS; ++i) {
        const ProfileKey *k = &profile_keys[i];
        if (!k->count) continue;
        if (!current || strcmp(current, k->thread)) {
            total = 0;
            for (uint32_t j = i; j < PROFILE_KEYS && profile_keys[j].count && !strcmp(profile_keys[j].thread, k->thread); ++j)
                total += profile_keys[j].count;
            current = k->thread;
            shown = 0;
            if (total * 20 < profile_samples) { /* idle-ish threads (under 5% of samples) skipped */ }
        }
        if (shown < 6 && k->count * 50 >= profile_samples) {
            printf("  %-24s %5.1f%% %s\n", k->thread, 100.0 * k->count / profile_samples, k->where);
            ++shown;
        }
    }
    fflush(stdout);
    memset(profile_keys, 0, PROFILE_KEYS * sizeof(*profile_keys));
    profile_samples = 0;
}
static void *profiler(void *argument) {
    const unsigned period = (unsigned)(uintptr_t)argument;
    pthread_setname_np("tlg:profile");
    Capture *caps = calloc(MAX_THREADS, sizeof(*caps));
    RuntimeThreadInfo *infos = calloc(MAX_THREADS, sizeof(*infos));
    profile_keys = calloc(PROFILE_KEYS, sizeof(*profile_keys));
    if (!caps || !infos || !profile_keys) return NULL;
    const uint64_t self = own_tid();
    uint64_t last_report = runtime_wait_now_ms();
    for (;;) {
        sleep_ms(period);
        thread_act_array_t list = NULL;
        mach_msg_type_number_t count = 0;
        if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) continue;
        size_t n = 0;
        for (mach_msg_type_number_t i = 0; i < count; ++i) {
            const uint64_t tid = mach_thread_id(list[i]);
            if (tid && tid != self && n < MAX_THREADS && thread_suspend(list[i]) == KERN_SUCCESS) {
                x86_thread_state64_t state;
                mach_msg_type_number_t words = x86_THREAD_STATE64_COUNT;
                Capture *c = &caps[n];
                memset(c, 0, sizeof(*c));
                c->tid = tid;
                if (thread_get_state(list[i], x86_THREAD_STATE64, (thread_state_t)&state, &words) == KERN_SUCCESS) {
                    c->rip = (uintptr_t)state.__rip;
                    c->frames = walk_frames((uintptr_t)state.__rbp, c->chain, MAX_FRAMES);
                    c->captured = 1;
                }
                thread_resume(list[i]);
                pthread_t thread = pthread_from_mach_thread_np(list[i]);
                if (thread) pthread_getname_np(thread, c->name, sizeof(c->name));
                ++n;
            }
            mach_port_deallocate(mach_task_self(), list[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(*list));
        const size_t records = runtime_thread_snapshot(infos, MAX_THREADS);
        for (size_t i = 0; i < n; ++i) {
            const Capture *c = &caps[i];
            if (!c->captured) continue;
            const RuntimeThreadInfo *info = NULL;
            for (size_t r = 0; r < records && !info; ++r)
                if (infos[r].os_tid == c->tid && !infos[r].finished) info = &infos[r];
            char thread[32], where[120];
            snprintf(thread, sizeof(thread), "%s", info && info->name[0] ? info->name : c->name[0] ? c->name : "?");
            const RuntimeWait *w = info ? &info->wait : NULL;
            const unsigned depth = w ? (w->depth < RUNTIME_WAIT_LEVELS ? w->depth : RUNTIME_WAIT_LEVELS) : 0;
            if (depth && w->level[depth - 1].kind) {
                snprintf(where, sizeof(where), "wait %s '%s'", w->level[depth - 1].kind, w->level[depth - 1].name);
            } else {
                uintptr_t pc = c->rip;
                char at[100], via[100] = "";
                describe_address(at, sizeof(at), pc);
                if (!(image_base && pc - image_base < image_bytes)) {
                    for (unsigned f = 0; f < c->frames; ++f) {
                        if (image_base && c->chain[f] - image_base < image_bytes) {
                            describe_address(via, sizeof(via), c->chain[f]);
                            break;
                        }
                    }
                }
                snprintf(where, sizeof(where), "%s%s%s", at, via[0] ? " < " : "", via);
            }
            profile_count(thread, where);
        }
        ++profile_samples;
        const uint64_t now = runtime_wait_now_ms();
        if (now - last_report >= 10000) {
            profile_report();
            last_report = now;
        }
    }
    return NULL;
}
#endif


static void sleep_ms(unsigned ms) {
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
static void *monitor(void *argument) {
    FreezeMonitor *m = argument;
#ifdef __APPLE__
    pthread_setname_np("tlg:freeze");
#else
    pthread_setname_np(pthread_self(), "tlg:freeze");
#endif
    const unsigned period = m->seconds >= 4 ? 1000 : (unsigned)m->seconds * 250;
    for (;;) {
        sleep_ms(period);
        TlgGpuFrameCounters now;
        tlggpu_frame_counters(&now);
        const uint64_t ms = runtime_wait_now_ms();
        const uint64_t before = m->last_frames;
        switch (runtime_freeze_step(m, now.shown, ms)) {
        case 1: {
            char head[96];
            snprintf(head, sizeof(head), "FREEZE: no frame for %u s; threads:\n", (unsigned)m->seconds);
            runtime_freeze_report(stdout, head);
            break;
        }
        case 2:
            printf("FREEZE: frames resumed after %.1f s\n", m->freeze_ms / 1000.0);
            fflush(stdout);
            break;
        }
        if (now.shown != before) counters_at_last_frame = now;
    }
    return NULL;
}

static void gpu_wait_begin(const char *kind, const void *object, const char *name, uintptr_t frame) {
    runtime_wait_begin(kind, object, name, NULL, frame);
}
void runtime_freeze_start(uintptr_t image, uint64_t image_size, unsigned seconds) {
    image_base = image; image_bytes = image_size;
    tlggpu_set_wait_hooks(gpu_wait_begin, runtime_wait_end);
#if !defined(__APPLE__)
    struct sigaction action = {0};
    action.sa_sigaction = capture_signal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGURG, &action, NULL);
#endif
#ifdef __APPLE__
    if (getenv("TLG_THREAD_PROFILE_MS") && atoi(getenv("TLG_THREAD_PROFILE_MS")) > 0) {
        pthread_t profile_thread;
        if (!pthread_create(&profile_thread, NULL, profiler,
                            (void *)(uintptr_t)atoi(getenv("TLG_THREAD_PROFILE_MS"))))
            pthread_detach(profile_thread);
    }
#endif
    if (!seconds) return;
    static FreezeMonitor state;
    state = (FreezeMonitor){.seconds = seconds};
    pthread_t thread;
    if (pthread_create(&thread, NULL, monitor, &state)) { puts("Freeze report: monitor thread not started"); return; }
    pthread_detach(thread);
    printf("Freeze report: after %u s without a new frame (TLG_FREEZE_REPORT_SECONDS, 0 = off)\n", seconds);
}
