// SPDX-License-Identifier: GPL-2.0-or-later
// The temporal upscaling hooks of the game side (src/iss_temporal_hook.h): the signature check against
// the real eboot, the jumps written into the image, and the stubs EXECUTED on the host with the registers
// the game has at the two sites, so the machine code is run and not only compared with bytes.
//
// The eboot is not in the repository: set ISS_TEST_EBOOT to its path (CUSA00223 01.07, the update's
// eboot.bin) or keep the workspace layout (../../dump/patch0/eboot.bin). Without it the tests that
// need the real bytes are skipped.
#include "../src/iss_temporal_hook.h"
#include "../gpu_v2/gpu/iss_temporal_math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

static int failures = 0;
static int skipped = 0;
#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #condition);                  \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

// ---------------------------------------------------------------------------- pure functions
static void TestHalton() {
    CHECK(Near(iss_temporal_halton(1, 2), 0.5f));
    CHECK(Near(iss_temporal_halton(2, 2), 0.25f));
    CHECK(Near(iss_temporal_halton(3, 2), 0.75f));
    CHECK(Near(iss_temporal_halton(4, 2), 0.125f));
    CHECK(Near(iss_temporal_halton(1, 3), 1.0f / 3.0f));
    CHECK(Near(iss_temporal_halton(2, 3), 2.0f / 3.0f));
    CHECK(Near(iss_temporal_halton(3, 3), 1.0f / 9.0f));
    for (uint32_t i = 1; i <= 64; ++i) {   // always inside (0, 1)
        CHECK(iss_temporal_halton(i, 2) > 0.0f && iss_temporal_halton(i, 2) < 1.0f);
        CHECK(iss_temporal_halton(i, 3) > 0.0f && iss_temporal_halton(i, 3) < 1.0f);
    }
}

static void TestDecodeImage() {
    // base 0x123456700 (dword0 = 0x01234567, dword1 low bits 0), 1920 x 1080
    uint32_t t[8] = {0x01234567u, 0x00000000u, (1920u - 1) | ((1080u - 1) << 14), 0, 0, 0, 0, 0};
    IssTemporalImage image;
    iss_temporal_decode_image(reinterpret_cast<const unsigned char*>(t), &image);
    CHECK(image.valid == 1);
    CHECK(image.width == 1920 && image.height == 1080);
    CHECK(image.base == 0x0123456700ull);
    CHECK(image.descriptor == reinterpret_cast<uint64_t>(t));
    t[1] = 0x3f;   // base bits 32..37
    iss_temporal_decode_image(reinterpret_cast<const unsigned char*>(t), &image);
    CHECK(image.base == ((0x3full << 32 | 0x01234567ull) << 8));
    uint32_t bad[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // no base, 1 x 1
    iss_temporal_decode_image(reinterpret_cast<const unsigned char*>(bad), &image);
    CHECK(image.valid == 0);
    iss_temporal_decode_image(nullptr, &image);
    CHECK(image.valid == 0 && image.descriptor == 0);
}

static int growth_calls = 0;
static unsigned char* grow_buffer = nullptr;
static char ISS_TEMPORAL_ABI Grow(void* writer, uint32_t dwords, void* argument) {
    ++growth_calls;
    unsigned char* w = static_cast<unsigned char*>(writer);
    if (argument == nullptr) return 0;                    // refuses
    // a new chunk with room for `dwords`
    unsigned char* end = grow_buffer + 4096;
    std::memcpy(w + 8, &end, sizeof(end));
    std::memcpy(w + 0x10, &grow_buffer, sizeof(grow_buffer));
    (void)dwords;
    return 1;
}

static void TestMarker() {
    IssTemporalFrame frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.seq = 41;
    frame.reset = 1;
    frame.render_width = 1920;
    frame.render_height = 1080;
    frame.jitter[0] = 0.25f;
    frame.jitter[1] = -1.0f / 6.0f;
    for (int i = 0; i < 16; ++i) frame.camera.m470[i] = 0.5f + float(i);
    frame.camera.near_z = 10.0f;
    frame.camera.far_z = 1.0e7f;
    frame.camera.fov = 0.9f;
    frame.aa_input.base = 0x0123456700ull;
    frame.aa_output.base = 0x3f89abcd00ull;

    std::vector<unsigned char> context(0x100, 0);
    std::vector<uint32_t> room(80, 0xdeadbeef);
    unsigned char* end = reinterpret_cast<unsigned char*>(room.data() + 80);
    unsigned char* cursor = reinterpret_cast<unsigned char*>(room.data() + 8);
    std::memcpy(&context[8 + 8], &end, sizeof(end));          // writer = context + 8: end at +8
    std::memcpy(&context[8 + 0x10], &cursor, sizeof(cursor)); // cursor at +0x10
    CHECK(iss_temporal_write_marker(context.data(), &frame) == 1);
    CHECK(room[8] == 0xc0201000u);   // type 3, count 34 - 2, opcode 0x10 (NOP)
    CHECK(room[7] == 0xdeadbeef && room[8 + ISS_TEMPORAL_MARKER_DWORDS] == 0xdeadbeef);
    unsigned char* after;
    std::memcpy(&after, &context[8 + 0x10], sizeof(after));
    CHECK(after == reinterpret_cast<unsigned char*>(room.data() + 8 + ISS_TEMPORAL_MARKER_DWORDS));

    // the renderer's own parser (gpu_v2/gpu/iss_temporal_math.h) reads it back
    Vulkan::IssTemporal::Frame parsed;
    CHECK(Vulkan::IssTemporal::ParseMarker(&room[9], ISS_TEMPORAL_MARKER_PAYLOAD_DWORDS, parsed));
    CHECK(parsed.frame_id == 41 && (parsed.flags & Vulkan::IssTemporal::kFlagCameraCut) &&
          (parsed.flags & Vulkan::IssTemporal::kFlagReversedDepth));
    CHECK(parsed.render_width == 1920 && parsed.render_height == 1080);
    CHECK(Near(parsed.jitter[0], -0.25f) && Near(parsed.jitter[1], -1.0f / 6.0f));   // the content moves the other way in x
    for (int i = 0; i < 16; ++i) CHECK(Near(parsed.view_proj[i], 0.5f + float(i)));
    CHECK(Near(parsed.near_z, 10.0f) && Near(parsed.far_z, 1.0e7f) && Near(parsed.fov_y, 0.9f));
    // the colour to upscale is what the game's AA left (its output); the input is information only
    CHECK(parsed.color_addr == 0x3f89abcd00ull && parsed.output_addr == 0x3f89abcd00ull && parsed.depth_addr == 0);

    // not enough room: the game's own grow function is asked, and a refusal writes nothing
    std::vector<uint32_t> chunk(1024, 0);
    grow_buffer = reinterpret_cast<unsigned char*>(chunk.data());
    cursor = reinterpret_cast<unsigned char*>(room.data() + 70);
    std::memcpy(&context[8 + 0x10], &cursor, sizeof(cursor));
    void* grow = reinterpret_cast<void*>(&Grow);
    void* argument = &growth_calls;
    std::memcpy(&context[8 + 0x18], &grow, sizeof(grow));
    std::memcpy(&context[8 + 0x20], &argument, sizeof(argument));
    frame.seq = 42;
    CHECK(iss_temporal_write_marker(context.data(), &frame) == 1);
    CHECK(growth_calls == 1 && chunk[0] == 0xc0201000u && chunk[3] == 42);
    cursor = reinterpret_cast<unsigned char*>(room.data() + 70);
    std::memcpy(&context[8 + 0x10], &cursor, sizeof(cursor));
    end = reinterpret_cast<unsigned char*>(room.data() + 80);
    std::memcpy(&context[8 + 8], &end, sizeof(end));
    argument = nullptr;                                       // Grow refuses
    std::memcpy(&context[8 + 0x20], &argument, sizeof(argument));
    CHECK(iss_temporal_write_marker(context.data(), &frame) == 0);
    CHECK(room[70] == 0xdeadbeef);
}

// A fake image with the few places begin_frame / end_frame / aa_begin read.
static std::vector<unsigned char> fake_image;

static void TestFrameLifecycle() {
    fake_image.assign(0x2000000, 0);
    IssTemporalState& s = iss_temporal_state;
    s = IssTemporalState{};
    s.image = fake_image.data();
    uint32_t size[2] = {1920, 1080};
    std::memcpy(&fake_image[ISS_TEMPORAL_RENDER_SIZE], size, 8);
    s.installed = 1;
    iss_temporal_update_active();
    CHECK(s.active == 0);               // the renderer is not ready
    s.ready = 1;
    iss_temporal_update_active();
    CHECK(s.active == 1);

    float window[4];
    iss_temporal_begin_frame(window);
    // phase 0 = Halton index 1: (0.5 - 0.5, 1/3 - 0.5) = (0, -1/6)
    CHECK(Near(window[0], -1.0f) && Near(window[2], 1.0f));
    CHECK(Near(window[1], -1.0f + 2.0f * (-1.0f / 6.0f) / 1080.0f));
    CHECK(Near(window[3], 1.0f + 2.0f * (-1.0f / 6.0f) / 1080.0f));
    IssTemporalFrame f;
    CHECK(iss_temporal_copy_frame(1, &f) == 1);
    CHECK(f.seq == 1 && f.reset == 1 && f.phase == 0 && f.render_width == 1920 && f.render_height == 1080);
    CHECK(Near(f.jitter[0], 0.0f) && Near(f.jitter[1], 1.0f / 3.0f - 0.5f));

    // the camera, patterned so every block can be told apart
    std::vector<float> camera(0x700 / 4);
    for (size_t i = 0; i < camera.size(); ++i) camera[i] = float(i);
    iss_temporal_end_frame(reinterpret_cast<unsigned char*>(camera.data()));
    CHECK(iss_temporal_copy_frame(1, &f) == 1);
    CHECK(f.camera.address == reinterpret_cast<uint64_t>(camera.data()));
    CHECK(Near(f.camera.proj[0], float(0x1c0 / 4)) && Near(f.camera.proj[15], float(0x1c0 / 4 + 15)));
    CHECK(Near(f.camera.m200[0], float(0x200 / 4)) && Near(f.camera.m240[0], float(0x240 / 4)));
    CHECK(Near(f.camera.vec420[3], float(0x420 / 4 + 3)));
    CHECK(Near(f.camera.m430[0], float(0x430 / 4)) && Near(f.camera.m470[15], float(0x470 / 4 + 15)));
    CHECK(Near(f.camera.near_z, float(0x584 / 4)) && Near(f.camera.far_z, float(0x588 / 4)));
    CHECK(Near(f.camera.fov, float(0x590 / 4)) && Near(f.camera.aspect, float(0x59c / 4)));
    CHECK(Near(f.camera.window[0], float(0x5ac / 4)) && Near(f.camera.window[3], float(0x5ac / 4 + 3)));

    // the AA hooks: begin notes the images and writes nothing; done, after the AA, writes the marker
    std::vector<unsigned char> context(0x100, 0);
    std::vector<uint32_t> room(80, 0);
    unsigned char* end = reinterpret_cast<unsigned char*>(room.data() + 80);
    unsigned char* cursor = reinterpret_cast<unsigned char*>(room.data());
    std::memcpy(&context[16], &end, sizeof(end));
    std::memcpy(&context[24], &cursor, sizeof(cursor));
    std::vector<unsigned char> in_object(0x100, 0), out_object(0x100, 0), blank(0x100, 0);
    uint32_t t_in[8] = {0x100000, 0, (1919u) | (1079u << 14), 0, 0, 0, 0, 0};
    uint32_t t_out[8] = {0x200000, 0, (2559u) | (1439u << 14), 0, 0, 0, 0, 0};
    std::memcpy(&in_object[0xb4], t_in, sizeof(t_in));
    std::memcpy(&out_object[0xb4], t_out, sizeof(t_out));

    iss_temporal_aa_done(context.data());                          // no AA seen: nothing is written
    CHECK(room[0] == 0);
    iss_temporal_aa_begin(context.data(), in_object.data(), out_object.data());
    CHECK(room[0] == 0);                                          // before the AA: still nothing in the stream
    CHECK(iss_temporal_copy_frame(1, &f) == 1);
    CHECK(f.aa_replaced == 0 && f.aa_input.valid && f.aa_output.valid);
    CHECK(f.aa_input.width == 1920 && f.aa_output.width == 2560 && f.aa_output.height == 1440);
    CHECK(f.aa_input.base == (0x100000ull << 8) && f.aa_output.descriptor ==
          reinterpret_cast<uint64_t>(out_object.data() + 0xb4));
    unsigned char* other_context = context.data() + 1;
    iss_temporal_aa_done(other_context);                          // another command buffer: not ours
    CHECK(room[0] == 0);
    iss_temporal_aa_begin(context.data(), in_object.data(), out_object.data());
    iss_temporal_aa_done(context.data());                         // after the AA
    CHECK(room[0] == 0xc0201000u && room[1] == (ISS_TEMPORAL_MARKER_MAGIC | ISS_TEMPORAL_MARKER_KIND_RESOLVE) &&
          room[2] == ISS_TEMPORAL_MARKER_VERSION && room[3] == 1);
    CHECK(room[28] == (0x200000u << 8) && room[32] == (0x200000u << 8));   // colour == output
    CHECK(iss_temporal_copy_frame(1, &f) == 1 && f.aa_replaced == 1);
    CHECK(f.camera.address != 0);                                 // the camera survived the AA hooks
    const uint32_t first_marker_end = room[35];
    iss_temporal_aa_done(context.data());                         // once per AA: a second call writes nothing
    CHECK(room[34] == 0 && first_marker_end == 0);

    // an output that is not an image: no marker, the game's AA is all that runs
    std::fill(room.begin(), room.end(), 0u);
    cursor = reinterpret_cast<unsigned char*>(room.data());
    std::memcpy(&context[24], &cursor, sizeof(cursor));
    iss_temporal_aa_begin(context.data(), in_object.data(), blank.data());
    iss_temporal_aa_done(context.data());
    CHECK(room[0] == 0);
    iss_temporal_aa_begin(nullptr, in_object.data(), out_object.data());
    iss_temporal_aa_done(nullptr);
    CHECK(room[0] == 0);
    // a begin from an older frame is stale once the next frame is open
    iss_temporal_aa_begin(context.data(), in_object.data(), out_object.data());
    iss_temporal_begin_frame(window);
    iss_temporal_aa_done(context.data());
    CHECK(room[0] == 0);

    // phases cycle; the frame after a render size change asks for a reset; the ring forgets old frames
    s.phases = 4;
    uint32_t phase_seen[12];
    for (int i = 0; i < 12; ++i) {
        iss_temporal_begin_frame(window);
        IssTemporalFrame g;
        CHECK(iss_temporal_copy_frame(s.seq, &g) == 1);
        phase_seen[i] = g.phase;
        CHECK(g.reset == 0);
    }
    for (int i = 4; i < 12; ++i) CHECK(phase_seen[i] == phase_seen[i - 4]);
    // 3x upscaling asks for 8 * 9 = 72 phases (the limit was 32); more than the limit is cut to it
    s.phases = 72;
    uint32_t big_phase[150];
    for (int i = 0; i < 150; ++i) {
        iss_temporal_begin_frame(window);
        IssTemporalFrame g;
        CHECK(iss_temporal_copy_frame(s.seq, &g) == 1);
        big_phase[i] = g.phase;
        CHECK(g.phase < 72);
    }
    for (int i = 72; i < 150; ++i) CHECK(big_phase[i] == big_phase[i - 72]);
    s.phases = 100000;
    for (int i = 0; i < 300; ++i) {
        iss_temporal_begin_frame(window);
        IssTemporalFrame g;
        CHECK(iss_temporal_copy_frame(s.seq, &g) == 1);
        CHECK(g.phase < ISS_TEMPORAL_MAX_PHASES);
    }
    s.phases = 4;
    size[0] = 1280;
    size[1] = 720;
    std::memcpy(&fake_image[ISS_TEMPORAL_RENDER_SIZE], size, 8);
    iss_temporal_begin_frame(window);
    CHECK(iss_temporal_copy_frame(s.seq, &f) == 1 && f.reset == 1 && f.render_width == 1280);
    CHECK(iss_temporal_copy_frame(1, &f) == 0);            // 13 frames ago: out of the ring
    CHECK(iss_temporal_copy_frame(s.seq + 1, &f) == 0);    // not opened yet
    CHECK(iss_temporal_copy_frame(0, &f) == 0);

    // the game's own no-AA state: no jitter, still a frame
    fake_image[ISS_TEMPORAL_FRAME_OBJECT + ISS_TEMPORAL_NO_AA_FLAG] = 1;
    iss_temporal_begin_frame(window);
    CHECK(Near(window[0], -1.0f) && Near(window[1], -1.0f) && Near(window[2], 1.0f) && Near(window[3], 1.0f));
    CHECK(iss_temporal_copy_frame(s.seq, &f) == 1 && Near(f.jitter[0], 0.0f) && Near(f.jitter[1], 0.0f));
    s = IssTemporalState{};
}

// ------------------------------------------------------------------- the stubs, executed on the host
#if defined(__x86_64__) || defined(_M_X64)
struct Regs {
    uint64_t rcx, rdi, rbp, r12, r13, r14, r15;   // 0 .. 48: loaded before the stub
    uint64_t id;                                  // 56: which landing pad the stub reached
    uint64_t saved_rsp, done;                     // 64, 72
    uint64_t rax;                                 // 80: rax at the landing pad
    uint64_t rbx;                                 // 88: rbx at the landing pad
};
static Regs g_regs;                               // the landing pads know its address

extern "C" __attribute__((sysv_abi)) void iss_test_run_stub(const void* stub, Regs* regs);
__asm__(
    ".text\n"
    ".globl iss_test_run_stub\n"
    "iss_test_run_stub:\n"
    "    pushq %rbp\n    pushq %rbx\n    pushq %r12\n    pushq %r13\n    pushq %r14\n    pushq %r15\n"
    "    movq %rsi, %rbx\n"
    "    leaq 1f(%rip), %rax\n"
    "    movq %rax, 72(%rbx)\n"
    "    subq $8, %rsp\n"            // rsp is 16-byte aligned when the stub starts, as at the game's site
    "    movq %rsp, 64(%rbx)\n"
    "    movq %rdi, %r11\n"
    "    movq 0(%rbx), %rcx\n    movq 8(%rbx), %rdi\n    movq 16(%rbx), %rbp\n    movq 24(%rbx), %r12\n"
    "    movq 32(%rbx), %r13\n    movq 40(%rbx), %r14\n    movq 48(%rbx), %r15\n"
    "    jmp *%r11\n"
    "1:\n"
    "    addq $8, %rsp\n"
    "    popq %r15\n    popq %r14\n    popq %r13\n    popq %r12\n    popq %rbx\n    popq %rbp\n"
    "    ret\n");

static unsigned char* AllocateExecutable(size_t size) {
#if defined(_WIN32)
    return static_cast<unsigned char*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<unsigned char*>(p);
#endif
}

// Landing pad (uses only r11, which every stub leaves free): remember rax and rbx, set the id, go back to the
// trampoline. rbx is the game's register at the DONE site (the lea loads it), so the pad cannot rely on it.
static void MakePad(unsigned char* out, uint32_t id) {
    unsigned char code[64];
    unsigned n = 0;
    code[n++] = 0x49;
    code[n++] = 0xbb;                                        // movabs $&g_regs,%r11
    const uint64_t regs = reinterpret_cast<uint64_t>(&g_regs);
    std::memcpy(code + n, &regs, 8);
    n += 8;
    const unsigned char tail[] = {
        0x49, 0x89, 0x43, 0x50,                              // mov %rax,0x50(%r11)
        0x49, 0x89, 0x5b, 0x58,                              // mov %rbx,0x58(%r11)
        0xb8, uint8_t(id), uint8_t(id >> 8), uint8_t(id >> 16), uint8_t(id >> 24), // mov $id,%eax
        0x49, 0x8b, 0x63, 0x40,                              // mov 0x40(%r11),%rsp
        0x49, 0x89, 0x43, 0x38,                              // mov %rax,0x38(%r11)
        0x41, 0xff, 0x63, 0x48};                             // jmp *0x48(%r11)
    std::memcpy(code + n, tail, sizeof(tail));
    n += sizeof(tail);
    std::memcpy(out, code, n);
}

static std::vector<char> call_log;
static float begin_window[4] = {0.125f, 0.25f, 0.375f, 0.5f};
static const void* seen_begin_window = nullptr;
static const void* seen_camera_set = nullptr;
static const void* seen_camera_end = nullptr;
static float seen_window[4];
static const void* seen_ctx = nullptr;
static const void* seen_in = nullptr;
static const void* seen_out = nullptr;
static const void* seen_done_ctx = nullptr;

static ISS_TEMPORAL_ABI void MockBegin(float* window) {
    call_log.push_back('B');
    seen_begin_window = window;
    std::memcpy(window, begin_window, sizeof(begin_window));
}
static ISS_TEMPORAL_ABI void MockSetWindow(const void* camera, const float* window) {
    call_log.push_back('W');
    seen_camera_set = camera;
    std::memcpy(seen_window, window, sizeof(seen_window));
}
static ISS_TEMPORAL_ABI void MockEnd(const void* camera) {
    call_log.push_back('E');
    seen_camera_end = camera;
}
static ISS_TEMPORAL_ABI void MockAaBegin(const void* ctx, const void* in, const void* out) {
    call_log.push_back('A');
    seen_ctx = ctx;
    seen_in = in;
    seen_out = out;
}
static ISS_TEMPORAL_ABI void MockAaDone(const void* ctx) {
    call_log.push_back('D');
    seen_done_ctx = ctx;
}

static void ResetMocks() {
    call_log.clear();
    seen_begin_window = seen_camera_set = seen_camera_end = seen_ctx = seen_in = seen_out = seen_done_ctx = nullptr;
    std::memset(seen_window, 0, sizeof(seen_window));
}

static std::string Log() { return std::string(call_log.begin(), call_log.end()); }

static void TestExecutedStubs() {
    unsigned char* page = AllocateExecutable(4096);
    if (!page) {
        std::puts("skipped: no executable memory");
        ++skipped;
        return;
    }
    enum { Target = 1, Fall = 2, AaNext = 3, DoneNext = 4 };
    unsigned char* pads = page + 2048;
    for (uint32_t i = 1; i <= 4; ++i) MakePad(pads + i * 64, i);

    uint8_t active = 0;
    uint8_t aa_mode = 0;
    unsigned char camera_object[0x100] = {};
    const void* camera_pointer = camera_object;
    std::vector<unsigned char> frame_object(0xe00000, 0);      // frame_object + 0xdf0f05 is read
    std::vector<unsigned char> post_object(0x20000, 0);        // r14: + 0x1eb8d is read
    const uint64_t rbx_value = 0x5eed0000cafeull;
    IssTemporalTargets t{};
    t.active = reinterpret_cast<uintptr_t>(&active);
    t.begin_frame = reinterpret_cast<uintptr_t>(&MockBegin);
    t.end_frame = reinterpret_cast<uintptr_t>(&MockEnd);
    t.aa_begin = reinterpret_cast<uintptr_t>(&MockAaBegin);
    t.aa_done = reinterpret_cast<uintptr_t>(&MockAaDone);
    t.camera_pointer = reinterpret_cast<uintptr_t>(&camera_pointer);
    t.set_window = reinterpret_cast<uintptr_t>(&MockSetWindow);
    t.aa_mode = reinterpret_cast<uintptr_t>(&aa_mode);
    t.frame_object = reinterpret_cast<uintptr_t>(frame_object.data());
    t.jitter_target = reinterpret_cast<uintptr_t>(pads + Target * 64);
    t.jitter_fall = reinterpret_cast<uintptr_t>(pads + Fall * 64);
    t.aa_next = reinterpret_cast<uintptr_t>(pads + AaNext * 64);
    t.done_next = reinterpret_cast<uintptr_t>(pads + DoneNext * 64);
    t.done_rbx = rbx_value;

    unsigned char* jitter_stub = page;
    unsigned char* aa_stub = page + 512;
    unsigned char* done_stub = page + 1024;
    const unsigned jitter_size = iss_temporal_build_jitter_stub(jitter_stub, &t);
    const unsigned aa_size = iss_temporal_build_aa_stub(aa_stub, &t);
    const unsigned done_size = iss_temporal_build_done_stub(done_stub, &t);
    CHECK(jitter_size > 0 && jitter_size <= ISS_TEMPORAL_STUB_MAX);
    CHECK(aa_size > 0 && aa_size <= ISS_TEMPORAL_STUB_MAX);
    CHECK(done_size > 0 && done_size <= ISS_TEMPORAL_STUB_MAX);
    std::printf("stubs: jitter %u bytes, aa %u bytes, done %u bytes\n", jitter_size, aa_size, done_size);

    alignas(16) unsigned char game_frame[0x200] = {};
    Regs& regs = g_regs;

    // --- jitter site: the game's pair when not active
    ResetMocks();
    active = 0;
    aa_mode = 0;
    regs = {};
    regs.rcx = reinterpret_cast<uint64_t>(&aa_mode);
    regs.rbp = reinterpret_cast<uint64_t>(game_frame + 0x100);
    iss_test_run_stub(jitter_stub, &regs);
    CHECK(regs.id == Target && call_log.empty());                 // FXAA mode: jz taken
    aa_mode = 1;
    iss_test_run_stub(jitter_stub, &regs);
    CHECK(regs.id == Fall && call_log.empty() && aa_mode == 1);   // T2x mode: falls through, untouched

    // --- jitter site: active
    ResetMocks();
    active = 1;
    aa_mode = 1;
    iss_test_run_stub(jitter_stub, &regs);
    CHECK(regs.id == Target);
    CHECK(Log() == "BWE");
    CHECK(seen_begin_window == game_frame + 0x100 - 0x40);        // the game's own window slot
    CHECK(seen_camera_set == camera_object && seen_camera_end == camera_object);
    CHECK(Near(seen_window[0], 0.125f) && Near(seen_window[1], 0.25f) && Near(seen_window[2], 0.375f) &&
          Near(seen_window[3], 0.5f));
    CHECK(aa_mode == 0);                                          // no T2x for the rest of the frame
    aa_mode = 0;                                                  // active, game in FXAA mode: same
    ResetMocks();
    iss_test_run_stub(jitter_stub, &regs);
    CHECK(regs.id == Target && Log() == "BWE");

    // --- AA site: the game's AA ALWAYS runs (the stub always ends in its two instructions and aa_next)
    regs = {};
    regs.r12 = 0x1111;
    regs.r13 = 0x2222;
    regs.r14 = reinterpret_cast<uint64_t>(post_object.data());
    regs.r15 = 0x3333;
    unsigned char* no_aa = &frame_object[ISS_TEMPORAL_NO_AA_FLAG];
    unsigned char* second = &post_object[0x1eb8d];
    ResetMocks();
    active = 0;
    *no_aa = 7;
    iss_test_run_stub(aa_stub, &regs);
    CHECK(regs.id == AaNext && (regs.rax & 0xff) == 7 && call_log.empty());   // the game's two instructions
    *no_aa = 0;
    iss_test_run_stub(aa_stub, &regs);
    CHECK(regs.id == AaNext && (regs.rax & 0xff) == 0 && call_log.empty());

    ResetMocks();
    active = 1;
    iss_test_run_stub(aa_stub, &regs);                            // notes the images, then the game's AA
    CHECK(regs.id == AaNext && (regs.rax & 0xff) == 0 && Log() == "A");
    CHECK(seen_ctx == reinterpret_cast<void*>(0x2222) && seen_in == reinterpret_cast<void*>(0x1111) &&
          seen_out == reinterpret_cast<void*>(0x3333));

    ResetMocks();
    *no_aa = 5;                                                   // the game runs no AA: left alone
    iss_test_run_stub(aa_stub, &regs);
    CHECK(regs.id == AaNext && (regs.rax & 0xff) == 5 && call_log.empty());
    *no_aa = 0;
    *second = 1;                                                  // its second skip flag
    iss_test_run_stub(aa_stub, &regs);
    CHECK(regs.id == AaNext && call_log.empty());
    *second = 0;

    // --- DONE site: after the AA. The lea the jump replaced is redone with an absolute address.
    regs = {};
    regs.r13 = 0x2222;
    regs.rbx = 0;
    ResetMocks();
    active = 0;
    iss_test_run_stub(done_stub, &regs);
    CHECK(regs.id == DoneNext && regs.rbx == rbx_value && call_log.empty());
    ResetMocks();
    active = 1;
    iss_test_run_stub(done_stub, &regs);
    CHECK(regs.id == DoneNext && regs.rbx == rbx_value && Log() == "D" && seen_done_ctx == reinterpret_cast<void*>(0x2222));

    // --- the order of a frame: the AA hook, then (the game's AA) then the DONE hook
    ResetMocks();
    regs = {};
    regs.r12 = 0x1111;
    regs.r13 = 0x2222;
    regs.r14 = reinterpret_cast<uint64_t>(post_object.data());
    regs.r15 = 0x3333;
    iss_test_run_stub(aa_stub, &regs);
    call_log.push_back('G');                                      // the game's FXAA pass runs here
    iss_test_run_stub(done_stub, &regs);
    CHECK(Log() == "AGD");
}

// The real host functions behind the real stubs: the marker lands in the command stream after the AA.
static void TestMarkerAfterTheAa() {
    unsigned char* page = AllocateExecutable(4096);
    if (!page) {
        ++skipped;
        return;
    }
    enum { AaNext = 3, DoneNext = 4 };
    unsigned char* pads = page + 2048;
    MakePad(pads + AaNext * 64, AaNext);
    MakePad(pads + DoneNext * 64, DoneNext);

    fake_image.assign(0x2000000, 0);
    IssTemporalState& s = iss_temporal_state;
    s = IssTemporalState{};
    s.image = fake_image.data();
    s.installed = 1;
    s.ready = 1;
    iss_temporal_update_active();
    uint32_t size[2] = {1920, 1080};
    std::memcpy(&fake_image[ISS_TEMPORAL_RENDER_SIZE], size, 8);
    float window[4];
    iss_temporal_begin_frame(window);

    std::vector<unsigned char> context(0x100, 0);
    std::vector<uint32_t> room(80, 0);
    unsigned char* end = reinterpret_cast<unsigned char*>(room.data() + 80);
    unsigned char* cursor = reinterpret_cast<unsigned char*>(room.data());
    std::memcpy(&context[16], &end, sizeof(end));
    std::memcpy(&context[24], &cursor, sizeof(cursor));
    std::vector<unsigned char> in_object(0x100, 0), out_object(0x100, 0);
    uint32_t t_in[8] = {0x100000, 0, (1919u) | (1079u << 14), 0, 0, 0, 0, 0};
    uint32_t t_out[8] = {0x200000, 0, (1919u) | (1079u << 14), 0, 0, 0, 0, 0};
    std::memcpy(&in_object[0xb4], t_in, sizeof(t_in));
    std::memcpy(&out_object[0xb4], t_out, sizeof(t_out));

    IssTemporalTargets t{};
    t.active = reinterpret_cast<uintptr_t>(&s.active);
    t.aa_begin = reinterpret_cast<uintptr_t>(&iss_temporal_aa_begin);
    t.aa_done = reinterpret_cast<uintptr_t>(&iss_temporal_aa_done);
    t.frame_object = reinterpret_cast<uintptr_t>(fake_image.data() + ISS_TEMPORAL_FRAME_OBJECT);
    t.aa_next = reinterpret_cast<uintptr_t>(pads + AaNext * 64);
    t.done_next = reinterpret_cast<uintptr_t>(pads + DoneNext * 64);
    t.done_rbx = 1;
    unsigned char* aa_stub = page;
    unsigned char* done_stub = page + 512;
    iss_temporal_build_aa_stub(aa_stub, &t);
    iss_temporal_build_done_stub(done_stub, &t);

    std::vector<unsigned char> post_object(0x20000, 0);
    Regs& regs = g_regs;
    regs = {};
    regs.r12 = reinterpret_cast<uint64_t>(in_object.data());
    regs.r13 = reinterpret_cast<uint64_t>(context.data());
    regs.r14 = reinterpret_cast<uint64_t>(post_object.data());
    regs.r15 = reinterpret_cast<uint64_t>(out_object.data());
    iss_test_run_stub(aa_stub, &regs);
    CHECK(regs.id == AaNext);
    CHECK(room[0] == 0);                                      // the AA has not run yet: no marker
    room[0] = 0x11111111u;                                    // the game's AA commands go here
    cursor = reinterpret_cast<unsigned char*>(room.data() + 1);
    std::memcpy(&context[24], &cursor, sizeof(cursor));
    iss_test_run_stub(done_stub, &regs);
    CHECK(regs.id == DoneNext);
    CHECK(room[0] == 0x11111111u && room[1] == 0xc0201000u);  // the marker comes after them
    CHECK(room[2] == (ISS_TEMPORAL_MARKER_MAGIC | ISS_TEMPORAL_MARKER_KIND_RESOLVE));
    CHECK(room[29] == (0x200000u << 8) && room[33] == (0x200000u << 8));
    IssTemporalFrame f;
    CHECK(iss_temporal_copy_frame(1, &f) == 1 && f.aa_replaced == 1);
    s = IssTemporalState{};
}
#endif

// ------------------------------------------------------------------------- the real eboot bytes
static std::vector<unsigned char> LoadImage() {
    std::vector<unsigned char> image;
    const char* path = std::getenv("ISS_TEST_EBOOT");
    FILE* f = std::fopen(path && *path ? path : "../../dump/patch0/eboot.bin", "rb");
    if (!f) return image;
    // segment 0 (text + rodata): virtual address v is at file offset v + 0x4000, up to 0xcc55b8 bytes
    const size_t text = 0xcc55b8;
    image.assign(size_t(ISS_TEMPORAL_CAMERA_POINTER) + 0x10, 0);   // the bss reaches the camera pointer
    std::fseek(f, 0x4000, SEEK_SET);
    const size_t got = std::fread(image.data(), 1, text, f);
    std::fclose(f);
    if (got != text) image.clear();
    return image;
}

static void TestRealEboot() {
    std::vector<unsigned char> image = LoadImage();
    if (image.empty()) {
        std::puts("skipped: the eboot is not at ISS_TEST_EBOOT or ../../dump/patch0/eboot.bin");
        ++skipped;
        return;
    }
    CHECK(iss_temporal_signature_match(image.data(), image.size()) == 1);
    CHECK(iss_temporal_signature_match(image.data(), 0x400000) == 0);          // too short
    for (unsigned i = 0; i < sizeof(iss_temporal_signatures) / sizeof(iss_temporal_signatures[0]); ++i) {
        std::vector<unsigned char> copy = image;
        copy[iss_temporal_signatures[i].at + iss_temporal_signatures[i].length / 2] ^= 0xff;
        CHECK(iss_temporal_signature_match(copy.data(), copy.size()) == 0);    // every entry is checked
    }

    // every site holds the instructions the notes at the top of the header say
    CHECK(image[ISS_TEMPORAL_JITTER_SITE] == 0x80 && image[ISS_TEMPORAL_JITTER_SITE + 3] == 0x0f);
    // the jz displacement points at the target: 0x472944 + 3 + 6 + 0x93
    CHECK(ISS_TEMPORAL_JITTER_SITE + 9 + 0x93 == ISS_TEMPORAL_JITTER_TARGET);
    CHECK(ISS_TEMPORAL_JITTER_SITE + 9 == ISS_TEMPORAL_JITTER_FALL);
    // the lea at the AA site: disp 0x00d8907d from the next instruction
    CHECK(ISS_TEMPORAL_AA_SITE + 7 + 0xd8907d == ISS_TEMPORAL_FRAME_OBJECT);
    CHECK(ISS_TEMPORAL_AA_SITE + ISS_TEMPORAL_AA_LENGTH == ISS_TEMPORAL_AA_NEXT);
    // the or/jnz after it: jnz +0x99 from 0x192c60 + 6 lands on the instruction after the AA call, the DONE site
    CHECK(0x192c60 + 6 + 0x99 == ISS_TEMPORAL_DONE_SITE);
    CHECK(ISS_TEMPORAL_DONE_SITE + ISS_TEMPORAL_DONE_LENGTH == ISS_TEMPORAL_DONE_NEXT);
    CHECK(ISS_TEMPORAL_DONE_SITE + 7 + 0x7bb91a2 == ISS_TEMPORAL_DONE_RBX);   // the lea the stub redoes

    // installing: only the two sites change, each into jmp rel32 + nops reaching a stub of this size
    std::vector<unsigned char> stubs(4096);
    iss_temporal_state = IssTemporalState{};
    std::vector<unsigned char> patched = image;
    // the stubs must be within a rel32 jump of the image: a vector of this size is, on the heap, only by luck
    if (!iss_temporal_jump_in_range(patched.data() + ISS_TEMPORAL_JITTER_SITE, stubs.data()) ||
        !iss_temporal_jump_in_range(patched.data() + ISS_TEMPORAL_AA_SITE, stubs.data() + ISS_TEMPORAL_STUB_MAX) ||
        !iss_temporal_jump_in_range(patched.data() + ISS_TEMPORAL_DONE_SITE, stubs.data() + 2 * ISS_TEMPORAL_STUB_MAX)) {
        std::puts("skipped: the heap put the stubs out of rel32 reach of the image");
        ++skipped;
        return;
    }
    CHECK(iss_temporal_install_hooks(patched.data(), patched.size(), stubs.data()) == 1);
    size_t changed = 0;
    for (size_t i = 0; i < image.size(); ++i) changed += image[i] != patched[i];
    int32_t rel;
    CHECK(changed <= ISS_TEMPORAL_JITTER_LENGTH + ISS_TEMPORAL_AA_LENGTH + ISS_TEMPORAL_DONE_LENGTH);
    CHECK(patched[ISS_TEMPORAL_JITTER_SITE] == 0xe9 && patched[ISS_TEMPORAL_AA_SITE] == 0xe9 &&
          patched[ISS_TEMPORAL_DONE_SITE] == 0xe9);
    for (unsigned i = 5; i < ISS_TEMPORAL_DONE_LENGTH; ++i) CHECK(patched[ISS_TEMPORAL_DONE_SITE + i] == 0x90);
    std::memcpy(&rel, &patched[ISS_TEMPORAL_DONE_SITE + 1], 4);
    CHECK(patched.data() + ISS_TEMPORAL_DONE_SITE + 5 + rel == stubs.data() + 2 * ISS_TEMPORAL_STUB_MAX);
    for (unsigned i = 5; i < ISS_TEMPORAL_JITTER_LENGTH; ++i) CHECK(patched[ISS_TEMPORAL_JITTER_SITE + i] == 0x90);
    for (unsigned i = 5; i < ISS_TEMPORAL_AA_LENGTH; ++i) CHECK(patched[ISS_TEMPORAL_AA_SITE + i] == 0x90);
    std::memcpy(&rel, &patched[ISS_TEMPORAL_JITTER_SITE + 1], 4);
    CHECK(patched.data() + ISS_TEMPORAL_JITTER_SITE + 5 + rel == stubs.data());
    std::memcpy(&rel, &patched[ISS_TEMPORAL_AA_SITE + 1], 4);
    CHECK(patched.data() + ISS_TEMPORAL_AA_SITE + 5 + rel == stubs.data() + ISS_TEMPORAL_STUB_MAX);
    CHECK(iss_temporal_state.installed == 1 && iss_temporal_state.active == 0 && iss_temporal_state.image == patched.data());
    iss_temporal_state.ready = 1;
    iss_temporal_update_active();
    CHECK(iss_temporal_state.active == 1);

    // a mismatch writes nothing
    std::vector<unsigned char> other = image;
    other[ISS_TEMPORAL_AA_SITE] ^= 1;
    std::vector<unsigned char> before = other;
    CHECK(iss_temporal_install_hooks(other.data(), other.size(), stubs.data()) == 0 && other == before);
    iss_temporal_state = IssTemporalState{};
}

int main() {
    TestHalton();
    TestDecodeImage();
    TestMarker();
    TestFrameLifecycle();
#if defined(__x86_64__) || defined(_M_X64)
    TestExecutedStubs();
    TestMarkerAfterTheAa();
#else
    std::puts("skipped: the stubs are x86-64 code");
    ++skipped;
#endif
    TestRealEboot();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("iss temporal hooks: ok (%d skipped)\n", skipped);
    return EXIT_SUCCESS;
}
