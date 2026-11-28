/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "runtime.h"
#ifdef __APPLE__
#include "gpu_process.h"
#endif
#include "cpu_check.h"
#include "isa_rewrite.h"
#include "orb_hook.h"
#include "cheat_hook.h"
#include "actor_hook.h"
#include "damage_hook.h"
#include "view_hook.h"
#include "iss_temporal_hook.h"
#include "iss_camera_hook.h"
#include "iss_mouse_aim.h"
#include "iss_camera_pitch_hook.h"
#include "gpu/tlggpu.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/uio.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <CoreFoundation/CoreFoundation.h>
#undef round_page /* <mach/vm_param.h> macros; probe.c has its own */
#undef trunc_page
/* Signal-context registers by name: __ss.__rip etc. on macOS, gregs[REG_RIP] etc. on Linux. */
#define UC_REG(uc, r) ((uc)->uc_mcontext->__ss.__##r)
/* process_vm_readv on this process, one range (all this file uses): Mach reads that fail instead
 * of faulting, as Linux's. */
static ssize_t process_vm_readv(pid_t pid, const struct iovec *local, unsigned long local_count,
                                const struct iovec *remote, unsigned long remote_count, unsigned long flags) {
    (void)pid; (void)local_count; (void)remote_count; (void)flags;
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)remote->iov_base, remote->iov_len,
                               (mach_vm_address_t)(uintptr_t)local->iov_base, &got) != KERN_SUCCESS) return -1;
    return (ssize_t)got;
}
#else
#include <malloc.h>
#include <ucontext.h>
#include <sys/syscall.h>
#define UC_REG(uc, r) ((uc)->uc_mcontext.gregs[UC_##r])
#define UC_rip REG_RIP
#define UC_rsp REG_RSP
#define UC_rbp REG_RBP
#define UC_rax REG_RAX
#define UC_rbx REG_RBX
#define UC_rcx REG_RCX
#define UC_rdx REG_RDX
#define UC_rsi REG_RSI
#define UC_rdi REG_RDI
#define UC_rflags REG_EFL
#endif
/* Mach-O prefixes C symbols with '_': top-level asm must name them through SYM(). */
#ifdef __APPLE__
#define SYM(name) "_" #name
#else
#define SYM(name) #name
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
static unsigned cpu_missing_sets; /* CPU_SET_* the host lacks; the guest code needs them rewritten */
static IsaTable *isa_table; /* rewritten sites (--isa-rewrite); the fault handlers and the report read it */
static uintptr_t guest_code_end; /* end of the image and its stub areas (emulation, red zone); TLG_CLOBBER_RED_ZONE */
static void isa_report_hook(void) { isa_rewrite_report(isa_table, stdout); }
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",(uintptr_t)(caller-modules[m].base));
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
/* TLG_IMPORT_TRACE=<n>: each import the host runtime resolves goes through a stub that logs the
 * call (name, first four integer arguments, caller offset) for its first n calls, then jumps to
 * the implementation. Shows which system call precedes a guest fault during bring-up. */
static unsigned char *trace_stubs;
static uintptr_t *trace_made;
static uint32_t *trace_counts, trace_limit;
#define TRACE_STUB_SIZE 192
static ABI void trace_import(uint32_t index, const uint64_t *regs) {
    /* regs: rax, r9, r8, rcx, rdx, rsi, rdi, return address */
    if (__atomic_fetch_add(&trace_counts[index], 1, __ATOMIC_RELAXED) >= trace_limit) return;
    fprintf(stderr, "IMPORT %s(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ") from +%#" PRIxPTR "\n",
            runtime_import_name(names[index]), regs[6], regs[5], regs[4], regs[3], (uintptr_t)regs[7] - (uintptr_t)image);
}
static uintptr_t trace_stub(uint64_t index, uintptr_t target) {
    if (trace_made[index]) return trace_made[index];
    unsigned char *t = trace_stubs + index * TRACE_STUB_SIZE, *p = t;
    static const unsigned char save[] = {0x57,0x56,0x52,0x51,0x41,0x50,0x41,0x51,0x50, 0x48,0x81,0xec,0x80,0,0,0};
    memcpy(p, save, sizeof(save)); p += sizeof(save);
    for (int x = 0; x < 8; ++x) { unsigned char m[] = {0xf3,0x0f,0x7f,(unsigned char)(0x44|(x<<3)),0x24,(unsigned char)(x*16)}; memcpy(p, m, 6); p += 6; }
    uint32_t id = (uint32_t)index;
    *p++ = 0xbf; memcpy(p, &id, 4); p += 4;
    static const unsigned char lea[] = {0x48,0x8d,0xb4,0x24,0x80,0,0,0};
    memcpy(p, lea, sizeof(lea)); p += sizeof(lea);
    uintptr_t logger = (uintptr_t)trace_import;
    *p++ = 0x48; *p++ = 0xb8; memcpy(p, &logger, 8); p += 8;
    *p++ = 0xff; *p++ = 0xd0;
    for (int x = 0; x < 8; ++x) { unsigned char m[] = {0xf3,0x0f,0x6f,(unsigned char)(0x44|(x<<3)),0x24,(unsigned char)(x*16)}; memcpy(p, m, 6); p += 6; }
    static const unsigned char restore[] = {0x48,0x81,0xc4,0x80,0,0,0, 0x58,0x41,0x59,0x41,0x58,0x59,0x5a,0x5e,0x5f};
    memcpy(p, restore, sizeof(restore)); p += sizeof(restore);
    *p++ = 0x49; *p++ = 0xbb; memcpy(p, &target, 8); p += 8;
    *p++ = 0x41; *p++ = 0xff; *p++ = 0xe3;
    if (p - t > TRACE_STUB_SIZE) fail("trace stub too large");
    return trace_made[index] = (uintptr_t)t;
}
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl " SYM(enter_on_stack) "\n" SYM(enter_on_stack) ":\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
/* IsaRegs.gpr (x86 numbering: rax rcx rdx rbx rsp rbp rsi rdi r8..r15) in the signal context. */
#ifdef __APPLE__
static uint64_t *uc_gpr(ucontext_t *uc, int i) {
    _STRUCT_X86_THREAD_STATE64 *s = &uc->uc_mcontext->__ss;
    uint64_t *regs[16] = {&s->__rax, &s->__rcx, &s->__rdx, &s->__rbx, &s->__rsp, &s->__rbp, &s->__rsi, &s->__rdi,
                          &s->__r8, &s->__r9, &s->__r10, &s->__r11, &s->__r12, &s->__r13, &s->__r14, &s->__r15};
    return regs[i];
}
/* XMM0 of the signal frame: the float state follows the FXSAVE layout (XMM0 at its byte 160). */
static unsigned char *uc_xmm(ucontext_t *uc) { return (unsigned char *)&uc->uc_mcontext->__fs.__fpu_xmm0; }
#else
static const int isa_gregs[16]={REG_RAX,REG_RCX,REG_RDX,REG_RBX,REG_RSP,REG_RBP,REG_RSI,REG_RDI,
                                REG_R8,REG_R9,REG_R10,REG_R11,REG_R12,REG_R13,REG_R14,REG_R15};
static uint64_t *uc_gpr(ucontext_t *uc, int i) { return (uint64_t *)&uc->uc_mcontext.gregs[isa_gregs[i]]; }
#endif
/* TLG_CLOBBER_RED_ZONE=<bytes> (test, Linux): after each guest fault the handler resolved (GPU page
 * tracking, int3 ISA sites), zero that many bytes below the interrupted rsp, as Windows' exception
 * dispatch overwrites them: reproduces the Windows-only corruption here ("1" = the 128-byte red
 * zone). Over 128 bytes the handler needs an alternate signal stack (the kernel's frame starts 128
 * bytes below rsp): each guest thread gets one; elsewhere the clobber stays at 128. The first
 * distinct guest RIPs are logged. */
static unsigned clobber_red_zone;
static void clobber_thread_init(void) {
    if (clobber_red_zone <= 128) return;
    stack_t ss = {0};
    ss.ss_size = 256 * 1024;
    ss.ss_sp = mmap(NULL, ss.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ss.ss_sp != MAP_FAILED) sigaltstack(&ss, NULL);
}
static void clobber_below(ucontext_t *uc) {
    uintptr_t rip = (uintptr_t)UC_REG(uc, rip), rsp = (uintptr_t)UC_REG(uc, rsp);
    if (!clobber_red_zone || rip < (uintptr_t)image || rip >= guest_code_end) return;
    unsigned bytes = clobber_red_zone;
    stack_t current;
    if (bytes > 128 && (sigaltstack(NULL, &current) || !(current.ss_flags & SS_ONSTACK))) bytes = 128;
    memset((void *)(rsp - bytes), 0, bytes);
    static uintptr_t seen[32];
    static unsigned seen_count;
    for (unsigned i = 0; i < seen_count && i < 32; ++i) if (seen[i] == rip) return;
    unsigned slot = __atomic_fetch_add(&seen_count, 1, __ATOMIC_RELAXED);
    if (slot >= 32) return;
    seen[slot] = rip;
    char line[96];
    uint64_t site = 0;
    int in_stub = isa_red_zone_site_of_stub(isa_table, rip, &site);
    int n = snprintf(line, sizeof line, "Test: red zone clobbered (%u bytes) at guest offset 0x%lx%s%lx\n", bytes,
                     (unsigned long)(rip - (uintptr_t)image), in_stub ? ", red-zone stub of 0x" : "", in_stub ? (unsigned long)site : 0ul);
    if (!in_stub) n = snprintf(line, sizeof line, "Test: red zone clobbered (%u bytes) at guest offset 0x%lx\n", bytes,
                               (unsigned long)(rip - (uintptr_t)image));
    if (n > 0) { ssize_t written_ = write(2, line, (size_t)n); (void)written_; }
}
static void fault(int sig, siginfo_t *info, void *context) {
    if (sig == SIGTRAP && isa_table) {
        /* An int3 ISA site (RIP is past the CC byte); any other trap ends in the report below. */
        ucontext_t *uc = context;
        IsaRegs regs;
        uintptr_t rip;
        for (int i = 0; i < 16; ++i) regs.gpr[i] = *uc_gpr(uc, i);
        regs.rflags = (uint64_t)UC_REG(uc, rflags);
        /* XMM0-15 of the signal frame (FXSAVE layout at 160): an SSE4A site changes one of them,
         * and the kernel restores them on return. */
#ifdef __APPLE__
        unsigned char *xmm = uc_xmm(uc);
#else
        unsigned char *fpu = (unsigned char *)uc->uc_mcontext.fpregs;
        unsigned char *xmm = fpu ? fpu + 160 : NULL;
#endif
        if (isa_rewrite_trap(isa_table, (uintptr_t)UC_REG(uc, rip) - 1, &regs, xmm, &rip)) {
#ifndef __APPLE__
            /* XSAVE frame (FP_XSTATE_MAGIC1): XSTATE_BV bit 1 (SSE) set, or XRSTOR would reset the
             * XMM registers instead of loading the changed ones. XSAVE wrote their real values. */
            uint32_t magic;
            if (fpu && (memcpy(&magic, fpu + 464, 4), magic == 0x46505853u)) fpu[512] |= 2;
#endif
            for (int i = 0; i < 16; ++i) *uc_gpr(uc, i) = regs.gpr[i];
            UC_REG(uc, rflags) = regs.rflags;
            UC_REG(uc, rip) = rip;
            clobber_below(context);
            return;
        }
    }
    /* A speculative guest memory read (GPU draw-preparation workers) failed: resume its recovery
     * point before any readback. With precise readbacks the page may be read-protected because
     * the GPU wrote it; flushing it from a GPU worker waits on that worker's own pipeline
     * (deadlock), while giving up leaves the draw to the GPU thread, which reads it safely. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* GPU page tracking (write-protected guest pages) is resolved next. */
    /* macOS reports a write to a mapped read-only page as SIGBUS, not SIGSEGV. */
#ifdef __APPLE__
    if (gpu_enabled && (sig == SIGSEGV || sig == SIGBUS) && tlggpu_handle_fault(context, info->si_addr)) {
#else
    if (gpu_enabled && sig == SIGSEGV && tlggpu_handle_fault(context, info->si_addr)) {
#endif
        clobber_below(context);
        return;
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)UC_REG(uc, rip);
    char line[512];
    Dl_info where;
    int site = isa_table ? isa_rewrite_site_of_stub(isa_table, rip) : -1;
    char emulating[64] = "";
    uint64_t rz_site = 0;
    if (site >= 0)
        snprintf(emulating, sizeof(emulating), " While emulating site 0x%" PRIx64 " (%s)",
                 isa_table->sites[site].offset, isa_kind_name(isa_table->sites[site].kind));
    else if (isa_red_zone_site_of_stub(isa_table, rip, &rz_site))
        snprintf(emulating, sizeof(emulating), " (red-zone stub of 0x%" PRIx64 ")", rz_site);
    if (site >= 0 || rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p%s\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr, emulating);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (gpu_enabled) tlggpu_dump_guest_writes(context);
    /* Engine ASSERT (int 0x41): the formatted "file(line): func() ASSERT(cond)" message sits in a
     * stack buffer of the asserting frame; its logger may be null, so print it from here. */
    if (rip - (uintptr_t)image < 0x10000000) {
        unsigned char op[2] = {0};
        struct iovec lo = {op, 2}, ro = {(void *)rip, 2};
        if (process_vm_readv(getpid(), &lo, 1, &ro, 1, 0) == 2 && op[0] == 0xcd && op[1] == 0x41) {
            static char stack[0x4000];
            uintptr_t rsp = (uintptr_t)UC_REG(uc, rsp);
            struct iovec ls = {stack, sizeof(stack) - 1}, rs = {(void *)rsp, sizeof(stack) - 1};
            ssize_t got = process_vm_readv(getpid(), &ls, 1, &rs, 1, 0);
            for (ssize_t i = 0; got > 0 && i + 8 < got; ++i) {
                if (!memcmp(stack + i, "D:\\trico", 8) || !memcmp(stack + i, "C:\\", 3)) {
                    stack[got] = 0;
                    const char *msg = stack + i;
                    if (strstr(msg, "ASSERT")) {
                        snprintf(line, sizeof(line), "  engine assert: %.400s\n", msg);
                        ssize_t written_=write(2, line, strlen(line)); (void)written_;
                        break;
                    }
                }
            }
        }
    }
    /* Guest call chain: the eboot keeps frame pointers, so follow rbp (reads cannot fault). */
    if (rip - (uintptr_t)image < 0x10000000) {
        uintptr_t rbp = (uintptr_t)UC_REG(uc, rbp);
        static const char *regs[] = {"rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp"};
        const uint64_t values[] = {(uint64_t)UC_REG(uc, rax), (uint64_t)UC_REG(uc, rbx), (uint64_t)UC_REG(uc, rcx),
                                   (uint64_t)UC_REG(uc, rdx), (uint64_t)UC_REG(uc, rsi), (uint64_t)UC_REG(uc, rdi),
                                   (uint64_t)UC_REG(uc, rbp), (uint64_t)UC_REG(uc, rsp)};
        for (int r = 0; r < 8; ++r) {
            snprintf(line, sizeof(line), "  %s=%016llx%s", regs[r], (unsigned long long)values[r], r == 3 || r == 7 ? "\n" : " ");
            ssize_t written_=write(2, line, strlen(line)); (void)written_;
        }
        for (int depth = 0; depth < 24 && rbp; ++depth) {
            uintptr_t frame[2];
            struct iovec local = {frame, sizeof(frame)}, remote = {(void *)rbp, sizeof(frame)};
            if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)sizeof(frame) || frame[0] <= rbp) break;
            if (frame[1] - (uintptr_t)image < 0x10000000)
                snprintf(line, sizeof(line), "  guest #%d return 0x%lx\n", depth, (unsigned long)(frame[1] - (uintptr_t)image));
            else
                snprintf(line, sizeof(line), "  guest #%d return %p (host)\n", depth, (void *)frame[1]);
            ssize_t written_=write(2, line, strlen(line)); (void)written_;
            rbp = frame[0];
        }
    }
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    if (isa_table) { isa_rewrite_report(isa_table, stderr); fflush(stderr); }
    _exit(128 + sig);
}
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)UC_REG(uc, rip), rbp=(uintptr_t)UC_REG(uc, rbp);
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)UC_REG(uc, rdi));
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    {   /* Thread name (the process is terminating: stdio-free reads are enough). */
        char name[32]={0};
#ifdef __APPLE__
        pthread_getname_np(pthread_self(),name,sizeof(name));
#else
        char path[64];
        snprintf(path,sizeof(path),"/proc/self/task/%lu/comm",(unsigned long)tid);
        int fd=open(path,O_RDONLY);
        if (fd>=0) { ssize_t got=read(fd,name,sizeof(name)-1); (void)got; close(fd); }
#endif
        char head[96]; snprintf(head,sizeof(head),"  tid=%lu name=%s",(unsigned long)tid,name[0] ? name : "?\n");
        { ssize_t written_=write(2,head,strlen(head)); (void)written_; }
    }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        Dl_info where;
        if (rip-(uintptr_t)image>=0x10000000 && dladdr((void *)rip,&where) && where.dli_fname) {
            char sym[320];
            const char *file=strrchr(where.dli_fname,'/');
            snprintf(sym,sizeof(sym),"      = %s+0x%lx (%s)\n",file ? file+1 : where.dli_fname,
                     (unsigned long)(rip-(uintptr_t)where.dli_fbase),where.dli_sname ? where.dli_sname : "?");
            ssize_t written_=write(2,sym,strlen(sym)); (void)written_;
        }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void dump_all_threads(void *context, const char *head) {
    { ssize_t written_=write(2,head,strlen(head)); (void)written_; }
    dump_frames(context);
#ifdef __APPLE__
    /* No /proc: signal every other pthread of the task (Mach threads without a pthread, e.g.
     * workqueue or Rosetta helpers, are skipped), as bloodborne_mac_port. */
    thread_act_array_t list;
    mach_msg_type_number_t count=0;
    mach_port_t self=pthread_mach_thread_np(pthread_self());
    if (task_threads(mach_task_self(),&list,&count)==KERN_SUCCESS)
        for (mach_msg_type_number_t i=0;i<count;++i) {
            pthread_t thread=list[i]!=self ? pthread_from_mach_thread_np(list[i]) : NULL;
            if (thread) { pthread_kill(thread,SIGUSR2); usleep(20000); }
        }
#else
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    if (dir>=0) close(dir);
#endif
    usleep(100000);
}
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    dump_all_threads(context, "STOP: watchdog timeout; thread stacks:\n");
    _exit(128 + sig);
}
/* kill -USR1 <pid>: dump every thread's stack into the log and keep running (diagnose hangs). */
static void dump_request(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    dump_all_threads(context, "DUMP: thread stacks on request (SIGUSR1); the game keeps running\n");
    const char tail[]="DUMP: end\n";
    ssize_t written_=write(2,tail,sizeof(tail)-1); (void)written_;
}
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
extern ABI float tlg_orbs_gain(void *player, float requested);
extern void tlg_orbs_status(int supported);
extern uintptr_t tlg_cheat_flag(unsigned id);
extern uintptr_t tlg_actor_slots_address(void);
extern void tlg_actor_status(int supported);
extern void tlg_cheat_status(unsigned id);
extern uintptr_t tlg_damage_multiplier(int taken);
extern uintptr_t tlg_orb_pickup_multiplier(unsigned kind);
extern void tlg_stat_multipliers_status(int damage, int orb_pickup);

static void install_cheats(const char *app0, Segment *segments, uint64_t count, uint64_t size) {
    char path[4096], serial[32] = "", version[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    sfo_value(path, "APP_VER", version, sizeof(version), NULL);
    if (strcmp(serial, "CUSA01623") || strcmp(version, "01.02")) return;
    const char *names[] = {"health", "magic", "item meter", "Rage of Sparta", "red orbs",
                           "Very Hard unlock", "infinite double jumps"};
    for (unsigned id = 0; id < TLG_CHEAT_COUNT; ++id) {
        int valid = 1;
        for (unsigned i = 0; i < TLG_CHEAT_HOOK_COUNT; ++i) {
            const TlgCheatHook *hook = &tlg_cheat_hooks[i];
            if (hook->id != id) continue;
            if (hook->offset > size || hook->length > size - hook->offset ||
                !mapped(segments, count, hook->offset, hook->length) ||
                !tlg_cheat_signature_match(hook, image + hook->offset, size - hook->offset)) valid = 0;
        }
        if (!valid) {
            printf("Cheats: %s unavailable (code signature mismatch)\n", names[id]);
            continue;
        }
        unsigned char *stubs = allocate(page_size);
        int32_t relative[TLG_CHEAT_HOOK_COUNT] = {0};
        for (unsigned i = 0; i < TLG_CHEAT_HOOK_COUNT; ++i) {
            const TlgCheatHook *hook = &tlg_cheat_hooks[i];
            if (hook->id != id) continue;
            unsigned char *stub = stubs + i * 256;
            int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + hook->offset + 5);
            if (delta < INT32_MIN || delta > INT32_MAX) { valid = 0; break; }
            relative[i] = (int32_t)delta;
            tlg_cheat_build_stub(stub, hook, tlg_cheat_flag(id),
                                 (uintptr_t)(image + hook->offset + hook->length));
        }
        if (!valid) {
            printf("Cheats: %s unavailable (hook outside relative-jump range)\n", names[id]);
            continue;
        }
        protect(stubs, page_size, 5);
        for (unsigned i = 0; i < TLG_CHEAT_HOOK_COUNT; ++i) {
            const TlgCheatHook *hook = &tlg_cheat_hooks[i];
            if (hook->id != id) continue;
            image[hook->offset] = 0xe9;
            memcpy(image + hook->offset + 1, &relative[i], 4);
            memset(image + hook->offset + 5, 0x90, hook->length - 5);
        }
        tlg_cheat_status(id);
        printf("Cheats: %s ready (live toggle, initially disabled unless saved)\n", names[id]);
    }
}

/* Damage hook for the enemy health bar (src/actor_hook.h). Always
 * installed when the code matches: it only records values; the overlay decides what to show. */
static void install_actor_watch(const char *app0, Segment *segments, uint64_t count, uint64_t size) {
    char path[4096], serial[32] = "", version[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    sfo_value(path, "APP_VER", version, sizeof(version), NULL);
    const uint64_t at = TLG_ACTOR_HOOK_OFFSET;
    if (strcmp(serial, "CUSA01623") || strcmp(version, "01.02") || at > size ||
        !mapped(segments, count, at, TLG_ACTOR_HOOK_LENGTH) ||
        !tlg_actor_signature_match(image + at, size - at)) {
        printf("Health display: unavailable (game version or code signature mismatch)\n");
        return;
    }
    unsigned char *stub = allocate(page_size);
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        printf("Health display: unavailable (hook outside relative-jump range)\n");
        return;
    }
    tlg_actor_build_stub(stub, tlg_actor_slots_address(), (uintptr_t)(image + at + TLG_ACTOR_HOOK_LENGTH));
    protect(stub, page_size, 5);
    const int32_t relative = (int32_t)delta;
    image[at] = 0xe9;
    memcpy(image + at + 1, &relative, 4);
    memset(image + at + 5, 0x90, TLG_ACTOR_HOOK_LENGTH - 5);
    tlg_actor_status(1);
    printf("Health display: damage hook ready (enemy health bar in the menu)\n");
}

static void install_orb_multiplier(const char *app0, Segment *segments, uint64_t count,
                                   uint64_t size) {
    char path[4096], serial[32] = "", version[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    sfo_value(path, "APP_VER", version, sizeof(version), NULL);
    if (strcmp(serial, "CUSA01623") || strcmp(version, "01.02") ||
        !tlg_orb_signatures_match(image, size)) {
        printf("Red orbs: multiplier unavailable (game version or gain signatures mismatch)\n");
        return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!mapped(segments, count, tlg_orb_calls[i], 5)) return;
    }
    /* SysV call sites: preserve the receiver across the helper, then tail-call the original
     * setter. Its cap and upgrade notifications remain in the game's own code. */
    unsigned char *stub = allocate(page_size);
    uintptr_t helper = (uintptr_t)tlg_orbs_gain, setter = (uintptr_t)(image + 0x6a9f0);
    unsigned char code[26];
    tlg_orb_build_stub(code, helper, setter);
    int32_t displacements[3];
    for (unsigned i = 0; i < 3; ++i) {
        int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + tlg_orb_calls[i] + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) {
            printf("Red orbs: multiplier unavailable (hook outside relative-call range)\n");
            return;
        }
        displacements[i] = (int32_t)delta;
    }
    memcpy(stub, code, sizeof(code));
    protect(stub, page_size, 5);
    for (unsigned i = 0; i < 3; ++i)
        memcpy(image + tlg_orb_calls[i] + 1, &displacements[i], 4);
    tlg_orbs_status(1);
    printf("Red orbs: live multiplier ready (3 validated gain paths, 0.1x to 100x)\n");
}
/* Writes a jump from image+at to `stub`; the rest of the replaced bytes become nops (the stub
 * replays them). 0 when the stub is out of relative-jump range. */
static int install_jump(uint64_t at, unsigned length, unsigned char *stub) {
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return 0;
    const int32_t relative = (int32_t)delta;
    image[at] = 0xe9;
    memcpy(image + at + 1, &relative, 4);
    memset(image + at + 5, 0x90, length - 5);
    return 1;
}

/* Damage dealt/taken and green/blue/gold orb multipliers (src/damage_hook.h, src/orb_hook.h):
 * the stubs read the menu's floats directly, so changes apply on the next hit or pickup. */
static void install_stat_multipliers(const char *app0, Segment *segments, uint64_t count, uint64_t size) {
    char path[4096], serial[32] = "", version[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    sfo_value(path, "APP_VER", version, sizeof(version), NULL);
    const int game = !strcmp(serial, "CUSA01623") && !strcmp(version, "01.02");
    const uint64_t damage_at = TLG_DAMAGE_HOOK_OFFSET, pickup_at = TLG_ORB_PICKUP_OFFSET;
    int damage = game && damage_at < size && mapped(segments, count, damage_at, TLG_DAMAGE_HOOK_LENGTH) &&
                 tlg_damage_signature_match(image + damage_at, size - damage_at);
    int pickup = game && pickup_at < size && mapped(segments, count, pickup_at, TLG_ORB_PICKUP_LENGTH) &&
                 tlg_orb_pickup_signature_match(image + pickup_at, size - pickup_at);
    unsigned char *stubs = damage || pickup ? allocate(page_size) : NULL;
    if (damage) {
        tlg_damage_build_stub(stubs, tlg_damage_multiplier(1), tlg_damage_multiplier(0),
                               (uintptr_t)(image + damage_at + TLG_DAMAGE_HOOK_LENGTH));
    }
    if (pickup) {
        tlg_orb_pickup_build_stub(stubs + 256, tlg_orb_pickup_multiplier(0), tlg_orb_pickup_multiplier(1),
                                   tlg_orb_pickup_multiplier(2),
                                   (uintptr_t)(image + pickup_at + TLG_ORB_PICKUP_LENGTH));
    }
    if (stubs) protect(stubs, page_size, 5);
    damage = damage && install_jump(damage_at, TLG_DAMAGE_HOOK_LENGTH, stubs);
    pickup = pickup && install_jump(pickup_at, TLG_ORB_PICKUP_LENGTH, stubs + 256);
    tlg_stat_multipliers_status(damage, pickup);
    printf("Multipliers: damage %s, green/blue/gold orbs %s\n", damage ? "ready" : "unavailable",
           pickup ? "ready" : "unavailable");
}
extern uintptr_t tlg_view_region_params(void);
extern uintptr_t tlg_view_fade_inverse(void);
extern uintptr_t tlg_view_floor_address(void);
extern void tlg_view_attach(unsigned char *guest_image);
extern void tlg_view_start(void);

/* Draw distance, LOD and clip planes (src/view_hook.h): the menu's values reach the game's data
 * through tlg_view_attach; the regions' LOD_ADJUST and FADE_DISTANCE go through four stubs. */
static void install_view_tuning(const char *app0, Segment *segments, uint64_t count, uint64_t size) {
    /* inFAMOUS Second Son 01.07 only (CUSA00004 has the same code); the byte signature below still decides. */
    char path[4096], serial[32] = "", version[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    sfo_value(path, "APP_VER", version, sizeof(version), NULL);
    if ((strcmp(serial, "CUSA00223") && strcmp(serial, "CUSA00004")) || strcmp(version, "01.07")) return;
    if (!mapped(segments, count, TLG_VIEW_LOD_TABLE, TLG_VIEW_LOD_ROWS * TLG_VIEW_LOD_STRIDE) ||
        !mapped(segments, count, TLG_VIEW_BANDS, 4 * TLG_VIEW_BAND_COUNT) ||
        !mapped(segments, count, TLG_VIEW_NEAR_CLIP, 8) ||
        size < TLG_VIEW_LEVEL + 4 || /* the level word is in the bss: inside the image, past the file-backed segments */
        !mapped(segments, count, tlg_view_budget_at[0], tlg_view_budget_at[TLG_VIEW_BUDGET_SITES - 1] + 4 - tlg_view_budget_at[0]) ||
        !mapped(segments, count, tlg_view_region_sites[1], sizeof(tlg_view_region_code)) ||
        !mapped(segments, count, TLG_VIEW_LOWER_SITE, TLG_VIEW_LOWER_LENGTH) ||
        !tlg_view_signature_match(image, size)) {
        return; /* another game or build: silent, like the other hooks of other games */
    }
    unsigned char *stubs = allocate(page_size);
    const uint32_t *sites[2] = {tlg_view_region_sites, tlg_view_fade_sites};
    for (unsigned kind = 0; kind < 2; ++kind) {
        for (unsigned i = 0; i < 2; ++i) {
            const uint32_t at = sites[kind][i];
            unsigned char *stub = stubs + (kind * 2 + i) * 128;
            const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
            if (delta < INT32_MIN || delta > INT32_MAX) {
                printf("Draw distance: unavailable (hook outside relative-jump range)\n");
                return;
            }
            const uintptr_t resume = (uintptr_t)(image + at + TLG_VIEW_SITE_LENGTH);
            if (kind == 0) tlg_view_build_region_stub(stub, tlg_view_region_params(), resume);
            else tlg_view_build_fade_stub(stub, tlg_view_fade_inverse(), resume);
        }
    }
    /* Quality floor (view_hook.h): the stub that stops the game from lowering its dynamic quality level
     * under the floor. Optional: without it the floor still raises the level, only not for long. */
    unsigned char *lower_stub = stubs + 4 * 128;
    const int64_t lower_delta = (int64_t)(uintptr_t)lower_stub - (int64_t)(uintptr_t)(image + TLG_VIEW_LOWER_SITE + 5);
    const int lower = tlg_view_lower_signature(image, size) && lower_delta >= INT32_MIN && lower_delta <= INT32_MAX;
    if (lower)
        tlg_view_build_lower_stub(lower_stub, tlg_view_floor_address(),
                                  (uintptr_t)(image + TLG_VIEW_LOWER_SITE + TLG_VIEW_LOWER_LENGTH));
    protect(stubs, page_size, 5);
    for (unsigned kind = 0; kind < 2; ++kind)
        for (unsigned i = 0; i < 2; ++i)
            install_jump(sites[kind][i], TLG_VIEW_SITE_LENGTH, stubs + (kind * 2 + i) * 128);
    if (lower) install_jump(TLG_VIEW_LOWER_SITE, TLG_VIEW_LOWER_LENGTH, lower_stub);
    else puts("Quality floor: unavailable (code differs)");
    tlg_view_attach(image);
    printf("Draw distance: ready (LOD scale, LOD bands, region LOD_ADJUST and FADE_DISTANCE, near/far clip)\n");
}

/* Temporal upscaling, game side (src/iss_temporal.h, iss_temporal_hook.h). Off unless
 * iss_temporal=1 in tlg.ini (or TLG_ISS_TEMPORAL=1); then the image keeps its bytes. */
static int iss_temporal_requested(void) {
    const char *env = getenv("TLG_ISS_TEMPORAL");
    if (env && *env) return env[0] == '1' && !env[1];
    const char *config_path = getenv("TLG_CONFIG");
    FILE *f = fopen(config_path && *config_path ? config_path : "tlg.ini", "r");
    if (!f) return 0;
    char line[1024];
    int on = 0;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "iss_temporal=", 13)) on = line[13] == '1' && (line[14] == 0 || line[14] == '\r' || line[14] == '\n');
    fclose(f);
    return on;
}

/* TLG_EXPOSURE_LOG=1 (diagnostic, 2026-11-17): the owner sees the screen go dark for 2-3 s after a
 * camera change, only with GPU particles on. FUN_00171610 sums an array of floats on the CPU (the
 * per-tile luminance the GPU wrote; eboot+0x1717c0 is the AVX loop) and the exposure follows the sum.
 * A hook before the loop logs the array: count, sum, mean, max (and where), NaN/Inf count. */
static int exposure_cmp(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}
/* Auto exposure fix (default on; TLG_EXPOSURE_ROBUST=<d> sets the margin, 0 or "off" turns it off): no tile
 * may be brighter than the frame's median tile by more than d (0.05; the values are the game's per-tile
 * luminance terms, higher = brighter, max ~0). Bright particles near the camera (sun-lit smoke on Delsin's
 * hands, HDR up to ~450) lifted a few tiles and the game darkened the whole picture for 2-3 s (owner's
 * captures 2026-11-18); a uniformly bright scene keeps a high median and is not touched. */
static float exposure_margin(void) {
    const char *e = getenv("TLG_EXPOSURE_ROBUST");
    if (!e || !*e) return 0.05f;
    if (!strcmp(e, "off")) return 0;
    return (float)atof(e);
}
/* A tile's luminance term as the GPU may have written it: finite, not exactly 0 (unwritten). */
static int exposure_candidate(float v) {
    return isfinite(v) && v != 0.0f;
}
static void exposure_robust(float *values, uint64_t count, uint64_t *clamped) {
    static float d = -1;
    static int read;
    if (!read) { read = 1; d = exposure_margin(); }
    if (d <= 0 || count > 4096) return;
    /* Tiles the GPU did not measure stand for the median of those it did. At a 4K output the game sums
     * 900 tiles where 1080p has 225, and every other call 50-275 come back exactly 0 (unwritten) and
     * ~180 far below the rest (-43, -1.3e5, -1.7e38) or NaN: the sum swung by up to 1e39 and the coals
     * scene went dark at random (2026-11-23). Real tiles stay within ~1 of the median (-0.7..0 at
     * 1080p), so anything more than 4 below it is not a measurement. No 1080p value is affected. */
    float sorted[4096];
    uint64_t n = 0;
    for (uint64_t i = 0; i < count; ++i)
        if (exposure_candidate(values[i])) sorted[n++] = values[i];
    if (n == 0) return;
    qsort(sorted, n, sizeof(float), exposure_cmp);
    const float median = sorted[n / 2];
    const float floor = median - 4.0f, limit = median + d;
    for (uint64_t i = 0; i < count; ++i) {
        if (!exposure_candidate(values[i]) || values[i] < floor) { values[i] = median; ++*clamped; }
        else if (values[i] > limit) { values[i] = limit; ++*clamped; }
    }
}
/* The sum's caller (eboot 0x192f9e) keeps the eye adaptation: a ring of the last measurements
 * ([rbx+0x1eb48] values, +0x1eb50 capacity, +0x1eb54 count, +0x1eb60 their average) and the adapted value
 * [rbx+0x17264], which moves toward its target by dt * speed * count / capacity; a camera cut or load resets
 * the ring (count 0), so the old exposure stays on screen until the ring refills. Its rbx is the one the
 * sum saved at [rbp-0x28]. */
struct exposure_state { uint32_t capacity, count; float average, adapted, scale, timer; uint32_t flag; };
static int exposure_caller_state(uint64_t frame, struct exposure_state *st, uint64_t *base) {
    if (frame < 0x100000) return 0;
    const uint64_t rbx = *(const uint64_t *)(uintptr_t)(frame - 0x28);
    if (rbx < 0x100000) return 0;
    st->capacity = *(const uint32_t *)(uintptr_t)(rbx + 0x1eb50);
    st->count = *(const uint32_t *)(uintptr_t)(rbx + 0x1eb54);
    st->average = *(const float *)(uintptr_t)(rbx + 0x1eb60);
    st->adapted = *(const float *)(uintptr_t)(rbx + 0x17264);
    st->scale = *(const float *)(uintptr_t)(rbx + 0x17260);
    st->timer = *(const float *)(uintptr_t)(rbx + 0x1eb70);
    st->flag = *(const uint8_t *)(uintptr_t)(rbx + 0x1eb6a) | (uint32_t)*(const uint8_t *)(uintptr_t)(rbx + 0x1eb69) << 8;
    *base = rbx;
    return 1;
}
/* The caller keeps the ring's sum as a running float ([rbx+0x1eb5c]: minus the value it replaces, plus
 * the new one; the average [rbx+0x1eb60] = sum / count). Its values are tiny (exp of the tile sum, ~1e-20
 * at a 4K output against ~1e-18 at 1080p), so rounding left in the running sum drifted it below zero
 * after a few hundred frames and the eye adaptation collapsed (4K coals, 2026-11-23). Before the caller
 * updates it, the sum is recomputed from the ring itself (part of the exposure fix). */
static void exposure_ring_exact(uint64_t frame) {
    if (exposure_margin() <= 0 || frame < 0x100000) return;
    const uint64_t rbx = *(const uint64_t *)(uintptr_t)(frame - 0x28);
    if (rbx < 0x100000) return;
    const float *ring = *(const float *const *)(uintptr_t)(rbx + 0x1eb48);
    const uint32_t capacity = *(const uint32_t *)(uintptr_t)(rbx + 0x1eb50);
    const uint32_t count = *(const uint32_t *)(uintptr_t)(rbx + 0x1eb54);
    if (!ring || (uintptr_t)ring < 0x100000 || capacity == 0 || capacity > 256 || count > capacity) return;
    double sum = 0;
    for (uint32_t i = 0; i < count; ++i) sum += ring[i];
    *(float *)(uintptr_t)(rbx + 0x1eb5c) = (float)sum;
}
static void exposure_sum_log(float *values, uint64_t count, uint64_t frame) {
    static uint64_t calls;
    ++calls;
    if (!values || count == 0 || count > (1u << 20)) {
        printf("Exposure sum %llu: count %llu (skipped)\n", (unsigned long long)calls, (unsigned long long)count);
        return;
    }
    uint64_t clamped = 0;
    exposure_robust(values, count, &clamped);
    exposure_ring_exact(frame);
    static int log = -1;
    if (log < 0) { const char *e = getenv("TLG_EXPOSURE_LOG"); log = e && *e == '1'; }
    if (!log) return;
    double sum = 0;
    float top = -INFINITY, low = INFINITY;
    uint64_t at = 0, bad = 0, zero = 0;
    for (uint64_t i = 0; i < count; ++i) {
        const float v = values[i];
        if (!isfinite(v)) { ++bad; continue; }
        if (v == 0) ++zero;
        sum += v;
        if (v > top) { top = v; at = i; }
        if (v < low) low = v;
    }
    struct exposure_state st = {0};
    uint64_t base = 0;
    const int have = exposure_caller_state(frame, &st, &base);
    printf("Exposure sum %llu: n=%llu sum=%.5g mean=%.5g min=%.5g max=%.5g@%llu bad=%llu zero=%llu clamped=%llu"
           " | ring %u/%u avg=%.5g adapted=%.5g scale=%.5g timer=%.4g flags=%x\n",
           (unsigned long long)calls, (unsigned long long)count, sum, sum / (double)count, low, top,
           (unsigned long long)at, (unsigned long long)bad, (unsigned long long)zero, (unsigned long long)clamped,
           have ? st.count : 0, have ? st.capacity : 0, have ? st.average : 0.0f, have ? st.adapted : 0.0f,
           have ? st.scale : 0.0f, have ? st.timer : 0.0f, have ? st.flag : 0);
}
static void install_exposure_log(uint64_t size) {
    const char *env = getenv("TLG_EXPOSURE_LOG");
    if ((!env || *env != '1') && exposure_margin() <= 0) return;
    /* mov eax,0xe0e ; mov rsi,r13 ; lea r13,[rdi+32]  (rdi = the array, r13 = its length) */
    static const unsigned char want[] = {0xb8, 0x0e, 0x0e, 0x00, 0x00, 0x4c, 0x89, 0xee, 0x4c, 0x8d, 0x6f, 0x20};
    const uint64_t site = 0x171783;
    if (site + sizeof(want) > size || memcmp((unsigned char *)image + site, want, sizeof(want))) {
        puts("Auto exposure: code differs, hook not installed");
        return;
    }
    unsigned char *s = allocate(page_size);
    size_t k = 0;
#define EB(x) (s[k++] = (unsigned char)(x))
    static const unsigned char pushes[] = {0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53};
    for (size_t i = 0; i < sizeof(pushes); ++i) EB(pushes[i]);
    EB(0x49); EB(0x89); EB(0xe3);                                        /* mov r11, rsp */
    EB(0x48); EB(0x83); EB(0xe4); EB(0xe0);                              /* and rsp, -32 */
    EB(0x48); EB(0x81); EB(0xec); EB(0x20); EB(0x02); EB(0x00); EB(0x00); /* sub rsp, 544 */
    for (int n = 0; n < 16; ++n) {                                        /* vmovups [rsp+32n], ymmN */
        EB(0xc5); EB(n < 8 ? 0xfc : 0x7c); EB(0x11); EB(0x84 | ((n & 7) << 3)); EB(0x24);
        const uint32_t off = 32u * (uint32_t)n;
        EB(off); EB(off >> 8); EB(off >> 16); EB(off >> 24);
    }
    EB(0x4c); EB(0x89); EB(0x9c); EB(0x24); EB(0x00); EB(0x02); EB(0x00); EB(0x00); /* mov [rsp+512], r11 */
    EB(0x4c); EB(0x89); EB(0xee);                                        /* mov rsi, r13 */
    EB(0x48); EB(0x89); EB(0xea);                                        /* mov rdx, rbp (the sum's frame) */
    EB(0x48); EB(0xb8);                                                  /* mov rax, imm64 */
    const uint64_t fn = (uint64_t)(uintptr_t)&exposure_sum_log;
    for (int i = 0; i < 8; ++i) EB(fn >> (8 * i));
    EB(0xff); EB(0xd0);                                                  /* call rax */
    for (int n = 0; n < 16; ++n) {                                        /* vmovups ymmN, [rsp+32n] */
        EB(0xc5); EB(n < 8 ? 0xfc : 0x7c); EB(0x10); EB(0x84 | ((n & 7) << 3)); EB(0x24);
        const uint32_t off = 32u * (uint32_t)n;
        EB(off); EB(off >> 8); EB(off >> 16); EB(off >> 24);
    }
    EB(0x4c); EB(0x8b); EB(0x9c); EB(0x24); EB(0x00); EB(0x02); EB(0x00); EB(0x00); /* mov r11, [rsp+512] */
    EB(0x4c); EB(0x89); EB(0xdc);                                        /* mov rsp, r11 */
    static const unsigned char pops[] = {0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58};
    for (size_t i = 0; i < sizeof(pops); ++i) EB(pops[i]);
    for (size_t i = 0; i < 8; ++i) EB(want[i]);                          /* the two replaced instructions */
    EB(0xff); EB(0x25); EB(0); EB(0); EB(0); EB(0);                      /* jmp [rip+0] */
    const uint64_t back = (uint64_t)(uintptr_t)image + site + 8;
    for (int i = 0; i < 8; ++i) EB(back >> (8 * i));
#undef EB
    const int64_t distance = (int64_t)(uintptr_t)s - ((int64_t)(uintptr_t)image + (int64_t)site + 5);
    if (distance > INT32_MAX || distance < INT32_MIN) {
        puts("Auto exposure: stub out of reach of the image, hook not installed");
        return;
    }
    protect(s, page_size, 5);
    unsigned char *p = (unsigned char *)image + site;
    const int32_t rel = (int32_t)distance;
    p[0] = 0xe9;
    memcpy(p + 1, &rel, 4);
    p[5] = p[6] = p[7] = 0x90;
    printf("Auto exposure: tile limit median + %.3g (TLG_EXPOSURE_ROBUST=off: game default), hook at eboot+0x171783\n", exposure_margin());
}
static void iss_start_camera_dump(void);
static void install_iss_temporal(const char *app0, Segment *segments, uint64_t count, uint64_t size) {
    if (!iss_temporal_requested()) return;
    char path[4096], serial[32] = "", version[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    sfo_value(path, "APP_VER", version, sizeof(version), NULL);
    /* CUSA00004 01.07 has the same code (2026-11-08: every hook signature found); the hooks still compare
     * their bytes before writing. */
    if ((strcmp(serial, "CUSA00223") && strcmp(serial, "CUSA00004")) || strcmp(version, "01.07")) {
        printf("Temporal upscaling: unavailable (title '%s', version '%s'; needs CUSA00223 or CUSA00004 01.07)\n", serial, version);
        return;
    }
    if (!mapped(segments, count, ISS_TEMPORAL_JITTER_SITE, ISS_TEMPORAL_JITTER_LENGTH) ||
        !mapped(segments, count, ISS_TEMPORAL_AA_SITE, ISS_TEMPORAL_AA_LENGTH) ||
        !mapped(segments, count, ISS_TEMPORAL_DONE_SITE, ISS_TEMPORAL_DONE_LENGTH)) {
        puts("Temporal upscaling: unavailable (hook outside the executable segments)");
        return;
    }
    unsigned char *stubs = allocate(page_size);
    if (!iss_temporal_install_hooks(image, size, stubs)) {
        puts("Temporal upscaling: unavailable (code differs, or the stubs are out of reach of the image)");
        return;
    }
    protect(stubs, page_size, 5);
    puts("Temporal upscaling: game hooks ready (they run the game's own code until the renderer reports ready)");
    /* The renderer runs in the GPU process (split mode): it cannot call iss_temporal_set_renderer_ready, so
     * the launcher says it with TLG_ISS_TEMPORAL_READY=1 when its temporal pass is on. */
    const char *ready = getenv("TLG_ISS_TEMPORAL_READY");
    if (ready && *ready == '1') {
        iss_temporal_set_renderer_ready(1);
        puts("Temporal upscaling: renderer ready (TLG_ISS_TEMPORAL_READY=1): jitter and markers on");
    }
    iss_start_camera_dump();
}
/* Diagnostic (TLG_ISS_CAMERA_DUMP=1): every 500 ms, the camera blocks of the two newest complete
 * frames, to identify the view-projection offline. Reads the snapshot ring only (no guest thread). */
static void iss_print_floats(const char *name, const float *f, int n) {
    printf("  %s", name);
    for (int i = 0; i < n; ++i) printf(" %.9g", f[i]);
    putchar('\n');
}
static void *iss_camera_dump_thread(void *arg) {
    (void)arg;
    for (int round = 0; round < 2000; ++round) {
        usleep(500000);
        const uint32_t latest = iss_temporal_latest_seq();
        if (latest < 3) continue;
        for (uint32_t seq = latest - 2; seq <= latest - 1; ++seq) {
            IssTemporalFrame f;
            if (!iss_temporal_copy_frame(seq, &f)) continue;
            printf("ISS camera seq %u phase %u jitter %.6f %.6f size %u %u reset %u\n", f.seq, f.phase, f.jitter[0],
                   f.jitter[1], f.render_width, f.render_height, f.reset);
            iss_print_floats("window", f.window, 4);
            iss_print_floats("proj", f.camera.proj, 16);
            iss_print_floats("m200", f.camera.m200, 16);
            iss_print_floats("m240", f.camera.m240, 16);
            iss_print_floats("v420", f.camera.vec420, 4);
            iss_print_floats("m430", f.camera.m430, 16);
            iss_print_floats("m470", f.camera.m470, 16);
            iss_print_floats("raw580", f.camera.raw580, 16);
            iss_print_floats("camwindow", f.camera.window, 4);
        }
        fflush(stdout);
    }
    return NULL;
}
static void iss_start_camera_dump(void) {
    const char *env = getenv("TLG_ISS_CAMERA_DUMP");
    if (!env || *env != '1') return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, iss_camera_dump_thread, NULL) == 0) pthread_detach(thread);
}
/* The renderer's side of src/iss_temporal.h. */
int iss_temporal_installed(void) { return iss_temporal_state.installed; }
void iss_temporal_set_renderer_ready(int ready) {
    iss_temporal_state.ready = ready ? 1 : 0;
    iss_temporal_update_active();
}
void iss_temporal_set_phases(uint32_t phases) {
    iss_temporal_state.phases = phases < 1 ? 1 : phases > ISS_TEMPORAL_MAX_PHASES ? ISS_TEMPORAL_MAX_PHASES : phases;
}
uint32_t iss_temporal_latest_seq(void) { return iss_temporal_state.seq; }
int iss_temporal_get_frame(uint32_t seq, IssTemporalFrame *out) { return iss_temporal_copy_frame(seq, out); }

#include "iss_karma.h"
#include "iss_perf_patches.h"
#include "iss_karma_signatures.h"
#include "iss_native_resolution.h"
#include "iss_hud_trace.h"
#include "iss_hud_corners.h"

/* Two frames in flight (TLG_FRAMES_AHEAD=1, experimental: no GPU particles yet). The game submits frame N only after the GPU
 * finished N-1, because its five per-frame ring allocators (graphics context at 0xF1BCD0, allocators at
 * +0x28 +0x58 +0x88 +0xb8 +0xe8: base[2], cur, end, size, idx) have two slots. Here they get a third
 * slot (guest direct memory, GPU-visible), rotate over three, and the wait at 0x1f26e7 targets N-2. */
static uint64_t frames_third[5];
static int frames_ready = -1;
static void ISS_MS_ABI frames_rotate(uint64_t *regs) {
    static const unsigned offs[5] = {0x28, 0x58, 0x88, 0xb8, 0xe8};
    unsigned char *ctx = (unsigned char *)(uintptr_t)regs[3]; /* r12 */
    if (frames_ready < 0) {
        /* The game takes all free direct memory at start; the third slots come from its own pools
         * (the allocator gfx_context_init uses: FUN_00059d50(pool, size, 0x400, 0x2e)). */
        typedef uint64_t (*pool_alloc_fn)(void *, uint64_t, uint64_t, uint64_t) __attribute__((sysv_abi));
        static const uint32_t pools[5] = {0xE0CA70, 0xE0CB30, 0xE0CA70, 0xE0CA70, 0xE0CB30};
        pool_alloc_fn pool_alloc = (pool_alloc_fn)(uintptr_t)(image + 0x59d50);
        frames_ready = 1;
        uint64_t sum = 0;
        for (unsigned i = 0; i < 5; ++i) {
            const uint64_t size = *(uint32_t *)(ctx + offs[i] + 0x20);
            const uint64_t addr = pool_alloc(image + pools[i], size, 0x400, 0x2e);
            if (!addr) {
                printf("Frames ahead: slot %u (%llu bytes) not allocated\n", i, (unsigned long long)size);
                frames_ready = 0;
                break;
            }
            frames_third[i] = addr;
            sum += size;
        }
        /* The game's own command-buffer copy mode ([0xF1BCC0]): every command buffer of the frame is
         * copied into allocator 4's ring (now three slots) before the submit, so the game can rewrite its
         * command buffers (and what they chain to) while the previous frame is still being parsed.
         * TLG_FRAMES_COPY=0: leave the game's setting. */
        const char *copy = getenv("TLG_FRAMES_COPY");
        const unsigned before = image[0xF1BCC0];
        if (frames_ready && !(copy && *copy == '0')) image[0xF1BCC0] = 1;
        printf("Frames ahead: third slots %s (%.1f MB), DCB copy mode %u -> %u\n", frames_ready ? "ready" : "FAILED",
               sum / 1048576.0, before, (unsigned)image[0xF1BCC0]);
    }
    /* The game's slot index stays 0/1 (its code may index two-element arrays with it); the physical
     * buffer under the slot it switches to cycles over three, so the slot it leaves still holds the
     * previous frame and the one it enters is the buffer of three frames ago. */
    static uint64_t phys[5][3];
    static int32_t active[5] = {-1, -1, -1, -1, -1};
    for (unsigned i = 0; i < 5; ++i) {
        unsigned char *a = ctx + offs[i];
        uint32_t *idx = (uint32_t *)(a + 0x28);
        uint64_t *base = (uint64_t *)a;
        const uint32_t next_idx = *idx ^ 1;
        if (frames_ready > 0 && active[i] < 0) {
            phys[i][0] = base[0];
            phys[i][1] = base[1];
            phys[i][2] = frames_third[i];
            active[i] = (int32_t)(*idx & 1);
        }
        if (frames_ready > 0) {
            active[i] = (active[i] + 1) % 3;
            base[next_idx] = phys[i][active[i]];
        }
        *idx = next_idx;
        *(uint64_t *)(a + 0x10) = base[next_idx];
        *(uint64_t *)(a + 0x18) = base[next_idx] + *(uint32_t *)(a + 0x20);
    }
    regs[0x68 / 8] = *(uint64_t *)(ctx + 0xe8 + 0x18);          /* rcx: allocator 5's end */
    regs[0x70 / 8] = *(uint64_t *)(image + 0xD9E058);           /* rax: as the original block left it */
}
/* The deferred-free list (0xE86EC8, r13 in 0x196c30): two 24-byte slot records at +0x10 and +0x28; after each
 * frame submit the game toggles [r13+8] and frees the new slot's items (released two frames ago). With two
 * frames in flight that is one frame too early: the record about to be freed is swapped with one held back
 * from the previous frame, so the game frees what was released three frames ago. */
static unsigned char frames_held[24];
static void frames_free_held(void) {
    /* As the game's own loop (0x196ce0): entries of 16 bytes {pool index, pointer}, each freed with
     * FUN_0005A5E0(0xE0CA70 + index * 0xC0, pointer), then the array with FUN_005B92C0. */
    typedef void (*free_item_fn)(void *, uint64_t) __attribute__((sysv_abi));
    typedef void (*free_array_fn)(void *) __attribute__((sysv_abi));
    unsigned char *array = *(unsigned char **)frames_held;
    const int32_t count = *(int32_t *)(frames_held + 8);
    for (int32_t i = 0; i < count; ++i) {
        const int64_t index = *(int32_t *)(array + i * 16);
        ((free_item_fn)(uintptr_t)(image + 0x5a5e0))(image + 0xE0CA70 + index * 0xC0, *(uint64_t *)(array + i * 16 + 8));
    }
    if (array) ((free_array_fn)(uintptr_t)(image + 0x5b92c0))(array);
    memset(frames_held, 0, sizeof(frames_held));
}
static void ISS_MS_ABI frames_defer_free(uint64_t *regs) {
    if (frames_ready <= 0) return;
    unsigned char *list = (unsigned char *)(uintptr_t)regs[2]; /* r13 */
    /* A world unload (0x5054b0 -> 0x196d90) runs this frame end with [list+4] = 0x10 after the GPU went idle:
     * free what is held now, before the level's pools change, and let the game free its slot as before. */
    if (*(uint32_t *)(list + 4) == 0x10) {
        frames_free_held();
        return;
    }
    const int32_t slot = *(int32_t *)(list + 8);
    unsigned char *record = list + 0x10 + (size_t)(1 - slot) * 24;
    unsigned char tmp[24];
    memcpy(tmp, record, 24);
    memcpy(record, frames_held, 24);
    memcpy(frames_held, tmp, 24);
}
static void install_frames_ahead(Segment *segments, uint64_t count, uint64_t size) {
    /* Opt-in (TLG_FRAMES_AHEAD=1): +8-10% FPS, but the GPU particles (fire, smoke) disappear: the particle
     * system reads the previous frame's GPU results, which are not done yet with two frames in flight. */
    const char *env = getenv("TLG_FRAMES_AHEAD");
    if (!env || *env != '1') return;
    static const unsigned char wait_orig[10] = {0x49,0x8b,0x9c,0x24,0x30,0x01,0x00,0x00,0xeb,0x19};
    static const unsigned char rot_orig[16] = {0x49,0x8b,0x44,0x24,0x38,0x49,0x8b,0x44,0x24,0x68,0x41,0x8b,0x44,0x24,0x50,0x89};
    if (0x1f2949 > size || !mapped(segments, count, 0x1f26e7, 0x1f2949 - 0x1f26e7) ||
        memcmp(image + 0x1f26e7, wait_orig, sizeof(wait_orig)) || memcmp(image + 0x1f2808, rot_orig, sizeof(rot_orig))) {
        printf("Frames ahead: code check failed, not installed\n");
        return;
    }
    unsigned char *stub = allocate(page_size);
    unsigned n = 0;
    memcpy(stub, wait_orig, 8); n = 8;
    const unsigned char dec[] = {0x48,0xff,0xcb}; memcpy(stub + n, dec, 3); n += 3;
    iss_karma_jump(stub + n, (uintptr_t)(image + 0x1f270a)); n += 14;
    unsigned char *bridge = stub + 256;
    iss_karma_bridge_ex(bridge, (uintptr_t)frames_rotate, NULL, (uintptr_t)(image + 0x1f2941), 0, 5);
    protect(stub, page_size, 5);
    if (!install_jump(0x1f26e7, sizeof(wait_orig), stub)) { printf("Frames ahead: stub out of range\n"); return; }
    iss_karma_jump(image + 0x1f2808, (uintptr_t)bridge);
    static const unsigned char free_orig[14] = {0x49,0x63,0x4d,0x08,0xb8,0x01,0x00,0x00,0x00,0xbb,0x08,0x00,0x00,0x00};
    if (!memcmp(image + 0x196cb1, free_orig, sizeof(free_orig))) {
        unsigned char *free_bridge = stub + 2048;
        protect(stub, page_size, 3);
        iss_karma_bridge_ex(free_bridge, (uintptr_t)frames_defer_free, free_orig, (uintptr_t)(image + 0x196cbf), 14, 5);
        protect(stub, page_size, 5);
        iss_karma_jump(image + 0x196cb1, (uintptr_t)free_bridge);
        printf("Frames ahead: deferred frees one frame later\n");
    } else {
        printf("Frames ahead: deferred-free code check failed\n");
    }
    printf("Frames ahead: two frames in flight (third allocator slots, wait for N-2)\n");
}
static void install_iss_hud_corners(const char *app0,Segment *segments,uint64_t count,uint64_t size){
    const char *enable=getenv("ISS_EXPERIMENTAL_HUD_CORNERS");
    if(!enable || strcmp(enable,"1"))return;
    char path[4096],serial[32]="",version[32]="";
    snprintf(path,sizeof(path),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
    sfo_value(path,"TITLE_ID",serial,sizeof(serial),NULL);sfo_value(path,"APP_VER",version,sizeof(version),NULL);
    int executable=0;
    for(uint64_t i=0;i<count;i++)if((segments[i].flags&1) && ISS_HUD_TRACE_SITE>=segments[i].address &&
        ISS_HUD_CORNER_SITE+sizeof(iss_hud_corner_tail)<=segments[i].address+segments[i].size)executable=1;
    int valid=!strcmp(serial,"CUSA00223")&&!strcmp(version,"01.07")&&executable&&iss_hud_trace_signature(image,size);
    if(valid)valid=!memcmp(image+ISS_HUD_CORNER_SITE,iss_hud_corner_tail,sizeof(iss_hud_corner_tail));
    const uint32_t vtables[]={0xd74668,0xd795d0,0xcdb7d0};
    const uint32_t methods[]={0x563710,0x5a7d50,0x2009e0};
    for(unsigned i=0;i<3&&valid;i++){
        uintptr_t method=0;
        if(vtables[i]+24u>size){valid=0;break;}
        memcpy(&method,image+vtables[i]+16,8);
        if(method!=(uintptr_t)image+methods[i])valid=0;
    }
    if(!valid){puts("HUD corners: refused incompatible game code/types; no writes");return;}
    unsigned char *bridge=allocate(page_size);
    unsigned n=iss_karma_bridge_ex(bridge,(uintptr_t)runtime_hud_corner_node,image+ISS_HUD_CORNER_SITE,
        (uintptr_t)image+ISS_HUD_CORNER_SITE+sizeof(iss_hud_corner_tail),sizeof(iss_hud_corner_tail),3);
    if(n>page_size)fail("HUD corner bridge overflow");
    protect(bridge,page_size,5);runtime_hud_corner_attach((uintptr_t)image);
    memset(image+ISS_HUD_CORNER_SITE,0x90,sizeof(iss_hud_corner_tail));
    iss_karma_jump(image+ISS_HUD_CORNER_SITE,(uintptr_t)bridge);
    puts("HUD corners: guarded gameplay-root transform candidate active");
}
static void install_iss_hud_trace(const char *app0,Segment *segments,uint64_t count,uint64_t size){
    const char *trigger=getenv("ISS_HUD_LAYOUT_TRACE_TRIGGER");
    if(!trigger || !*trigger)return;
    char path[4096],serial[32]="",version[32]="";
    snprintf(path,sizeof(path),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
    sfo_value(path,"TITLE_ID",serial,sizeof(serial),NULL);sfo_value(path,"APP_VER",version,sizeof(version),NULL);
    int executable=0;
    for(uint64_t i=0;i<count;i++)if((segments[i].flags&1) && ISS_HUD_TRACE_SITE>=segments[i].address &&
        ISS_HUD_TRACE_SITE+0x4b4u<=segments[i].address+segments[i].size)executable=1;
    if(strcmp(serial,"CUSA00223") || strcmp(version,"01.07") || !executable || !iss_hud_trace_signature(image,size)){
        puts("HUD layout observer: refused incompatible title/code; no writes");return;
    }
    unsigned char *bridge=allocate(page_size);
    unsigned n=iss_karma_bridge_ex(bridge,(uintptr_t)runtime_hud_trace_node,image+ISS_HUD_TRACE_SITE,
        (uintptr_t)image+ISS_HUD_TRACE_SITE+ISS_HUD_TRACE_STOLEN,ISS_HUD_TRACE_STOLEN,2);
    if(n>page_size)fail("HUD observer bridge overflow");
    protect(bridge,page_size,5);runtime_hud_trace_attach((uintptr_t)image);
    memset(image+ISS_HUD_TRACE_SITE,0x90,ISS_HUD_TRACE_STOLEN);
    iss_karma_jump(image+ISS_HUD_TRACE_SITE,(uintptr_t)bridge);
    puts("HUD layout observer: guarded US 1.07 hook active; widget values unchanged");
}
static void install_iss_native_resolution(const char *app0,Segment *segments,uint64_t count,uint64_t size) {
    const char *config_path=getenv("TLG_CONFIG");
    FILE *f=fopen(config_path && *config_path ? config_path : "tlg.ini","r");
    IssNativeConfig config;int valid=iss_native_config(f,&config);if(f)fclose(f);
    if(!valid){runtime_native_publish(-1,"Invalid native setting; no native patch applied",NULL);return;}
    /* TLG_NATIVE_OUTPUT=<1..5> (the launcher's "Resolution": 2 = 1440p, 4 = 4K): the native output
     * preset, over the ini's iss_native_output (not written back to it). */
    const char *native_output=getenv("TLG_NATIVE_OUTPUT");
    if(native_output && *native_output){
        char *end=NULL;const long preset=strtol(native_output,&end,10);
        if(end && !*end && preset>=1 && preset<ISS_NATIVE_PRESETS)config.output=(int)preset;
    }
    /* TLG_RENDER_SCALE=<25..99> (the launcher's "Render scale"): the game renders its 1080p frame at
     * that percentage when the ini asks for no native output (weaker Macs: fewer pixels to shade). */
    const char *scale=getenv("TLG_RENDER_SCALE");
    if(scale && *scale){
        char *end=NULL;const long percent=strtol(scale,&end,10);
        /* With a native output (1440p, 4K...) the launcher's scale applies there too, unless the
         * in-game "Native render scale" was set: 4K + 67% rendered the full 4K scene before. */
        if(end && !*end && percent>=25 && percent<100){
            if(!config.output){config.output=1;config.percent=(int)percent;}
            else if(config.percent==100)config.percent=(int)percent;
        }
    }
    /* TLG_BENCH_HALF_SCALE=1 (the launcher's Detailed benchmark, second run): half the render size,
     * whatever the ini asks for (native output too), so full against half tells GPU- from CPU-bound. */
    const char *half=getenv("TLG_BENCH_HALF_SCALE");
    if(half && half[0]=='1'){
        if(config.output)config.percent=config.percent/2<25 ? 25 : config.percent/2;
        else {config.output=1;config.percent=50;}
    }
    if(!config.output)return;
    IssNativePlan plan;
    if(!iss_native_plan(config,&plan)){
        runtime_native_publish(-1,"Requested scale exceeds the 4K pixel budget; no native patch applied",NULL);
        puts("Native rendering: refused dimensions/scale; original game dimensions retained");return;
    }
    char path[4096],serial[32]="",version[32]="";
    snprintf(path,sizeof(path),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
    sfo_value(path,"TITLE_ID",serial,sizeof(serial),NULL);sfo_value(path,"APP_VER",version,sizeof(version),NULL);
    for(unsigned s=0;s<ISS_NATIVE_SITE_COUNT+2;s++) {
        uint32_t at=s<ISS_NATIVE_SITE_COUNT ? iss_native_sites[s].offset : s==ISS_NATIVE_SITE_COUNT ? 0x58030c : 0x5802e6;
        unsigned length=s<ISS_NATIVE_SITE_COUNT ? iss_native_sites[s].length : s==ISS_NATIVE_SITE_COUNT ? sizeof(iss_native_projection) : sizeof(iss_native_aspect_width);
        int executable=0;
        for(uint64_t i=0;i<count;i++)if((segments[i].flags&1) && at>=segments[i].address &&
            at-segments[i].address<=segments[i].size && length<=segments[i].size-(at-segments[i].address))executable=1;
        if(!executable){valid=0;break;}
    }
    if(!valid || !iss_native_apply(image,size,serial,version,config)) {
        runtime_native_publish(-1,"US 1.07 code check failed or another resolution patch conflicts; no native writes",NULL);
        printf("Native rendering: refused title=%s version=%s (signature/segment mismatch); no writes\n",serial,version);return;
    }
    runtime_native_publish(1,"Experimental native startup rendering active",&plan);
    printf("Native rendering: output selection %ux%u, scale %d%%, scene/output surface %ux%u, logical HUD %ux%u, light entries %u; Hor+ vertical FOV preserved\n",
        plan.output_width,plan.output_height,plan.percent,plan.width,plan.height,plan.ui_width,plan.ui_height,plan.light_entries);
}
static void install_iss_karma(const char *app0,Segment *segments,uint64_t count,uint64_t size,int before_rewrites){
    char path[4096],serial[32]="",version[32]="";
    snprintf(path,sizeof(path),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
    sfo_value(path,"TITLE_ID",serial,sizeof(serial),NULL);sfo_value(path,"APP_VER",version,sizeof(version),NULL);
    if(strcmp(serial,"CUSA00223")){
        runtime_karma_compatibility_failure(1);printf("Karma switcher: unavailable: TITLE_ID='%s', expected CUSA00223\n",serial);return;
    }
    if(strcmp(version,"01.07")){
        runtime_karma_compatibility_failure(2);printf("Karma switcher: unavailable: APP_VER='%s', expected 01.07\n",version);return;
    }
    int executable=0;
    for(uint64_t i=0;i<count;++i)if((segments[i].flags&1) && ISS_KARMA_SITE>=segments[i].address && ISS_KARMA_SITE+ISS_KARMA_STOLEN<=segments[i].address+segments[i].size)executable=1;
    if(!executable){runtime_karma_compatibility_failure(3);puts("Karma switcher: unavailable: hook outside executable segments");return;}
    if(!iss_karma_signatures(image,size)){
        runtime_karma_compatibility_failure(before_rewrites?5:4);
        printf("Karma switcher: unavailable: code signatures differ; title=%s version=%s matched_before_cpu_rewrites=%d\n",serial,version,before_rewrites);
        for(unsigned i=0;i<sizeof(iss_karma_code_checks)/sizeof(iss_karma_code_checks[0]);++i){
            const IssKarmaCodeCheck *c=&iss_karma_code_checks[i];
            if(c->offset>size || c->length>size-c->offset){printf("Karma compatibility: eboot+0x%x outside image\n",c->offset);continue;}
            uint32_t actual=karma_hash(image+c->offset,c->length);
            if(actual!=c->expected)printf("Karma compatibility: eboot+0x%x length=0x%x expected=%08x actual=%08x\n",c->offset,c->length,c->expected,actual);
        }
        return;
    }
    unsigned char *bridge=allocate(page_size);
    unsigned n=iss_karma_bridge(bridge,(uintptr_t)runtime_karma_tick,image+ISS_KARMA_SITE,(uintptr_t)image+ISS_KARMA_SITE+ISS_KARMA_STOLEN);
    if(n>page_size)fail("karma bridge overflow");
    unsigned char *shard_bridge=bridge+2048;
    unsigned sn=iss_karma_bridge_ex(shard_bridge,(uintptr_t)runtime_karma_shard_event,image+0x1e2270,(uintptr_t)image+0x1e2284,20,1);
    if(sn>2048)fail("shard bridge overflow");
    unsigned char *purchase_bridge=bridge+3072;
    unsigned pn=iss_karma_bridge_ex(purchase_bridge,(uintptr_t)runtime_karma_purchase_event,image+0x231350,(uintptr_t)image+0x231364,20,1);
    if(pn>1024)fail("purchase bridge overflow");
    protect(bridge,page_size,5);memset(image+0x231350,0x90,20);
    iss_karma_jump(image+0x231350,(uintptr_t)purchase_bridge);
    memset(image+0x1e2270,0x90,20);
    iss_karma_jump(image+0x1e2270,(uintptr_t)shard_bridge);
    memset(image+ISS_KARMA_SITE,0x90,ISS_KARMA_STOLEN);
    iss_karma_jump(image+ISS_KARMA_SITE,(uintptr_t)bridge);runtime_karma_attach(image);
    puts("Karma switcher: supported CUSA00223 01.07 (normal game functions, game-thread queue)");
}

/* Opt-in, build/signature-guarded ISS camera angular-speed limits.
 * Installation runs after ISA/red-zone rewriting, before guest threads start. */
static void install_iss_camera_speed(const char *app0, Segment *segments, uint64_t count, uint64_t size) {
    float pan, tilt;
    if (!iss_camera_scale(getenv("ISS_CAMERA_PAN_SCALE"), &pan) ||
        !iss_camera_scale(getenv("ISS_CAMERA_TILT_SCALE"), &tilt)) {
        puts("Camera speed: disabled (scale must be a finite number from 0.25 to 4)");
        return;
    }
    if (pan == 1.0f && tilt == 1.0f) return;
    char path[4096], serial[32] = "", version[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    sfo_value(path, "APP_VER", version, sizeof(version), NULL);
    int executable = 0;
    for (uint64_t i=0; i<count; ++i)
        if ((segments[i].flags & 1) && segments[i].address <= ISS_CAMERA_SITE &&
            ISS_CAMERA_SITE-segments[i].address <= segments[i].size &&
            ISS_CAMERA_LENGTH <= segments[i].size-(ISS_CAMERA_SITE-segments[i].address)) executable = 1;
    if (!iss_camera_title_supported(serial,version) || !executable ||
        !iss_camera_signature_match(image,size)) {
        puts("Camera speed: disabled (title/version or original-code signature mismatch)");
        return;
    }
    unsigned char *stub = allocate(page_size);
    iss_camera_build_stub(stub,(uintptr_t)(image+ISS_CAMERA_SITE+ISS_CAMERA_LENGTH),pan,tilt);
    protect(stub,page_size,5);
    iss_camera_build_jump(image+ISS_CAMERA_SITE,(uintptr_t)stub);
    printf("Camera speed: horizontal %.3gx, vertical %.3gx (angular limits; adaptive gain unchanged)\n",pan,tilt);
}

static void install_iss_mouse_aim(const char *app0,Segment *segments,uint64_t count,uint64_t size){
    const char *enabled=getenv("ISS_DIRECT_MOUSE_AIM");
    if(!enabled || strcmp(enabled,"1"))return;
    char path[4096],serial[32]="",version[32]="";
    snprintf(path,sizeof(path),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
    sfo_value(path,"TITLE_ID",serial,sizeof(serial),NULL);sfo_value(path,"APP_VER",version,sizeof(version),NULL);
    int executable=0,writable=0;
    for(uint64_t i=0;i<count;i++){
        if((segments[i].flags&1) && segments[i].address<=0x14936eu &&
            ISS_MOUSE_SITE+ISS_MOUSE_STOLEN<=segments[i].address+segments[i].size)executable=1;
        if((segments[i].flags&2) && segments[i].address<=0xe16c40u &&
            0xe16c40u<segments[i].address+segments[i].size)writable=1;
    }
    if(strcmp(serial,"CUSA00223") || strcmp(version,"01.07") || !executable || !writable ||
        !iss_mouse_signature(image,size)){
        puts("Direct mouse: incompatible title/code/mapping; mouse-to-stick fallback retained");return;
    }
    unsigned char *bridge=allocate(page_size);
    unsigned n=iss_karma_bridge_ex(bridge,(uintptr_t)runtime_mouse_aim_camera,image+ISS_MOUSE_SITE,
        (uintptr_t)image+ISS_MOUSE_SITE+ISS_MOUSE_STOLEN,ISS_MOUSE_STOLEN,4);
    if(n>page_size)fail("Direct mouse bridge overflow");
    protect(bridge,page_size,5);
    memset(image+ISS_MOUSE_SITE,0x90,ISS_MOUSE_STOLEN);
    iss_karma_jump(image+ISS_MOUSE_SITE,(uintptr_t)bridge);
    runtime_mouse_aim_attach((uintptr_t)image);
    puts("Direct mouse: guarded US 1.07 angle input enabled; Controls sensitivity/invert apply; controller unchanged");
}

static void report_iss_pitch(void) {
    printf("Camera pitch safeguard: calls=%" PRIu64 " changes=%" PRIu64
           " last input=%.4g,%.4g pitch=%.4g zoom=%.4g cap=%.4g->%.4g\n",
           iss_pitch_stats.calls,iss_pitch_stats.changed,iss_pitch_stats.last_x,
           iss_pitch_stats.last_y,iss_pitch_stats.last_pitch,iss_pitch_stats.last_zoom,
           iss_pitch_stats.last_cap,iss_pitch_stats.last_result);
}
static void install_iss_pitch_safeguard(const char *app0, Segment *segments, uint64_t count, uint64_t size) {
    const char *enabled=getenv("ISS_CAMERA_PREVENT_PITCH_SLOWDOWN");
    if (!enabled || strcmp(enabled,"1")) return;
    char path[4096],serial[32]="",version[32]="";
    snprintf(path,sizeof(path),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
    sfo_value(path,"TITLE_ID",serial,sizeof(serial),NULL);
    sfo_value(path,"APP_VER",version,sizeof(version),NULL);
    int executable=0;
    for (uint64_t i=0;i<count;++i)
        if ((segments[i].flags&1) && segments[i].address<=ISS_PITCH_SITE &&
            ISS_PITCH_SITE-segments[i].address<=segments[i].size &&
            ISS_PITCH_LENGTH<=segments[i].size-(ISS_PITCH_SITE-segments[i].address)) executable=1;
    if (!iss_camera_title_supported(serial,version) || !executable ||
        !iss_pitch_signature(image,size)) {
        puts("Camera pitch safeguard: disabled (title/version or signature mismatch)");return;
    }
    unsigned char *stub=allocate(page_size);
    unsigned length=iss_pitch_build_stub(stub,(uintptr_t)&iss_pitch_cap,
                                        (uintptr_t)(image+ISS_PITCH_SITE+ISS_PITCH_LENGTH));
    if (length>ISS_PITCH_MAX_STUB || length>page_size) fail("camera pitch stub overflow");
    protect(stub,page_size,5);
    iss_pitch_build_jump(image+ISS_PITCH_SITE,(uintptr_t)stub);
    atexit(report_iss_pitch);
    puts("Camera pitch safeguard: enabled (unzoomed full diagonal at pitch limit only)");
}

/* Black city fix (from HayatoG/iss_pc beta 2026-11-23). The game installs from the disc in PlayGo
 * stages; each of its 71 world sections belongs to one chunk, and a section whose chunk is in a later
 * stage also waits for that stage's install package (section +0x3C from the world data; +0x46 = still
 * loading). The overcast "dismal" time of day lives in chunk 50 (pgc_i2), gated on install_i2, which
 * a PS4 loads once the Blu-ray install has finished. Here PlayGo reports every chunk local from the
 * start and nothing ever loads install_i2, so dismal never streams in: no time of day is active and
 * Seattle stays black on saves that want it (#71, #89). The section setup (eboot 0x3c3b10) reads the
 * wait at 0x3c3bf5 (sar ax,8; cwde); "xor eax,eax" there makes it 0, as on a fully installed PS4.
 * Applied only when the bytes match; ISS_INSTALL_GATE_FIX=0 turns it off. */
enum { ISS_INSTALL_GATE_SITE = 0x3c3bf0u };
static const unsigned char iss_install_gate_before[] = {
    0x66,0x41,0x8b,0x46,0x24, 0x66,0xc1,0xf8,0x08, 0x98, 0x89,0x43,0x3c, 0x49,0x8b,0x4e,0x20,
    0x89,0x4b,0x40, 0x48,0xc1,0xe9,0x31, 0x80,0xe1,0x01, 0x66,0x85,0xc0, 0x88,0x4b,0x48, 0x0f,0x95,0x43,0x46};
static uint32_t iss_install_gate_counts[256];
static void report_iss_install_gate(void) {
    printf("Black city fix: section install-package waits:");
    for (int i = 0; i < 256; ++i) if (iss_install_gate_counts[i]) printf(" %d=%u", i, iss_install_gate_counts[i]);
    printf("\n");
}
static void *iss_install_gate_watch(void *arg) {
    (void)arg;
    for (;;) { sleep(10); report_iss_install_gate(); fflush(stdout); }
    return NULL;
}
static void install_iss_install_gate(const char *app0, uint64_t size) {
    const char *off = getenv("ISS_INSTALL_GATE_FIX");
    const char *diag = getenv("ISS_INSTALL_GATE_LOG");
    if (off && off[0] == '0' && !(diag && diag[0] == '1')) { puts("Black city fix: off (ISS_INSTALL_GATE_FIX=0)"); return; }
    char path[4096], serial[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    if (strcmp(serial, "CUSA00223") && strcmp(serial, "CUSA00004")) return;
    if (size < ISS_INSTALL_GATE_SITE + sizeof(iss_install_gate_before) ||
        memcmp(image + ISS_INSTALL_GATE_SITE, iss_install_gate_before, sizeof(iss_install_gate_before))) {
        puts("Black city fix: not applied (game code not as expected); no writes");
        return;
    }
    const char *log = getenv("ISS_INSTALL_GATE_LOG");
    const int fix = !(off && off[0] == '0');
    if (log && log[0] == '1') {
        /* Diagnostics: count each section's install-package wait value; replays the 17 bytes it replaces. */
        unsigned char *stub = allocate(page_size), *q = stub;
        static const unsigned char head[] = {0x66,0x41,0x8b,0x46,0x24, 0x66,0xc1,0xf8,0x08, 0x98, 0x48,0xb9};
        memcpy(q, head, sizeof(head)); q += sizeof(head);
        uintptr_t table = (uintptr_t)iss_install_gate_counts; memcpy(q, &table, 8); q += 8;
        static const unsigned char count[] = {0x3d,0xff,0,0,0, 0x77,0x04, 0xf0,0xff,0x04,0x81};
        memcpy(q, count, sizeof(count)); q += sizeof(count);
        if (fix) { *q++ = 0x31; *q++ = 0xc0; }
        static const unsigned char tail[] = {0x89,0x43,0x3c, 0x49,0x8b,0x4e,0x20, 0xff,0x25,0,0,0,0};
        memcpy(q, tail, sizeof(tail)); q += sizeof(tail);
        uintptr_t back = (uintptr_t)image + ISS_INSTALL_GATE_SITE + 17; memcpy(q, &back, 8);
        protect(stub, page_size, 5);
        static const unsigned char jump[] = {0xff,0x25,0,0,0,0};
        memcpy(image + ISS_INSTALL_GATE_SITE, jump, 6);
        uintptr_t target = (uintptr_t)stub; memcpy(image + ISS_INSTALL_GATE_SITE + 6, &target, 8);
        memset(image + ISS_INSTALL_GATE_SITE + 14, 0x90, 3);
        pthread_t watcher; /* the game is usually ended by a signal: report every 10 s instead of atexit */
        if (!pthread_create(&watcher, NULL, iss_install_gate_watch, NULL)) pthread_detach(watcher);
        printf("Black city fix: %s, counting install-package waits (ISS_INSTALL_GATE_LOG=1)\n", fix ? "on" : "off");
        return;
    }
    static const unsigned char after[] = {0x31, 0xc0, 0x0f, 0x1f, 0x00}; /* xor eax,eax; nop dword [rax] */
    memcpy(image + ISS_INSTALL_GATE_SITE + 5, after, sizeof(after));
    puts("Black city fix: world sections do not wait for the Blu-ray install packages (dismal sky streams in)");
}

/* G3PATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"G3PATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
void runtime_restart(void) {
    fflush(NULL);
    puts("Runtime: restarting through run.sh");
#ifdef __APPLE__
    for (long fd=3, end=sysconf(_SC_OPEN_MAX); fd<end; ++fd) close((int)fd);
#else
    syscall(SYS_close_range, 3u, ~0u, 0u);
#endif
    execlp("bash", "bash", "run.sh", (char *)NULL);
    perror("runtime_restart: exec");
    _exit(1);
}

/* The guest code needs the PS4's instruction sets (#33, #66); TLG_CPU_CHECK=0 skips this. A
 * missing fatal set prints STOP and returns 0; the rewritable ones go to cpu_missing_sets, named
 * in missing. */
static int cpu_supported(char *missing, size_t size) {
    missing[0] = 0;
    const char *check = getenv("TLG_CPU_CHECK");
    if (check && !strcmp(check, "0")) return 1;
    const CpuFeatureWords words = cpu_feature_words();
    if (cpu_missing_fatal(words, missing, size)) {
        printf("STOP: this CPU lacks instruction sets the game's code uses: %s "
               "(Intel Sandy Bridge 2011 or newer, or AMD FX or newer, is needed)\n", missing);
        return 0;
    }
    cpu_missing_sets = cpu_missing_rewritable(words, missing, size);
    return 1;
}

/* Per-game environment prefix (ISS: ISS_). The core still reads TLG_* (renamed to a shared
 * prefix once every port uses ps4_core), so each ISS_X is copied to TLG_X at start, unless
 * TLG_X is set too. */
#ifndef PORT_ENV_PREFIX
#define PORT_ENV_PREFIX "ISS_"
#endif
extern char **environ;
static void alias_port_environment(void) {
    const size_t prefix = strlen(PORT_ENV_PREFIX);
    size_t count = 0;
    while (environ && environ[count]) ++count;
    char **copy = calloc(count + 1, sizeof(*copy)); /* setenv may reallocate environ */
    if (!copy) return;
    for (size_t i = 0; i < count; ++i) copy[i] = strdup(environ[i]);
    for (size_t i = 0; i < count; ++i) {
        char *entry = copy[i], *equals = entry ? strchr(entry, '=') : NULL;
        if (!equals || strncmp(entry, PORT_ENV_PREFIX, prefix) || equals == entry + prefix) continue;
        char name[256];
        const size_t length = (size_t)(equals - entry - prefix);
        if (length + 5 > sizeof(name)) continue;
        memcpy(name, "TLG_", 4);
        memcpy(name + 4, entry + prefix, length);
        name[4 + length] = 0;
        if (getenv(name)) continue;
        setenv(name, equals + 1, 0);
    }
    for (size_t i = 0; i < count; ++i) free(copy[i]);
    free(copy);
}

/* Detailed log mode (ISS_VERBOSE_LOG=1 or 2, copied to TLG_VERBOSE_LOG above): turns on the
 * bounded trace switches that are safe together and asks the GPU log for debug (1) or trace (2).
 * A variable the player already set is never overwritten. Switches left off on purpose, because
 * they change behavior or are heavy single-subsystem debugging: TLG_IMPORT_TRACE (reroutes every
 * import call through a stub), TLG_WRITE_LOG (ring buffer plus scans on every GPU write),
 * TLG_REGION_TRACE (writes a file), TLG_DEBUG_MOTION (draws a debug overlay), TLG_SCENE_DEBUG
 * (dumps frames and deletes the trigger file). */
static void verbose_default(const char *name, const char *value, char *list, size_t size) {
    if (getenv(name)) return;
    setenv(name, value, 0);
    size_t used = strlen(list);
    snprintf(list + used, size - used, "%s%s=%s", used ? ", " : "", name, value);
}
static void apply_verbose_log(void) {
    const char *mode = getenv("TLG_VERBOSE_LOG");
    const int level = mode ? atoi(mode) : 0;
    if (level <= 0) return;
    char list[512] = "";
    verbose_default("TLG_GPU_LOG", level >= 2 ? "trace" : "debug", list, sizeof list);
    verbose_default("TLG_AUDIO_TRACE", "1", list, sizeof list);        /* 400 lines: sound opens, Ajm */
    verbose_default("TLG_MOD_TRACE", "1", list, sizeof list);          /* 32 lines, only with TLG_MODS_DIR */
    verbose_default("TLG_OCCLUSION_LOG", "1", list, sizeof list);      /* first 48 events, then 1 in 2000 */
    verbose_default("TLG_MOTION_SELECT_LOG", "1", list, sizeof list);  /* one line per G-buffer shader */
    verbose_default("TLG_PIPELINE_LOG", "1", list, sizeof list);       /* one line per pipeline built */
    if (level >= 2) {
        verbose_default("TLG_TRACE_SEMA", "1", list, sizeof list);     /* capped at 2000 lines by the runtime */
        verbose_default("TLG_JSON_LOG", "1", list, sizeof list);       /* sce::Json serialize results */
        verbose_default("TLG_MOUSE_LOG", "1", list, sizeof list);      /* 4 lines/s while the mouse is used */
        verbose_default("TLG_RECORDER_TRACE", "1", list, sizeof list); /* top callers every 2000 recordings */
    }
    printf("Verbose log: on (level %d); switches: %s\n", level, list[0] ? list : "(all already set by the user)");
}
/* Detailed log mode: boot step with the time since the first one (ms). */
static void boot_step(const char *what) {
    if (!runtime_verbose()) return;
    static struct timespec start;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!start.tv_sec && !start.tv_nsec) start = now;
    printf("Verbose: boot step '%s' at +%ld ms\n", what,
           (long)((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000));
}

#ifndef TLG_BUILD_COMMIT
#define TLG_BUILD_COMMIT "unknown"
#endif
#ifdef __APPLE__
/* Cocoa (the SDL window and its events) only works on the main thread: the loader and the game
 * run on this thread instead, and main() runs the main run loop, which serves the main dispatch
 * queue (gpu/shim/tlggpu.cpp), as bloodborne_mac_port. dispatch_main() would not do: Cocoa then
 * finds no main thread (SDL: "No available video device"). */
static int probe_main(int argc, char **argv);
static int main_argc;
static char **main_argv;
static void *game_thread(void *unused) {
    (void)unused;
    /* TLG_MAC_GUEST_QOS=1: the game's main thread at user-interactive QoS (runtime_thread.c). */
    { const char *e=getenv("TLG_MAC_GUEST_QOS"); if (e && *e=='1') pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0); }
    exit(probe_main(main_argc, main_argv));
}
int main(int argc, char **argv) {
    main_argc = argc; main_argv = argv;
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20); /* the main thread's stack is 8 MiB; be generous */
    if (pthread_create(&thread, &attr, game_thread, NULL)) { perror("pthread_create"); return 1; }
    pthread_attr_destroy(&attr);
    /* SDL stops the main run loop at times (entering full screen): run it again. */
    for (;;) {
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0e10, false) == kCFRunLoopRunFinished)
            usleep(1000); /* no sources yet */
    }
}
static int probe_main(int argc, char **argv) {
#else
int main(int argc, char **argv) {
    if (getenv("TLG_START_TRACE")) { /* wall-clock marks of the start-up (vk_presenter.cpp StartMark) */
        struct timeval tv; gettimeofday(&tv,NULL);
        printf("Start trace: %lld tlg-probe main\n",(long long)tv.tv_sec*1000+tv.tv_usec/1000);
    }
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    alias_port_environment();
    apply_verbose_log();
    boot_step("start");
#ifdef __linux__
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits.
       macOS malloc lives far above 1 TiB: such objects must come from runtime_low_map. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    char missing[128];
    if (argc == 2 && !strcmp(argv[1], "--cpu-features")) {
        if (!cpu_supported(missing, sizeof missing)) return 20;
        if (cpu_missing_sets) printf("CPU: missing %s\n", missing);
        else puts("CPU: complete");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--gamepad")) {
        /* Before the memory reservation: this only asks SDL, nothing of the game is set up yet. */
        runtime_pad_describe();
        return 0;
    }
#if   defined(__APPLE__)
    /* Before SDL/MoltenVK map anything: on macOS large host mmaps land in the guest range. */
    runtime_memory_reserve();
#endif
    int cpu_only = 0, strict_imports = 0, check_imports = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL, *isa_file=NULL;
    const char *mounts[8]; int mount_count=0;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--check-imports")) check_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--isa-rewrite") && i+1<argc) isa_file=argv[++i];
        else if (!strcmp(argv[i], "--mount") && i+1<argc) {
            /* guest=host; the guest part never has '=', so the first one splits */
            const char *spec=argv[++i], *eq=strchr(spec,'=');
            if (mount_count==8 || !eq || eq==spec || !eq[1]) { fprintf(stderr, "Bad --mount (guest=host, at most 8): %s\n", spec); return 1; }
            mounts[mount_count++]=spec;
        }
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--check-imports] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--isa-rewrite file] [--mount guest=host]... [--timeout seconds] | --vulkan-only | --cpu-features | --gamepad\n", argv[0]);
        return 1;
    }
    {
        /* The first log line says which build and which game this is (players often can't tell
         * which zip they ran, or which dump: only CUSA00223 1.07 is supported). */
        char path[4096], serial[32] = "?", version[32] = "?";
        snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
        sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
        sfo_value(path, "APP_VER", version, sizeof(version), NULL);
        printf("Build: %s (built %s %s); game %s %s; threads inherit the creator's MXCSR/x87 control\n",
               TLG_BUILD_COMMIT, __DATE__, __TIME__, serial, version);
    }
    if (!cpu_supported(missing, sizeof missing)) return 20;
    if (cpu_missing_sets) printf("CPU: missing %s\n", missing);
    boot_step("cpu check done");
    /* TLG_ISA_REWRITE=0 never rewrites, =1 rewrites every emulated site (MOVBE, BMI1, LZCNT, F16C, SSE4A:
     * a test on a complete CPU), a list such as "sse4a", "f16c" or "movbe,bmi1" rewrites those sets; by
     * default the sets the CPU lacks. */
    const char *policy = getenv("TLG_ISA_REWRITE");
    int policy_valid;
    const unsigned isa_forced = cpu_sets_named(policy, &policy_valid);
    if (!policy_valid) printf("ISA: TLG_ISA_REWRITE=%s ignored (use 0, 1 or a list of movbe, bmi1, lzcnt, f16c, sse4a)\n", policy);
    const int forced = isa_forced != 0;
    unsigned isa_sets = forced ? isa_forced | cpu_missing_sets : policy && !strcmp(policy, "0") ? 0 : cpu_missing_sets;
    if (cpu_missing_sets && !isa_sets) {
        printf("STOP: this CPU lacks %s and the rewrite is disabled (TLG_ISA_REWRITE=0)\n", missing);
        return 20;
    }
    if (cpu_missing_sets && !isa_file) {
        printf("STOP: this CPU lacks %s and no rewrite table matches the image\n", missing);
        return 20;
    }
    /* Red-zone protection (scripts/red_zone.py, the table's G3RZONE1 section): guest code keeps data
     * below rsp (SysV red zone) and Windows' exception dispatch overwrites it on every fault (#29,
     * #41). On by default on Windows; TLG_RED_ZONE_REWRITE=1 forces it (Linux tests, with
     * TLG_CLOBBER_RED_ZONE), =0 turns it off. */
    const char *red_zone_policy = getenv("TLG_RED_ZONE_REWRITE");
    int red_zone_on = red_zone_policy && !strcmp(red_zone_policy, "1");
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"G3CONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        for (int i = 0; i < mount_count; ++i) {
            const char *eq = strchr(mounts[i], '=');
            char guest[64];
            size_t glen = (size_t)(eq - mounts[i]);
            if (glen >= sizeof(guest)) { fprintf(stderr, "Cannot mount %s: guest path longer than 63 characters\n", mounts[i]); return 1; }
            memcpy(guest, mounts[i], glen); guest[glen] = 0;
            if (runtime_file_mount(guest, eq + 1)) { fprintf(stderr, "Cannot mount %s: too many mounts or host path longer than 511 characters\n", mounts[i]); return 1; }
            printf("Runtime: mount %s -> %s\n", guest, eq + 1);
        }
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
    }
    boot_step("content mounted");
    tlggpu_register_kernel();
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    {
        const char *clobber = getenv("TLG_CLOBBER_RED_ZONE");
        long bytes = clobber ? strtol(clobber, NULL, 0) : 0;
        clobber_red_zone = bytes == 1 ? 128 : bytes > 0 && bytes <= 65536 ? (unsigned)bytes : 0;
        if (clobber_red_zone) {
            printf("Test: fault handler zeroes %u bytes below the guest rsp (TLG_CLOBBER_RED_ZONE)\n", clobber_red_zone);
            runtime_set_thread_start_hook(clobber_thread_init);
        }
    }
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO | (clobber_red_zone > 128 ? SA_ONSTACK : 0);
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL); /* int3 ISA sites */
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction request = {0}; request.sa_sigaction = dump_request; request.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&request.sa_mask); sigaction(SIGUSR1, &request, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "G3PROBE1", 8) && memcmp(magic, "G3PROBE2", 8) && memcmp(magic,"G3PROBE3",8) && memcmp(magic,"G3PROBE4",8) && memcmp(magic,"G3PROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "G3PROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    boot_step("boot file read, starting runtime");
    runtime_start(strict_imports ? 0 : capabilities);
    boot_step("runtime started");
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"G3PROBE5",8);
    int linked=!memcmp(magic,"G3PROBE3",8) || !memcmp(magic,"G3PROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* G3PROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"G3PROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    isa_table = (isa_sets || red_zone_on) && isa_file ? isa_table_load(isa_file) : NULL;
    if (!isa_table && cpu_missing_sets) { printf("STOP: this CPU lacks %s and no rewrite table matches the image\n", missing); return 20; }
    /* The stubs follow the image in the same low mapping (rel32 jmps reach them): the emulation
     * stubs, then the red-zone stub code at the offset scripts/red_zone.py encoded it for. */
    size_t stub_area = isa_table ? round_page(isa_stub_area_size(isa_table)) : 0;
    size_t red_zone_area = 0;
    if (red_zone_on) {
        if (!isa_table || !isa_table->rz) printf("Red zone: protection unavailable (no red-zone section in the rewrite table)\n");
        else if (isa_table->rz_code_offset != round_page(size) + stub_area) printf("Red zone: protection unavailable (stub offset does not match this page size)\n");
        else red_zone_area = round_page(isa_red_zone_code_size(isa_table));
        if (!red_zone_area) red_zone_on = 0;
    }
    image = allocate(round_page(size) + stub_area + red_zone_area);
    unsigned char *stubs = image + round_page(size);
    guest_code_end = (uintptr_t)image + round_page(size) + stub_area + red_zone_area;
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (isa_table && isa_table_matches(isa_table, image, size)) {
        if (cpu_missing_sets) {
            printf("STOP: this CPU lacks %s and no rewrite table matches the image\n", missing);
            return 20;
        }
        printf("Red zone: protection unavailable (the rewrite table is for another image)\n");
        isa_table = NULL;
        red_zone_on = 0;
    }
    if (!cpu_only) {
        char title[128]="ISS", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        TlgGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        boot_step("initializing GPU");
#ifdef __APPLE__
        /* TLG_GPU_PROCESS=1: the renderer runs in the native arm64 GPU process
         * (docs/macos/GPU_PROCESS_PLAN.md); the mirror replays the guest views mapped so far. */
        const char *split_env=getenv("TLG_GPU_PROCESS");
        if (split_env && split_env[0]=='1') {
            gpu_process_start();
            void *split=gpu_process_shared();
            if (!split) fail("the GPU process is not running");
            if (tlggpu_init_remote(&gpu,split)) fail("the renderer could not start in the GPU process");
            gpu_process_rendering();
        } else
#endif
        if (tlggpu_init(&gpu)) fail("GPU initialization failed");
        boot_step("GPU ready");
        /* TLG_TEST_NOTICE=1: an overlay notice and the input settings through the GPU library's
         * UI calls (a check of their forwarding with TLG_GPU_PROCESS=1). */
        if (getenv("TLG_TEST_NOTICE") && getenv("TLG_TEST_NOTICE")[0]=='1') {
            TlgGpuInputSettings in;
            tlggpu_input_settings(&in);
            printf("UI check: sensor aim %.2f, mouse %d, key map '%s'\n",
                   tlggpu_sensor_aim_scale(),in.mouse_enabled,in.key_map);
            tlggpu_overlay_notice("UI check: this notice came from the game process",120.0f);
        }
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,tlggpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    const char *import_trace = getenv("TLG_IMPORT_TRACE");
    if (import_trace && *import_trace) {
        trace_limit = atoi(import_trace) > 0 ? (uint32_t)atoi(import_trace) : 50;
        trace_stubs = allocate(round_page((import_count + 1) * TRACE_STUB_SIZE));
        trace_made = calloc(import_count + 1, sizeof(*trace_made));
        trace_counts = calloc(import_count + 1, sizeof(*trace_counts));
        if (!trace_made || !trace_counts) fail("allocation failed");
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved) {
                value = resolved + relocs[i].addend;
                if (trace_stubs && relocs[i].kind == 1 && !relocs[i].addend) value = trace_stub(relocs[i].value, resolved);
            } else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (check_imports) {
        /* Lists every import the game would stop at, without running it (needs the GPU
         * library: without --cpu-only its symbols are registered above). */
        unsigned char *checked=calloc(import_count ? import_count : 1,1);
        if (!checked) fail("allocation failed");
        uint64_t missing=0;
        for (uint64_t i=0;i<nr;++i) {
            uint64_t index=relocs[i].value;
            if (!relocs[i].kind || checked[index]) continue;
            checked[index]=1;
            if (runtime_resolve(names[index],relocs[i].kind==2) || (native_libc && bindings[index])) continue;
            printf("MISSING: %s (%s)\n",names[index],runtime_import_name(names[index]));
            ++missing;
        }
        printf("Imports: %" PRIu64 " missing\n",missing);
        fflush(NULL);
        _exit(missing ? 1 : 0); /* no destructors: GPU threads are running */
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    const int karma_before_runtime_rewrites=iss_karma_signatures(image,size);
    if (gpu_enabled) install_orb_multiplier(app0, segments, ns, size);
    if (gpu_enabled) install_cheats(app0, segments, ns, size);
    if (gpu_enabled) install_actor_watch(app0, segments, ns, size);
    if (gpu_enabled) install_stat_multipliers(app0, segments, ns, size);
    if (gpu_enabled) install_view_tuning(app0, segments, ns, size);
    if (gpu_enabled) install_iss_temporal(app0, segments, ns, size);
    install_exposure_log(size);
    if (isa_table) {
        isa_table->protect_red_zone = red_zone_on;
        IsaReport r = isa_rewrite_apply(isa_table, image, stubs, isa_sets);
        if (r.unsupported_needed) {
            printf("STOP: this CPU lacks %s and %u sites use encodings the rewrite cannot emulate\n", missing, r.unsupported_needed);
            fflush(NULL);
            _exit(20); /* no destructors: GPU threads are running */
        }
        if (forced) printf("ISA: forced rewrite of %u sites (TLG_ISA_REWRITE=%s; %u jmp, %u relocated, %u int3), %u skipped\n",
                           r.rewritten, policy, r.jmp, r.relocated, r.int3, r.skipped);
        else if (isa_sets) printf("ISA: CPU lacks %s: %u sites rewritten (%u jmp, %u relocated, %u int3), %u skipped\n",
                                  missing, r.rewritten, r.jmp, r.relocated, r.int3, r.skipped);
        if (red_zone_on) {
            const uint32_t *st = isa_table->rz_stats;   /* scripts/red_zone.py STATS_FIELDS */
            IsaRedZoneReport z = isa_red_zone_apply(isa_table, image, stubs + stub_area);
            printf("Red zone: %u instructions protected in %u patches (%u short-jump relays), %u patches skipped; "
                   "%u red-zone functions of %u (%u conservative); not protected: %u use rsp, %u control flow, "
                   "%u unrelocatable\n",
                   z.protected_instructions, z.groups, z.short_relays, z.skipped, st[1], st[0], st[2], st[7], st[8],
                   st[10]);
            if (r.int3) printf("Red zone: %u int3 ISA sites trap through the exception handler (not protected)\n", r.int3);
        }
        runtime_set_report_hook(isa_report_hook);
        protect(stubs, stub_area + red_zone_area, 5);
    }
    if (gpu_enabled) install_iss_native_resolution(app0, segments, ns, size);
    if (gpu_enabled) install_iss_hud_corners(app0, segments, ns, size);
    install_frames_ahead(segments, ns, size);
    if (gpu_enabled) install_iss_hud_trace(app0, segments, ns, size);
    if (gpu_enabled) install_iss_camera_speed(app0, segments, ns, size);
    if (gpu_enabled) install_iss_pitch_safeguard(app0, segments, ns, size);
    install_iss_install_gate(app0, size);
    if (gpu_enabled) install_iss_mouse_aim(app0, segments, ns, size);
    if (gpu_enabled) install_iss_karma(app0, segments, ns, size, karma_before_runtime_rewrites);
    if (gpu_enabled) iss_perf_patches(image, size); /* TLG_PERF_MODE, TLG_PARTICLE_LATE_READ, ... */
    protect(traps, round_page((import_count + 1) * 32), 5);
    if (trace_stubs) protect(trace_stubs, round_page((import_count + 1) * TRACE_STUB_SIZE), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    boot_step("image mapped and relocated");
    if (gpu_enabled) tlg_view_start(); /* its thread reads the image: only now that it is mapped again */
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    {   /* TLG_FREEZE_REPORT_SECONDS (default 10, 0 = off): no new frame for that long writes
         * every thread's wait, RIP and frames to the log (runtime_freeze.c). Needs the GPU. */
        const char *freeze = getenv("TLG_FREEZE_REPORT_SECONDS");
        unsigned seconds = freeze && *freeze ? (unsigned)strtoul(freeze, NULL, 10) : 10;
        runtime_freeze_start((uintptr_t)image, round_page(size) + stub_area + red_zone_area, gpu_enabled ? seconds : 0);
    }
    tlggpu_set_guest_image((uintptr_t)image, round_page(size));
    boot_step("entering the game");
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    clobber_thread_init();
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
    fail("entry unexpectedly returned");
    return 1;
}
