# GPU probes (macOS)

Small stand-alone programs and trace parsers behind the findings in AGENTS.md / CLAUDE.md.

| File | What it shows | Build / run |
|---|---|---|
| `mq.mm` | Two Metal queues: do a heavy job and a chain of 500 one-workgroup dispatches overlap? (No: A+B.) Cost of a tiny dispatch in a serial encoder (~2 us). | `clang++ -std=c++20 -O2 -fobjc-arc mq.mm -framework Metal -framework Foundation -o mq && ./mq 15000 500 400` |
| `mq2.mm` | Does a small compute job on a second queue overtake work queued on the first? (Yes: 0.25 ms past 130 ms.) | same, `mq2.mm` |
| `q2.cpp` + `heavy.comp`, `tiny.comp` | The same through Vulkan/MoltenVK (queue families 0 and 1); `NO_BARRIER=1`, `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` | `glslangValidator -V heavy.comp -o heavy.spv; glslangValidator -V tiny.comp -o tiny.spv; clang++ -std=c++20 -O2 -arch arm64 -I../../gpu/third_party/vulkan-headers/include q2.cpp -L../../deps/arm64/lib -lvulkan -Wl,-rpath,../../deps/arm64/lib -o q2; VK_DRIVER_FILES=../../deps/arm64/share/vulkan/icd.d/MoltenVK_icd.json ./q2 15000 500 20` |
| `mq3.mm` | Hazard tracking on a shared tracked buffer: the second queue's job waits for the first queue (~30 ms); untracked: ~4.7 ms. | same, `mq3.mm` |
| `mq4.mm` | Two `MTLBuffer`s over the same memory, tracked for the main queue and an untracked alias for the second: a tiny job passes 60 ms of work in 2.8 ms, data correct. | same, `mq4.mm` |
| `mq5.mm`, `mq5b.mm` | The CPU does not see GPU writes in the middle of a command buffer, not even at an encoder boundary. | same |
| `mq5c.mm` | `encodeSignalEvent` in the middle of a command buffer is seen at once (0.86 ms against 11.5 ms) and the data is visible by then: how `TLG_ACB_QUEUE` signals fences. | same, `mq5c.mm` |
| `time_profile.py` | CPU samples of `tlggpu-host` per thread and function (leaves, inclusive, `--callers`) from a Time Profiler trace | `scripts/macos/host_profile.sh <scene> <label>` records, exports and runs it |
| `trace_idle.py` | Why the GPU idles: gaps classed as starved (next command buffer committed during the gap) or held back, from a Metal System Trace | `python3 trace_idle.py x.xml` (export as for `trace_busy.py`) |
| `trace_busy.py` | GPU busy % per channel (vertex / fragment / compute) from a Metal System Trace | `xcrun xctrace export --input x.trace --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]' > gpu.xml; python3 trace_busy.py` (reads `gpu.xml`) |
| `trace_compute.py` | GPU time per compute shader: needs a MoltenVK built with the split-dispatch diagnostic (each dispatch its own labelled encoder; described in CLAUDE.md, not in `patches/`) and `TLG_DRAW_LABELS=1` | `python3 trace_compute.py gpu.xml` |
