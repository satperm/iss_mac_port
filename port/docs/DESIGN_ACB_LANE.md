# A compute lane in the draw pipe (design, 2026-11-18)

## Why

On the truck (M3 Max, 82 FPS) the game's main thread works ~32% of the frame and waits ~48% for the GPU
particle chain (label idx 2 at guest 0x1695e0). `TLG_CHAIN_LOG=1`'s "last 64 fences" timeline shows that the
game's compute-queue work arrives in bursts (up to ~27 fences in under 1 ms), then:

| Step | Median | p90 |
|---|---|---|
| First fence seen by the GPU command thread -> run on the draw-recording thread | 0.64 ms | **6.2 ms** |
| -> last fence submitted | 1.5 ms | 6.2 ms |
| -> last fence done on the GPU | 3.0 ms | 6.5 ms |

The first step is the draw-recording thread (stage B) working through graphics draws that stage A queued
before the burst. PS4 orders its compute queues against graphics only through labels, so compute packets
need not wait for those draws.

Tried without gain (2026-11-18): compute flush grouping 4/16/64, flushing when the compute queue idles,
unordered GDS, more or fewer graphics submissions.

## Idea

A second ring in `DrawPipe` for compute-queue packets (`TLG_ACB_LANE=1`). Stage B serves it first: it checks
the compute ring before each graphics packet. Order between the lanes is kept only where the game syncs.

## What breaks if packets are simply reordered, and the fix for each

1. **Register deltas.** `PostDraw` writes the register blocks changed since the previous packet
   (`liverpool->pipe_dirty`) into every packet, compute or graphics. A compute packet run early would
   apply graphics deltas ahead of the graphics packets queued before it. Fix: lane packets carry no blocks.
   Leave `pipe_dirty` for the next graphics packet. The dispatch then has to read only the
   `ComputeProgram` carried in the packet (check that `DispatchRecord` and `BindResources` on the compute
   path never use `Regs()`).
2. **Constant ring stamps.** `RunDrawPacket` stamps `constant_ring->Position()` (`ring_end`) after each
   packet, so stage A can reclaim ring space, and the stamps assume order. Fix: lane packets record their
   own ring end, and the reclaim uses the minimum of the two lanes' stamps. Or the lane copies constants
   into its own small ring.
3. **Pending fence positions.** `Liverpool::PendingFenceValue` treats a fence as written once
   `DrawPipeReached(position)`, with a position in the single ring. Fix: fences carry (lane, position)
   and `Reached` checks the right lane.
4. **Cross-lane waits.** A compute `WAIT_REG_MEM` met by a pending graphics fence (`wait_met_by_graphics_fence`)
   relies on the graphics fence being recorded first. Fix: the next compute-lane packet carries the
   graphics position it needs, and stage B runs graphics packets until that point before it. Same in
   reverse for `wait_met_by_compute_fence` on the graphics queue.
5. **Drains.** `Drain`, `Idle`, `Head` and `Consumed` are used by stage A for "stage B ran dry". Fix: both
   lanes, everywhere they are used.
6. **Main-queue fallback.** `AcbTarget` may keep a dispatch on the main queue: images bound, `uses_dma`,
   capture, or full barriers. Such a dispatch would then record into the graphics command buffer at a
   different point. Barrier tracking follows the actual recording order, so hazards stay correct, but
   a render pass ends there. Measure how often this happens (`TLG_ACB_PACKETS` "on the main queue" was 0
   on the truck).
7. **Scratch state.** Stage B's per-call vectors are reset per packet on one thread. The lane runs on
   the same thread, so they stay safe.
8. **Exclusions.** Frame capture and frame record stay single-lane, because they read passes in order.

## Steps

1. `DrawPipe`: a second ring with its own `published`, `consumed` and `head`, served first. Add the
   cross-lane gate (a packet header field "needs other lane at position").
2. `PostDraw` and `RunInOrder` from the compute coroutine go to the lane. The coroutine runs on stage A,
   so mark the active queue while it is resumed. No register blocks in lane packets.
3. Constant ring stamps per lane; pending fences with a lane; waits record the gate.
4. Drains, `Idle` and `Reached` over both lanes.
5. Behind `TLG_ACB_LANE=1`; measure with `TLG_CHAIN_LOG` (the p90 above) and lib_ab on four scenes;
   then the stability suite (chase 30 FPS cap 240 s for issue #11, smoke, chase), the 9-minute soak and
   screenshots (smoke and sparks must look the same).

## Result of the prototype (2026-11-18 afternoon, branch `agent/acb-lane`, not merged)

Built as designed: two lanes, sequence numbers, gates (graphics only on compute fences), no register blocks in
lane 1, constant ring stamps from lane 0 only when lane 1 holds nothing older, and a compute flush when lane 1
runs dry (without it, fences waited ~1 ms for the 64-fence flush because stage B never idled after a burst).

| | Base | Lane on |
|---|---|---|
| Compute fence, seen -> recorded (median) | 0.61 ms | 0.08 ms |
| Compute fence total (median) | 2.35 ms | 1.5 ms |
| Truck FPS (lib_ab 3 x 60 s) | 80.6 | 78.5 |
| Burning village FPS | 74.5 | 73.1 |
| Main thread samples in the particle-label wait (10 s) | 1271 | 1401 |

The median fence latency fell by a third, yet the game waited longer and the frame rate dropped ~2%. Serving
compute first delays the graphics recording. The game's wait is for the last fences of the frame, so the
median does not describe it. Open questions before another try: which fence the main thread waits on (its
address/index per frame against the chain timeline), and whether a fair interleave helps instead of strict
priority (e.g. lane 1 only when its oldest packet waited more than N us). Without the constant ring, compute
packets cost twice as much on the recording thread (0.61 -> 1.19 ms): keep it.
