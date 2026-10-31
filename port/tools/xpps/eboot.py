"""Reads the player's own eboot.bin (the decrypted ELF the port runs) for the engine's class table.

The packages name the class of every effect object by a 64-bit hash (CID chunk); the engine maps
that hash to a type id through a sorted (hash, value) table and creates the C++ material pass of
that type through a factory table (FACT, read in the 1.07 executable):

    factory entry of type id t:  va FACTORY_VA + (t - FACTORY_ID) * FACTORY_STRIDE
        +0x00 u32 t, +0x04 u32 implementation id, +0x10 create(), +0x18 destroy(),
        +0x74 u32 object size, +0x78 parent type
    (create() = FUN_012da100-like: operator new(size) + constructor; the constructor stores the
    vtable whose slot +0x30 is the per-draw virtual called by FUN_014484d0, all.c l.958113)

Only addresses and ids are kept here (numbers); hashes are looked up in the player's file at run
time and never stored. Without the eboot the job builder still runs, with no class information.
"""
from __future__ import annotations

import os
import struct
from typing import Dict, Iterable, Optional

FACTORY_VA, FACTORY_ID, FACTORY_STRIDE = 0xD04E28, 0x3EF, 0x80
MAX_TYPE_ID = 0x1000
ELF_PT_LOAD, SCE_RELRO = 1, 0x61000010
EBOOT_NAMES = ("eboot.bin",)


def find_eboot(xpps_dir: str, levels: int = 4) -> Optional[str]:
    """eboot.bin in the folder of the .xpps files or one of its parents (<game>/CUSA00223/art/cache)."""
    d = os.path.abspath(xpps_dir)
    for _ in range(levels + 1):
        for name in EBOOT_NAMES:
            p = os.path.join(d, name)
            if os.path.isfile(p):
                return p
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return None


class Eboot:
    def __init__(self, path: str):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF":
            raise ValueError(f"not an ELF: {path}")
        phoff, = struct.unpack_from("<Q", d, 0x20)
        phnum, = struct.unpack_from("<H", d, 0x38)
        self.segs = []
        for i in range(phnum):
            t, _fl, off, va, _pa, fs, _ms, _al = struct.unpack_from("<IIQQQQQQ", d, phoff + 56 * i)
            if t in (ELF_PT_LOAD, SCE_RELRO) and fs:
                self.segs.append((va, off, fs))
        self.text_end = min((off + fs for va, off, fs in self.segs if va == 0), default=len(d))

    def read(self, va: int, n: int) -> Optional[bytes]:
        for v, off, fs in self.segs:
            if v <= va and va + n <= v + fs:
                return self.data[off + va - v:off + va - v + n]
        return None

    def u64(self, va: int) -> Optional[int]:
        b = self.read(va, 8)
        return struct.unpack("<Q", b)[0] if b else None

    def imm64(self, va: int) -> Optional[int]:
        """The immediate of a `movabs $imm64, %reg` (REX.W 48/49, opcode b8+r) at va."""
        b = self.read(va, 10)
        if b is None or b[0] not in (0x48, 0x49) or not 0xB8 <= b[1] <= 0xBF:
            return None
        return struct.unpack_from("<Q", b, 2)[0]

    def lea_disp(self, va: int) -> Optional[int]:
        """The disp32 of a `lea disp32(%reg), %reg` (REX 48/49, 8d, modrm mod=10[, SIB]) at va."""
        b = self.read(va, 8)
        if b is None or b[0] not in (0x48, 0x49, 0x4C, 0x4D) or b[1] != 0x8D or b[2] >> 6 != 2:
            return None
        at = 4 if b[2] & 7 == 4 else 3
        return struct.unpack_from("<i", b, at)[0]

    def name_hash(self, ref) -> Optional[int]:
        """A name hash the engine code holds as an immediate: ref = va of a movabs, or
        (va of a movabs, va of a lea that adds a displacement to that register)."""
        if isinstance(ref, int):
            return self.imm64(ref)
        base, lea = ref
        v, d = self.imm64(base), self.lea_disp(lea)
        return None if v is None or d is None else (v + d) & (1 << 64) - 1

    def factory(self, tid: int) -> Optional[dict]:
        """The factory entry of a type id, or None when the slot does not hold that id."""
        b = self.read(FACTORY_VA + (tid - FACTORY_ID) * FACTORY_STRIDE, FACTORY_STRIDE)
        if b is None:
            return None
        t, impl = struct.unpack_from("<II", b, 0)
        if t != tid:
            return None
        create, destroy = struct.unpack_from("<QQ", b, 0x10)
        size, = struct.unpack_from("<I", b, 0x74)
        return {"id": t, "impl": impl, "create": create, "destroy": destroy, "size": size}

    def type_of_class(self, h: int) -> Optional[int]:
        """Type id of a CID class hash: the value after the hash in the engine's sorted table,
        accepted only when the factory has an entry for it."""
        pat = struct.pack("<Q", h & (1 << 64) - 1)
        i = self.data.find(pat)
        while 0 <= i < self.text_end:
            if i % 8 == 0:
                v, = struct.unpack_from("<Q", self.data, i + 8)
                if 0 < v < MAX_TYPE_ID and self.factory(v):
                    return v
            i = self.data.find(pat, i + 1)
        return None

    def types_of(self, hashes: Iterable[int]) -> Dict[int, int]:
        out = {}
        for h in set(hashes):
            t = self.type_of_class(h)
            if t is not None:
                out[h] = t
        return out
