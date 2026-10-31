// tlg: GPU-side assertion failures stop the port with exit code 23.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include "common/assert.h"
#include "common/assert_scope.h"
#include "common/logging/log.h"

namespace Common {
thread_local int assert_throws = 0;
}

void assert_fail_impl() {
    if (Common::assert_throws) {
        throw std::runtime_error("assertion failed (see GPU log above)");
    }
    std::fflush(stdout);
#ifdef __APPLE__
    // MVK_CONFIG_DEBUG=1: MoltenVK prints a lost command buffer's encoders from its completion
    // handler, after the submit or wait that fails here: let it finish.
    if (const char* debug = std::getenv("MVK_CONFIG_DEBUG"); debug && *debug == '1') {
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
#endif
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
