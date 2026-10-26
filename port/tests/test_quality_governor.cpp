// SPDX-License-Identifier: GPL-2.0-or-later
// The automatic quality governor (gpu/shim/tlg_quality.h): it climbs when the frame rate stays under the
// target, ignores menus, gives levels back slowly and does not flap.
#include "../gpu/shim/tlg_quality.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            std::printf("FAIL line %d: %s\n", __LINE__, #condition);                           \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

using TlgQuality::Governor;

static int Run(Governor& g, int seconds, int target, double fps, double draws = 2000.0) {
    int level = g.Level();
    for (int i = 0; i < seconds; ++i) {
        level = g.Update(target, fps, draws);
    }
    return level;
}

int main() {
    {   // Off: stays at 0 whatever the frame rate.
        Governor g;
        CHECK(Run(g, 60, 0, 10.0) == 0);
    }
    {   // On target: no change.
        Governor g;
        CHECK(Run(g, 60, 30, 31.0) == 0);
    }
    {   // Slightly slow: climbs one level at a time, never past the maximum.
        Governor g;
        CHECK(Run(g, 3, 30, 27.0) == 1);          // two slow seconds, then the first step
        CHECK(Run(g, 4, 30, 27.0) == 2);          // cooldown of 3 s between steps
        CHECK(Run(g, 600, 30, 27.0) == TlgQuality::MaxLevel);
    }
    {   // Very slow: three levels at once.
        Governor g;
        CHECK(Run(g, 2, 30, 15.0) == 3);
    }
    {   // Menus and loading screens (few draws) never move it.
        Governor g;
        CHECK(Run(g, 60, 30, 5.0, 100.0) == 0);
    }
    {   // Room to spare for a long time gives levels back, one every few seconds; a short burst does not.
        Governor g;
        Run(g, 2, 30, 15.0);
        CHECK(g.Level() == 3);
        Run(g, 5, 30, 55.0);
        CHECK(g.Level() == 3);                    // 5 s is not long enough
        Run(g, 40, 30, 55.0);
        CHECK(g.Level() == 0);
    }
    {   // Giving a level back and being slow again soon blocks further attempts for a while.
        Governor g;
        Run(g, 2, 30, 15.0);                      // level 3
        Run(g, 9, 30, 55.0);                      // lowers to 2
        CHECK(g.Level() == 2);
        Run(g, 4, 30, 25.0);                      // slow right away: back to 3 and blocked
        CHECK(g.Level() == 3);
        Run(g, 30, 30, 55.0);                     // room to spare, but blocked for 40 s
        CHECK(g.Level() == 3);
        Run(g, 40, 30, 55.0);
        CHECK(g.Level() < 3);
    }
    {   // Turning it off resets it.
        Governor g;
        Run(g, 2, 30, 15.0);
        CHECK(g.Update(0, 15.0, 2000.0) == 0);
        CHECK(g.Level() == 0);
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("quality governor: ok");
    return EXIT_SUCCESS;
}
