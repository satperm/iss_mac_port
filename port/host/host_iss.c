/* ISS-specific runtime functions the GPU library calls, in the native GPU host
 * (docs/macos/GPU_PROCESS_PLAN.md, M3). The Karma switcher, the native resolution plan and mouse
 * aim live in the game process; until they are forwarded the host reports them unavailable. */
#include <stdint.h>
#include <stdlib.h>
#include "src/iss_karma.h"
#include "src/iss_native_resolution.h"

const char *runtime_file_user_dir(void) {
    const char *dir = getenv("TLG_HOST_USER_DIR");
    return dir && *dir ? dir : "user";
}

static const char unavailable[] = "not available with the native GPU process yet";
const char *runtime_karma_compatibility_reason(void) { return unavailable; }
void runtime_karma_load_begin(void) {}
void runtime_karma_request(int mode) { (void)mode; }
void runtime_karma_set_enabled(int enabled) { (void)enabled; }
void runtime_karma_status(IssKarmaStatus *out) { *out = (IssKarmaStatus){0}; }

int runtime_native_status(void) { return 0; }
const char *runtime_native_reason(void) { return unavailable; }
const IssNativePlan *runtime_native_plan(void) {
    static const IssNativePlan none;
    return &none;
}

int runtime_mouse_aim_installed(void) { return 0; }
void runtime_report(void) {}

/* The guest's TSC as the game sees it under Rosetta: the counter scaled to 1 GHz
 * (gpu/shadps4/common/rdtsc.h). */
uint64_t runtime_read_tsc(void) {
    uint64_t ticks, frequency;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(ticks));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    return frequency == 1000000000 ? ticks : (uint64_t)((unsigned __int128)ticks * 1000000000 / frequency);
}
