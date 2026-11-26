// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// tlg: the "Perf:" log line (docs/PERF_STRATEGY.md): every 5 s, key=value tokens a player's log and the
// launcher's Detailed benchmark read. Always on (TLG_PERF_LINE=0 turns it off); cheap: a few counters
// per flip, process and thread queries once per window.
namespace TlgPerfLine {
void OnFlip(); ///< call once per presented frame (GPU process)
}
