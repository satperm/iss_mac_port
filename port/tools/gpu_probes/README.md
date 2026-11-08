# GPU probes (macOS)

Small stand-alone programs and trace parsers behind the findings in AGENTS.md / CLAUDE.md.

| File | What it shows | Build / run |
|---|---|---|
| `mq.mm` | Two Metal queues: do a heavy job and a chain of 500 one-workgroup dispatches overlap? (No: A+B.) Cost of a tiny dispatch in a serial encoder (~2 us). | `clang++ -std=c++20 -O2 -fobjc-arc mq.mm -framework Metal -framework Foundation -o mq && ./mq 15000 500 400` |
| `mq2.mm` | Does a small compute job on a second queue overtake work queued on the first? (Yes: 0.25 ms past 130 ms.) | same, `mq2.mm` |
| `q2.cpp` + `heavy.comp`, `tiny.comp` | The same through Vulkan/MoltenVK (queue families 0 and 1); `NO_BARRIER=1`, `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` | `glslangValidator -V heavy.comp -o heavy.spv; glslangValidator -V tiny.comp -o tiny.spv; clang++ -std=c++20 -O2 -arch arm64 -I../../gpu/third_party/vulkan-headers/include q2.cpp -L../../deps/arm64/lib -lvulkan -Wl,-rpath,../../deps/arm64/lib -o q2; VK_DRIVER_FILES=../../deps/arm64/share/vulkan/icd.d/MoltenVK_icd.json ./q2 15000 500 20` |
| `trace_busy.py` | GPU busy % per channel (vertex / fragment / compute) from a Metal System Trace | `xcrun xctrace export --input x.trace --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]' > gpu.xml; python3 trace_busy.py` (reads `gpu.xml`) |
| `trace_compute.py` | GPU time per compute shader: needs a MoltenVK built with the split-dispatch diagnostic (each dispatch its own labelled encoder; described in CLAUDE.md, not in `patches/`) and `TLG_DRAW_LABELS=1` | `python3 trace_compute.py gpu.xml` |
