# Upstream and credit

This repository is a fork of **[HayatoG/iss_pc](https://github.com/HayatoG/iss_pc)** (native Linux and
Windows port of ISS, GPL-2.0), made to add macOS on Apple Silicon. HayatoG welcomed
the fork in [iss_pc#717](https://github.com/HayatoG/iss_pc/issues/717). All of iss_pc's code, history
and credits are kept; `port/UPSTREAM.md` and `port/gpu_v2/README.md` record where iss_pc's own code
comes from.

Lineage:

- [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc): the native-port approach
  (flat game image, HLE runtime, shadPS4-derived video core);
- [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) →
  [FelipePrado0/gow3_pc](https://github.com/FelipePrado0/gow3_pc) → **HayatoG/iss_pc** (also the core
  of HayatoG's tlg_pc and gr2_pc);
- [Castlesea/iss-second-son-pc-port](https://github.com/Castlesea/infamous-second-son-pc-port):
  renderer v2 (`port/gpu_v2/`), with [weshayoub92/shadPS4-perf](https://github.com/weshayoub92/shadPS4-perf);
- [shadPS4](https://github.com/shadps4-emu/shadPS4): the video core and shader recompiler;
- [Wooloomooloo2/bloodborne_mac_port](https://github.com/Wooloomooloo2/bloodborne_mac_port): the macOS
  techniques (Rosetta 2, guest memory layout, MoltenVK, the arm64 GPU process, the Metal renderer)
  this fork applies.

## Branches

- `developer`: iss_pc as it is, kept equal to `upstream/developer`.
- `macos` (default): the macOS work, on top of iss_pc.

## Syncing with iss_pc

```sh
git remote add upstream https://github.com/HayatoG/iss_pc.git   # once
git fetch upstream
git checkout developer && git merge --ff-only upstream/developer && git push origin developer
git checkout macos && git merge developer                        # resolve, build, test, push
```

Forked at iss_pc commit `88e912f` (2026-10-27). Record each sync here:

| Date | iss_pc commit | Notes |
|---|---|---|
| 2026-10-27 | `88e912f` | fork created |
