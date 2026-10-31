"""Precompile job files (ISSPJOB1), read and written exactly as vk_precompile.h describes them.

    "ISSPJOB1", u32 version (1)
    record: u32 kind (0 graphics, 1 compute)
            u32 n, then n x {u32 dword index, u32 value}: the nonzero register dwords
              (graphics: AmdGpu::Regs::reg_array; compute: AmdGpu::ComputeProgram)
            u32 stages, then per stage:
              u32 sw stage, u32 hw stage, u64 program hash
              u32 bytes, code from its first instruction to the end of its OrbShdr block
              u32 user_data[16]
              u32 n, u32 flattened user data[n]
              u32 fetch shader bytes, the code (0: none), u32 its user data register
              u32 n, then n x {u32 user data register, u32 dword offset, u32 V#[4]}

A record is a dict {'kind', 'regs': {index: value}, 'stages': [stage dicts]} and a stage is
{'sw', 'hw', 'hash', 'code', 'ud' (16 u32), 'flat' (list), 'fetch' (bytes), 'fetch_sgpr',
'vsharps': [(sgpr, dword_offset, (4 x u32))]}, the same layout as the development reader.
"""
from __future__ import annotations

import struct
from typing import Iterable, List

MAGIC = b"ISSPJOB1"
VERSION = 1

KIND_GRAPHICS, KIND_COMPUTE = 0, 1
# Shader::SwStage and Shader::HwStage (shader_recompiler/runtime_info.h)
SW_FRAGMENT, SW_VERTEX, SW_COMPUTE = 0, 3, 5
HW_FRAGMENT, HW_VERTEX, HW_COMPUTE = 0, 1, 6


def read(path) -> List[dict]:
    with open(path, "rb") as f:
        data = f.read()
    return parse(data)


def parse(data: bytes) -> List[dict]:
    if data[:8] != MAGIC:
        raise ValueError("not a precompile job file")
    (version,) = struct.unpack_from("<I", data, 8)
    if version != VERSION:
        raise ValueError(f"job file version {version}, expected {VERSION}")
    at = 12
    out = []

    def u32():
        nonlocal at
        (v,) = struct.unpack_from("<I", data, at)
        at += 4
        return v

    def blob():
        nonlocal at
        n = u32()
        b = data[at:at + n]
        if len(b) != n:
            raise ValueError("truncated job file")
        at += n
        return b

    while at < len(data):
        kind = u32()
        regs = {}
        for _ in range(u32()):
            i = u32()
            regs[i] = u32()
        stages = []
        for _ in range(u32()):
            st = {"sw": u32(), "hw": u32()}
            (st["hash"],) = struct.unpack_from("<Q", data, at)
            at += 8
            st["code"] = blob()
            st["ud"] = [u32() for _ in range(16)]
            st["flat"] = [u32() for _ in range(u32())]
            st["fetch"] = blob()
            st["fetch_sgpr"] = u32()
            st["vsharps"] = []
            for _ in range(u32()):
                sg, off = u32(), u32()
                st["vsharps"].append((sg, off, tuple(u32() for _ in range(4))))
            stages.append(st)
        out.append({"kind": kind, "regs": regs, "stages": stages})
    return out


def encode_record(rec: dict) -> bytes:
    parts = [struct.pack("<I", rec["kind"])]
    regs = sorted((i, v) for i, v in rec["regs"].items() if v)
    parts.append(struct.pack("<I", len(regs)))
    for i, v in regs:
        parts.append(struct.pack("<II", i, v & 0xFFFFFFFF))
    parts.append(struct.pack("<I", len(rec["stages"])))
    for st in rec["stages"]:
        parts.append(struct.pack("<IIQ", st["sw"], st["hw"], st["hash"] & 0xFFFFFFFFFFFFFFFF))
        parts.append(struct.pack("<I", len(st["code"])))
        parts.append(bytes(st["code"]))
        ud = list(st["ud"])[:16] + [0] * (16 - len(st["ud"]))
        parts.append(struct.pack("<16I", *[x & 0xFFFFFFFF for x in ud]))
        flat = st["flat"]
        parts.append(struct.pack("<I%dI" % len(flat), len(flat), *[x & 0xFFFFFFFF for x in flat]))
        fetch = bytes(st.get("fetch") or b"")
        parts.append(struct.pack("<I", len(fetch)))
        parts.append(fetch)
        parts.append(struct.pack("<I", st.get("fetch_sgpr", 0) if fetch else 0))
        vs = st.get("vsharps") or []
        parts.append(struct.pack("<I", len(vs)))
        for sg, off, words in vs:
            parts.append(struct.pack("<II4I", sg, off, *[w & 0xFFFFFFFF for w in words]))
    return b"".join(parts)


def write(path, records: Iterable[dict]) -> int:
    n = 0
    with open(path, "wb") as f:
        f.write(MAGIC + struct.pack("<I", VERSION))
        for rec in records:
            f.write(encode_record(rec))
            n += 1
    return n
