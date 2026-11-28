# Two Vulkan recorders (design, 2026-11-28)

Owner-approved project. Goal: more FPS at 1080p (and Pro-class Macs), where the single Vulkan recording thread
(`tlg:VkRecorder`, MoltenVK encoding to Metal as commands are recorded, `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2`)
is the most sensitive stage: +200 ns per recorded command costs 26% FPS on the truck (REPORT_2026-11-26) and 12% at
1440p (REPORT_2026-11-28).

## How recording works today (vk_scheduler.cpp / .h)

- Stage B (`tlg:DrawRec`) calls `Scheduler::Record(func)`: closures go into `RecordChunk`s, handed to the recorder
  queue by `KickRecording`. `RecorderThread` runs the chunks in order: `chunk->Execute(current_cmdbuf)`.
- A submission (`SubmitExecution`, async path, `TLG_ASYNC_SUBMIT` default on) is itself a recorded closure: on the
  recorder thread it waits for host copies (`host_copies_done`, `TlgCopy::WaitAsync`), ends `current_cmdbuf`,
  submits it (signals `work_semaphore` = the tick), and begins a new one from `command_pool`.
  Submissions happen every ~64 draws at a render-pass boundary (`SubmitIfWorkPiledUp`), ~58 a frame, and at
  guest submission ends, fences, readbacks.
- State does not cross command buffers: `EndRendering()` before each submission, `dynamic_state.Invalidate()`
  after it; a new command buffer starts with nothing bound.
- Direct mode: `CommandBuffer()` (and `Record` while `direct_mode`) syncs with the recorder (`SyncRecording`) and
  records on the caller's thread into `current_cmdbuf`, until the next `KickRecording`.
- Some recorded closures have side effects besides commands, in stream order: `SignalAfterHostCopies` (guest fence
  labels via `TlgCopy::AfterCopies`, counted by `deferred_signals_done`; the guest frees memory once a label is
  written: labels must stay in order, see `DeferUnifiedFence`), the submission closure (host-copy waits), and
  possibly others (audit: closures whose `vk::CommandBuffer` parameter is unnamed or unused, and closures touching
  scheduler members or atomics).

## Design

`TLG_RECORDERS=2` (default 1 = today's code path, unchanged). Only the main graphics scheduler; the companion
(ACB, `own_queue`) and presenter schedulers keep one recorder.

1. **Segments.** The chunk stream is cut at submission closures: segment *k* = the commands of command buffer *k*,
   ending with its submission. Segments alternate between recorder 0 and recorder 1 (round robin). Each recorder
   has its own `CommandPool` (Vulkan pools are externally synchronised) and its own current command buffer.
2. **Submission order.** The submission closure of segment *k* waits until segment *k-1* has been submitted
   (a monotonically increasing `submitted_segment` atomic + wait/notify), then submits. Ticks stay in order.
3. **Ordered side effects.** Any closure with a side effect other than recording into its command buffer runs only
   after every earlier segment has *finished* (all its closures run, including its submission). Implement as an
   explicit API, e.g. `RecordOrdered(func)`, and convert every such call site found by the audit; recorded commands
   themselves (draws, binds, barriers, passes) need no ordering across command buffers.
4. **Direct mode.** `SyncRecording` waits for both recorders to be idle and for all queued segments to be
   submitted; direct recording then uses the command buffer of the segment that is current for the producer.
5. **Chunk handoff.** `KickRecording` must route chunks to the recorder that owns the segment they belong to; a
   segment may span several chunks. Keep the producer side (stage B) lock-free on the hot path as today.
6. **Diagnostics.** `TLG_FRAME_STATS` lines keep working; add per-recorder busy time and the time a recorder waited
   for submission order or for ordered side effects (to see whether the alternation pays).

## Not in scope

Tracy GPU profiling, `TLG_FRAME_RECORD`/capture paths: may force one recorder (fall back to 1 when active).

## Acceptance

- Default (`TLG_RECORDERS` unset) is today's behaviour: same code path, FPS within noise (lib_ab alternated).
- `build_macos.sh --test`, `build_gpu_arm64.sh`, launcher tests pass.
- With `TLG_RECORDERS=2`: truck, Seattle, Seattle turning (1080p) and bridge 4K run 75 s each without crash or
  critical lines; screenshots within the run-to-run noise (`ppmdiff.py`).
- Worth shipping only if truck or Seattle 1080p gains >= 5% (3 x 70 s alternated), with no loss elsewhere.
- Soaks: Seattle 1080p 60 min and the burning village 4K 30 min with `TLG_RECORDERS=2`: no crash, memory flat,
  held frames as today.

## Risks

Out-of-order label writes (guest heap corruption after minutes: the reason for the audit and the soaks); MoltenVK
or AGX serialising concurrent encoding internally (then no gain: measure early with a minimal version); more
command pools and Metal command buffers in flight (`MVK_CONFIG_MAX_ACTIVE_METAL_COMMAND_BUFFERS_PER_QUEUE=512`).

## Result (2026-11-28): no gain, not merged

Implemented on branch `two-recorders` (1bbc672): `TLG_RECORDERS=N` (1..4), segments round robin, in-order
submission, `RecordOrdered` for side effects (`SignalAfterHostCopies`; host-copy counting reworked), descriptor
push cache cleared per buffer, fallbacks for frame record/capture/profiler. No crashes in the smoke runs.

Truck 1080p, 2 x 70 s alternated: 1 recorder 89.2 / 89.2, 2 recorders 88.3 / 88.1 (-1.2%; us/draw 1.22 -> 1.27).
Each recorder was busy ~1.35 s per 5 s (~27%), waiting for order only ~30 ms: together the same ~55% one recorder
was busy before. The recorder never had a backlog: the -26% of the causal test is the *latency* each command adds
on the way from the draw-preparation stage to the GPU (the game keeps one frame in flight), not throughput.
Parallel buffers do not shorten one buffer's encoding, so they cannot help. What could: less work per command in
MoltenVK/AGX, or fewer commands. Branch kept for reference.

Re-measured on a quiet machine (owner's apps and System Settings closed), 3 x 70 s alternated: truck 87.6 [87.6
87.6 88.5] vs 87.8 [87.3 88.6 87.8]; Seattle 87.4 [87.7 87.4 87.3] vs 88.4 [87.2 88.4 90.1]. Within the noise, far
from the 5% bar; recorders busy 1.3-1.5 s per 5 s each, order waits 30-50 ms. Conclusion unchanged.
