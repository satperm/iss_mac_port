"""Small tables and rules of the game's engine that decide register and sharp values.

Every entry says where it comes from:
    EBOOT     read in the 1.07 executable (function named in the comment); enums only
    INFERRED  inferred from a development capture (jobs_eval.py infer); each one is a
              reverse-engineering task until the eboot confirms it
The INFERRED tables are not in this file: `jobs_eval.py infer` writes them to a JSON file of
enums only (formats, strides, masks, api slots, pass ids), and `jobs.py --tables` reads it.
Nothing here holds game bytes, hashes or shader numbers.
"""
from __future__ import annotations

import json
import struct
from typing import Optional, Tuple

EBOOT, INFERRED = "eboot", "inferred"

# --- EBOOT: DB_SHADER_CONTROL rewrite when an effect is loaded (FUN_014468e0, REVISAO B4) ---
SOURCE_DB_SHADER_CONTROL = EBOOT


def db_shader_control(v: int) -> int:
    if v & 0x40:
        v = 0x60
    if v & 1:
        v &= ~0x30
    if v & 0x900:
        v = 0x920
    return v


# --- EBOOT: pass stage codes (pairs +0x88/+0x98: {stage code, descriptor}); DAT_01987c80 maps
# them to Gnm stages, 0 = VS and 3 = PS are the only ones the index has ---
SOURCE_STAGE_CODES = EBOOT
STAGE_CODE = {0: "VS", 3: "PS"}

# --- EBOOT: material textures (FUN_01447190, FUN_01448640, FUN_0144a250; PLAN F3/F4, B7) ---
#   material texture i -> descriptor b = effect.matmap[i] (+0xE0); every pass pair {stage, b}
#   of +0x98 binds it at api slot tex_desc[b] + 0xB (descriptors of 0x10 bytes at +0xD0);
#   the T# is the texture object's (+0x18), or a default one when the material has none.
SOURCE_MATERIAL_TEXTURES = EBOOT
TEX_DESC_SIZE, TEX_DESC_TYPE, TEX_DESC_API, TEX_DESC_SWAP = 0x10, 0x8, 0xB, 0xC
MAT_TEX_ENTRY = 0x30
#   samplers: pass pairs +0x88 {stage, d} -> smp_desc[d] (0x20 bytes at +0xC0): +8 api slot,
#   +0xC the S# (FUN_01447fa0)
SOURCE_EFFECT_SAMPLERS = EBOOT
SMP_DESC_SIZE, SMP_DESC_API, SMP_DESC_SHARP = 0x20, 0x8, 0xC

# --- EBOOT: sRGB swap of a material texture (FUN_0144a130, REVISAO B3) ---
# when descriptor +0xC == 0, fmtword[23:20] == 1 and fmtword[15:8] is one of these codes, the
# T# gets this data format, number format sRGB and dst_sel xyzw
SOURCE_SRGB_SWAP = EBOOT
SRGB_SWAP_DFMT = {0: 10, 6: 35, 7: 36, 8: 37, 13: 41}   # 8_8_8_8, BC1, BC2, BC3, BC7


def srgb_swap(words: list, fmtword: int, desc_swap: int) -> list:
    """The T# words a material texture is bound with (FUN_0144a250 -> FUN_0144a130)."""
    w = list(words)
    if desc_swap != 0 or (fmtword >> 20) & 0xF != 1:
        return w
    code = (fmtword >> 8) & 0xFF
    df = SRGB_SWAP_DFMT.get(code)
    if df is None:
        return w
    w[1] = (w[1] & 0xC00FFFFF) | df << 20 | 9 << 26
    w[3] = (w[3] & 0xFFFFF000) | 4 | 5 << 3 | 6 << 6 | 7 << 9
    return w


# --- EBOOT/measured: canonical constant buffer V# (PLAN F5: stride 16, dword3 0x20077FAC) ---
SOURCE_CB = EBOOT
CB_DWORD3 = 0x20077FAC
CB_STRIDE = 16

# --- INFERRED (F5, measured on the reference cache, not yet read in the eboot): the image type
# of an engine texture from its descriptor byte +8 ---
SOURCE_DESC_TYPE = INFERRED
DESC_TYPE_TO_IMAGE = {2: 9, 3: 10, 4: 11, 7: 11, 5: 12, 6: 13}

# --- INFERRED: what a material slot gets when the material has no texture (DAT_08d4c348 and
# DAT_08d4c358 are runtime objects; their format is unknown offline) ---
SOURCE_DEFAULT_TEXTURE = INFERRED
DEFAULT_TEXTURE = (9, 10, 0, (4, 5, 6, 7))    # 2D, 8_8_8_8, unorm, xyzw


# --- compute V# per InputUsageSlot (re_compute report, 16/10: measured on two caches, 1,483
# compute .meta files of 745 programs; the strides with an all.c line are EBOOT) ---
#   (usageType, apiSlot) -> stride: a number, "def" (u32 at +0x88 of the particle definition
#   that holds the kernel, FUN_013f09a0 initAsRegularBuffer) or "code" (from the accesses)
#   EBOOT sources (all.c, particle dispatch FUN_013f47e0 l.886031 + FUN_013fab70 l.891700 through
#   the ACB setters FUN_011696b0 (read-only, IRO +0x36) / FUN_011696f0 (RW, IRO +0xb6)):
#     (4,1) def   current state, initAsRegularBuffer(stride u32 def+0x88), l.887336
#     (4,0) 1     FUN_0161e260(.., 1, 0x30) + setResourceMemoryType(0x6e), l.891747
#     (4,2) 96    DAT_08d3eb50 = FUN_0161e260(.., 0x60, 0x20000), l.884144
#     (4,5) 176 / (4,6) 80   l.887435 / l.887443;  (4,7) 16  l.887380 (null when no count)
#     (0,1) def or null  previous state, l.887323-887326;  (0,2) 16 or null  l.887364
#     (0,3) 16    FUN_0116a110: FUN_0161e260(.., 0x10, 0x20), l.316140
#     (0,4) 4     FUN_0116a2c0: FUN_0161e260(.., 4, 0x200), l.316255
#     (0,11) 48 or null  FUN_013fae10: FUN_0161e260(.., 0x30, n), l.891664
#     (0,0) at s[0:3]: the header's last entry {0,0,0,0} (H4) is the register the particle kernels
#           get the stride-1 buffer of RW slot 0 in (MEASURED 82/82; the IRO mapping is a
#           HYPOTHESIS, the stride is EBOOT l.891747)
#   still MEASURED (re_compute report, two caches; no all.c line yet): (4,3) 8, (4,4) code,
#   (0,5) 16, (0,6) 4, (0,7) 48, (0,8) 4, (0,10) 32 (runtime objects of the particle mesh)
SOURCE_COMPUTE_SLOTS = {"4,0": EBOOT, "4,1": EBOOT, "4,2": EBOOT, "4,5": EBOOT, "4,6": EBOOT,
                        "4,7": EBOOT, "0,0": EBOOT, "0,1": EBOOT, "0,2": EBOOT, "0,3": EBOOT,
                        "0,4": EBOOT, "0,11": EBOOT, "2,*": EBOOT, "other": INFERRED}
COMPUTE_SLOTS = {(4, 1): "def", (4, 0): 1, (4, 2): 96, (4, 3): 8, (4, 4): "code", (4, 5): 176,
                 (4, 6): 80, (4, 7): 16, (0, 0): 1, (0, 1): "def", (0, 2): 16, (0, 3): 16,
                 (0, 4): 4, (0, 5): 16, (0, 6): 4, (0, 7): 48, (0, 8): 4, (0, 10): 32, (0, 11): 48}
CS_RW_DWORD3, CS_RO_DWORD3 = 0x18027204, 0x20027204     # mtype 3 (written) / 4 (read only)
# the "previous state" V# in s[0:3] (no InputUsageSlot) is null on the first frame of a particle
# effect (FUN_013f09a0, all.c l.887323-887326): one record with it null and one with it bound
SOURCE_CS_PREVIOUS_STATE = EBOOT
# (a synthetic unbound V#: num_records 0 is all the signature and the port look at)
CS_NULL_VSHARP = [0, 0, 0, 0x20027204]
# definition: def+0x90 and def+0x98 point at the two kernel shader objects, whose Gnm header
# pointer is at +0x30 (FUN_013f2bd0); the state stride is the u32 at def+0x88
PARTICLE_KERNELS, PARTICLE_STRIDE, SHADER_OBJ_HDR = (0x90, 0x98), 0x88, 0x30


# =========================================================================================
# v2: the engine model of the graphics state and of the engine resources, read in the eboot
# (FACT = all.c line; HYPOTHESIS = consistent with the code, not proven). Only numbers here:
# hashes are read from the player's eboot.bin at the code addresses below (eboot.py).
# =========================================================================================

# --- render target sets (FUN_012ffeb0 l.680686: flags per target; FUN_013008e0 l.677970: colour
# format; depth = Z32F (FUN_0161f6c0(3)), stencil S8 when flag 0x4000000; default mask
# (1 << 4*ncolour) - 1 (l.680967) unless the caller stores another one at set+0x24) ---------
#   target = (CB format, number type, swap), depth = (z format, stencil format) or None
C8888, C16U, C16F, C32F1, C8, C16_16F, C16F1, C16_16U = (
    (10, 0, 1), (12, 0, 1), (12, 7, 1), (4, 7, 0), (1, 0, 0), (5, 7, 0), (2, 7, 0), (5, 0, 0))
D_ZS, D_Z = (3, 1), (3, 0)
SOURCE_RT_SETS = EBOOT
RT_SETS = {
    # name: (colour targets, mask, depth)                      all.c line of the creation
    "G7": ([C8888, C16U, C8888, C8888, C16F, C16F], 0x7FFFF7, D_ZS),   # l.351036 (+0xf2c)
    "G6": ([C8888, C16U, C8888, C8888, C16F], 0xFFFF7, D_ZS),          # l.351046 (+0x14)
    "G6_T0": ([C8888], 0x7, D_ZS),                                     # l.351197 FUN_01300dd0(G6, 0)
    "HDR_ZS": ([C16F], 0xF, D_ZS),                                     # l.351276 (+0x8060)
    "HDR": ([C16F], 0xF, None),                                        # l.351274 (+0x78d4)
    "HDR_Z": ([C16F], 0xF, D_Z),                                       # l.351300 / 351333
    "RG16F_ZS": ([C16_16F], 0xF, D_ZS),                                # l.351355 (+0xf194)
    "RG16F": ([C16_16F], 0xF, None),                                   # l.351358
    "C8888": ([C8888], 0xF, None),                                     # l.351327
    "C8888_ZS": ([C8888], 0xF, D_ZS),                                  # l.351280 (+0x10838)
    "MAIN": ([C8888], 0x7, D_ZS),                                      # 0x1f1d4b0, mask 7 l.420420
    "R8_ZS": ([C8], 0xF, D_ZS),                                        # l.351393 (shadow mask)
    "R8": ([C8], 0xF, None),                                           # l.351349
    "R16F": ([C16F1], 0xF, None),                                      # l.351331
    "R32F": ([C32F1], 0xF, None),                                      # l.351346
    "RG16U": ([C16_16U], 0xF, None),                                   # l.351400
    "SHADOW": ([], 0, D_Z),                                            # l.351304 (+0x14c24), views l.943294
}
# frame-wide flags DAT_02d0cbd7/8 turn 0x10000000/0x20000000 targets into 2_10_10_10 (l.677978):
# the main colour target can be (9, 0, 1) instead of 8888 (HYPOTHESIS: off on the PS4 1.07 run)

# --- per class of material pass (type id of the CID class, eboot.py), FACT from the virtuals
# +0x28 (setup) / +0x30 (draw) of each class (agentB report, all.c lines in the comments) ------
#   sets: candidate RT sets (the bucket the class draws in; FUN_011fecc0 l.434165 & co. and the
#   bucket binders FUN_011901f0..FUN_01191100: G7 = bucket 1, G6_T0 = bucket 2, HDR_ZS =
#   buckets 16/20, RG16F_ZS = bucket 29, SHADOW = bucket 12); a class missing here draws in the
#   default bucket 0xd (FUN_01446ec0 l.956339: HYPOTHESIS HDR_ZS)
#   blend: CB_BLEND0 values; depth: "test" (needs the attachment) or "off"
BL_ADD, BL_ALPHA, BL_PREMUL, BL_DUAL = 0x40000101, 0x40000504, 0x40000501, 0x40000F01
BL_MUL_DST, BL_ALPHA_SEP, BL_PART, BL_PART_1C = 0x40000008, 0x61070504, 0x61000501, 0x61070501
RES1FF_BLEND = (0x40000501, 0x40000301, 0x40000508, 0x40000101)   # table va 0x1ae34c0 (FUN_01561750)
SHADOW_CASTERS = {0x3C2, 0x3C3, 0x3C4, 0x3FC}                       # FUN_0143d330 l.946230
GBUFFER_CLASSES = {0x39B, 0x3AA, 0x3A2, 0x3A4, 0x399, 0x398, 0x3AB, 0x3A6, 0x3A1, 0x3A9,
                   0x3C7, 0x3A0, 0x3A3, 0x3A5, 0x3AC, 0x394}            # no colour state of their own
SOURCE_CLASS_STATE = EBOOT


def class_sets(tid, pid, nmrt):
    """Candidate RT sets of a pass (class type id, render slot = pass id, MRTs the PS writes)."""
    if tid in SHADOW_CASTERS:
        return ["SHADOW"]
    if tid == 0x396:                        # dual-source blend: 2 exports, 1 target (HDR_ZS)
        return ["HDR_ZS"]
    if nmrt >= 2:
        return ["G7"]                       # G6 projects the same on the MRTs a 5-target PS writes
    if tid in (0x3C0, 0x3C1):
        return ["MAIN", "C8888_ZS"]         # FUN_015a8b00: buckets 33/34 -> main colour 0x1f1d4b0
        #                                     (mask 7); +0x10838 (mask 0xf) is a HYPOTHESIS
    if tid == 0x3AD:
        return ["RG16F_ZS"]                 # bucket 29 (HYPOTHESIS: the only 16_16 float + depth set)
    if tid in GBUFFER_CLASSES and pid in (0, 9):
        return ["G7", "HDR_ZS"]
    if tid is None or pid == 255:
        return ENGINE_SETS
    return ["HDR_ZS"]


# explicit engine passes (post-processing, pass id 255) whose draw function is not traced: every
# single-target set (HYPOTHESIS)
ENGINE_SETS = ["HDR", "HDR_ZS", "C8888", "C8888_ZS", "RG16F", "R8", "R16F", "R32F", "RG16U", "SHADOW"]

# engine passes drawn by explicit code (agentF, FACT unless marked): (colour targets, mask, depth
# (attachment needed) or None, CB_BLEND0, colour off, primitive). Depth control 0x70/0x60 = no
# depth (no attachment in the key), 0x76 = always written. Classes absent here use ENGINE_SETS.
ENGINE_CLASS_STATE = {
    0x3AF: [([C16F], 0xF, None, 0, False, 4), ([C8], 0xF, None, 0, False, 4),
            ([C8888], 0xF, None, 0, False, 4)],                   # FUN_01455f00 l.965098, sets l.349515/349655/348372
    0x3AE: [([], 0, None, 0, True, 4)],                           # FUN_01301c90 l.679274 (colour off)
    0x3B9: [([], 0, None, 0, True, 4)],                           # FUN_01457a10, l.679296
    0x3B8: [([C16F], 0xF, None, BL_ADD, False, 4)],               # l.349660-349668, depth l.362375
    0x3BA: [([C16F], 0xF, None, 0, False, 4), ([C16_16F], 0xF, None, 0, False, 4)],   # l.349526-349560
    0x3BB: [([C16F], 0xF, None, BL_ALPHA, False, 4)],             # l.349564, blend l.363694
    0x3BC: [([C16F], 0xF, None, 0, False, 4), ([C16F], 0xF, None, BL_ALPHA, False, 4)],  # l.349051-349087
    0x3C6: [([], 0, D_ZS, 0, False, 4), ([], 0, None, 0, False, 4)],  # occlusion query, mask 0 l.416688
    0x3D4: [([], 0, D_Z, 0, False, 4)],                           # depth 0x76 l.360798, mask 0 l.360805
    0x3D5: [([C16F], 0xF, None, 0, False, 4)],                    # l.347973, depth l.361113
    0x3D6: [([C8888], 0x7, None, 0, False, 4), ([C8888], 0xF, None, 0, False, 4),
            ([C8888, C8], 0xFF, None, 0, False, 4)],              # l.350176/420108, l.349175
    0x3D7: [([C8888], 0x7, None, 0, False, 4)],                   # l.349184/350159, depth l.361403
    0x3D9: [([C16F], 0xF, None, 0, False, 4), ([C8], 0xF, None, 0, False, 4)],   # FUN_01191ac0 l.348121
    0x3DA: [([C16F], 0xF, None, 0x40000401, False, 4)],           # l.348722-348726
    0x3E4: [([C16F], 0xF, None, BL_ADD, False, 4)],               # l.352902, depth l.362760
    0x3E5: [([C16F], 0xF, None, 0, False, 4)],                    # l.348771-348784
    0x3E7: [([C16F], 0xF, None, 0, False, 4)],                    # l.348366, depth l.361232
    # HYPOTHESIS on the target (the class's own code gives blend/mask/depth/primitive):
    0x3B6: [([C8888], 0x7, None, 0, False, 6)],                   # FUN_014575b0 l.965540, prim 6 (FUN_014487a0 l.958739)
    0x3B7: [([C8888], 0x7, None, BL_ALPHA, False, 4), ([C8888], 0x7, None, 0, False, 4)],  # l.965843-965845
    0x3D2: [([C8888], 0x3, D_ZS, 0x40000200, False, 4)],          # bucket code l.347690-347706
    0x3E1: [([C8888], 0x3, D_ZS, 0x40000200, False, 4)],
}


# VGT_INSTANCE_STEP_RATE_0/1: written only (1, 1), by the classes 0x3fe (l.893924) and 0x3c1
# (l.1256315), never reset in a command buffer: every pass drawn after them in the frame may carry
# (1, 1). The opaque G-buffer bucket draws first (HYPOTHESIS), so its sets only get (0, 0)
FIRST_BUCKET_SETS = {"G7", "G6"}


def flags_of(ids_a8: bytes) -> bytes:
    """The material flag bitmap FUN_01447370 (l.957901) builds from the pass +0xa8 list: id < 161
    sets bit id."""
    out = bytearray(21)
    for i in range(0, len(ids_a8) - 3, 4):
        (v,) = struct.unpack_from("<I", ids_a8, i)
        if v < 0xA1:
            out[v >> 3] |= 1 << (v & 7)
    return bytes(out)


def class_blends(tid, pid, f: bytes):
    """[(rt index, CB_BLEND value, colour mask AND)] candidates of a class (agentB, FACT)."""
    if tid == 0x3A1:                                    # FUN_011a3f60 l.~359000, FUN_011a4080 l.358981
        b5 = bool(f[1] & 0x12 or f[3] & 1 or f[0x12] & 0x40)
        b4 = bool(f[9] & 0x40)
        mask, b3, blend = 0xFC3F, True, False
        if not f[9] & 0x20:
            if not (b4 or f[0] & 0x20 or f[10] & 4 or b5):
                b3, blend = False, True
            else:
                b3 = bool(f[10] & 4)
        if not b5:
            mask &= ~0xC00
        if not b4 and not f[0] & 0x20:
            mask &= ~0x30
        if not b3:
            mask &= ~0xF000
        return [(0, BL_ALPHA if blend else 0, mask)]
    if tid == 0x3A6:                                    # FUN_011a4fd0 l.359969
        mask = 0x330 if f[0] & 0x20 else 0x300
        return [(2, 0 if f[0] & 0x20 else BL_ALPHA, mask)]
    if tid == 0x396:                                    # FUN_01419590 l.915921
        return [(0, BL_DUAL, None)]
    if tid in (0x39E, 0x39F):                           # FUN_011ffb80 l.436200 / FUN_01200080 l.435949
        return [(0, BL_ADD if pid in (1, 0xB) else 0, None)]
    if tid == 0x39D:                                    # FUN_011feed0 l.437305
        return [(0, {10: BL_ALPHA_SEP, 0xC: BL_PREMUL}.get(pid, 0),
                 {0xB: 8, 0xC: 7, 0xD: 0}.get(pid))]
    if tid == 0x39A:                                    # FUN_01175150 l.323446
        return [(0, BL_ADD, 8)]
    if tid in (0x3CA, 0x3CB, 0x3CC, 0x3CE, 0x3CF, 0x3D0):   # FUN_0119b930 l.355764 (+0x11c option)
        return [(0, BL_ADD, None), (0, BL_MUL_DST, None), (0, 0, None)]
    if tid in (0x3C0, 0x3C1):                           # FUN_01561750 l.1193852: record +0x4c mode
        return [(0, b, None) for b in RES1FF_BLEND]
    if tid == 0x3F9:                                    # FUN_013f9be0 l.892144
        return [(0, BL_PART, None), (0, BL_PART_1C, None)]
    if tid == 0x3FE:                                    # FUN_013fc220 l.893135
        return [(0, BL_ADD, None)]
    if tid == 0x3DE:                                    # FUN_011d0890 l.393268
        return [(0, BL_ALPHA, None)]
    return [(0, 0, None)]


def class_depth_off(tid, pid):
    """Classes that draw with the depth test off (no attachment in the key): 0x3fe (depth 0x60,
    l.893135); engine passes vary (both)."""
    return tid == 0x3FE


# primitive (VGT_PRIMITIVE_TYPE, setter FUN_01623740): 4 for scene passes (l.358344) and the
# fullscreen FUN_014489a0 (l.958613); 0x13 quad list in FUN_011d0ce0 (class 0x3de); 6 in
# FUN_014487a0 (l.958739, class 0x3b6 in ENGINE_CLASS_STATE)
CLASS_PRIM = {0x3DE: (0x13,)}

# --- engine buffers named by hash (the shader declares the hash in the effect texture/buffer
# descriptor +0x00; FUN_01445ae0 l.~955100 finds the api slot by it). ref = va of the movabs
# (or (movabs, lea)) holding the hash in the eboot; value = (stride, dfmt, nfmt, dst_sel, mtype,
# nullable). FACT (agentC): VS globals FUN_0145dba0 l.973630-973690, bound by FUN_013f93e0
# l.892737-892794 (null when the object has no instance data); per object FUN_011a1770 l.358168,
# FUN_0143d480 l.946295, FUN_011a2030 l.367615; lights FUN_01446190 l.955339 ---
SOURCE_ENGINE_BUFFERS = EBOOT
REG4 = (4, 7, (4, 0, 0, 1))               # FUN_0161e260 initAsRegularBuffer: 32 float, x001
ENGINE_BUFFERS = [
    (0x1A0896, (64,) + REG4 + (4, True)),                   # FUN_011a0850 slot 1 (l.357759)
    (0x1A08C8, (64, 12, 4, (4, 5, 6, 7), 4, True)),         # slot 2, fmt 0xfac40c
    (0x1A08B0, (64, 12, 4, (4, 5, 6, 7), 4, True)),         # slot 3
    ((0x1A086D, 0x1A08E0), (8, 12, 7, (4, 5, 6, 7), 4, True)),   # slot 4, fmt 0xfac70c
    (0x1A08F5, (12,) + REG4 + (4, True)),                   # slot 5
    (0x1A090D, (12,) + REG4 + (4, True)),                   # slot 6
    ((0x1A0861, 0x1A0925), (80,) + REG4 + (4, False)),      # slot 7
    (0x1A0861, (2, 2, 4, (4, 0, 0, 0), 4, False)),          # slot 8, fmt 0x4402 (l.972163)
    (0x1A086D, (112,) + REG4 + (3, False)),                 # slot 9 (PS), FUN_0145f180 l.971660
    (0x1A13BA, (212,) + REG4 + (4, False)),                 # instanced scene draw, FUN_011a1770
    (0x1A13D5, (48,) + REG4 + (4, False)),                  # skinning palette
    (0x1A13F0, (48,) + REG4 + (4, False)),                  # previous palette
    (0x43D309, (72,) + REG4 + (4, False)),                  # instanced shadow draw, FUN_0143d480
    (0x445F4C, (176,) + REG4 + (4, False)),                # lights (FUN_01446190), 2000 entries
    (0x05D1FD, (176,) + REG4 + (4, False)),                # lights of the scene classes (FUN_0105d1f0)
    (0x445FFA, (2, 2, 4, (4, 0, 0, 0), 4, False)),         # light index list, fmt 0x4402 (l.317077)
    (0x41925D, (112,) + REG4 + (4, True)),                 # obj+0x7b8 (FUN_014187c0 l.919134), null l.918773
    (0x3FBA35, (96,) + REG4 + (3, False)),                 # DAT_08d3eb54, l.884144 (mtype 3: 0x6e)
    (0x1A140B, (4,) + REG4 + (3, False)),                  # FUN_011a1330 +0xd8 (HYPOTHESIS: the
    #                                                        4-byte buffer of FUN_0126a900 l.524232)
]
# (va relative to the ELF base 0 = all.c address - 0x1000000)


# the descriptor type byte (+8) of a plain buffer resource (format 32 float, initAsRegularBuffer);
# 0 = formatted buffer. MEASURED in the packages (an enum of the effect format, like
# DESC_TYPE_TO_IMAGE for images)
DESC_TYPE_REGULAR_BUFFER = 10
NULL_BUFFER_WITNESS = ["B", 0, 0, 0, 0, 0, 4, 7, [4, 0, 0, 1], 3, 0]    # DAT_01f1dc3c: num_records 0


# --- engine textures named by hash, created in code with a constant format (agentE, FACT):
#   ref = va of the movabs holding the slot hash; value = (image type, data format, number format)
#   FUN_014179e0 l.915719-915770 (fog/atmosphere volumes, bound by FUN_014192d0 l.~915890):
#     2D array 256x256x32 BC6 (FUN_01027540(0xb) -> 0x3ac028), 2D array BC1 sRGB (0xfac923),
#     1D array 512x32 BC1 sRGB; FUN_0105bab0 (va 0x5bab0): cube 256x256 BC6 (FUN_0105d240 l.84280)
SOURCE_ENGINE_TEXTURES = EBOOT
ENGINE_TEXTURES = [
    (0x41927B, (13, 40, 0)),
    (0x419296, (13, 35, 9)),
    (0x4192B1, (12, 35, 9)),
    (0x05D21B, (11, 40, 0)),
]


def engine_texture_table(eb) -> dict:
    out = {}
    if eb is None:
        return out
    for ref, spec in ENGINE_TEXTURES:
        h = eb.name_hash(ref)
        if h is not None:
            out.setdefault(h, spec)
    return out


# vertex attributes the mesh streams do not cover:
#   a one-component input past the mesh streams gets the per-instance index stream, the format of
#   the instance index buffer (FUN_0145f590 l.972017, DataFormat 0x4402 = 16 uint, x000, l.972163;
#   HYPOTHESIS: the same buffer feeds the vertex table entry); otherwise 32-bit float with the
#   input's component count (the fetch projection keeps only the number class and dst_sel)
INSTANCE_STREAM_WITNESS = ["B", 1, 2, 0, 0, 0, 2, 4, [4, 0, 0, 0], 4, 0]


def vertex_witness_by_size(n: int) -> list:
    df = {1: 4, 2: 11, 3: 13}.get(n, 14)
    sel = [(4, 0, 0, 1), (4, 5, 0, 1), (4, 5, 6, 1), (4, 5, 6, 7)][max(1, min(n, 4)) - 1]
    return ["B", 1, 4 * max(1, min(n, 4)), 0, 0, 0, df, 7, list(sel), 4, 0]


def engine_buffer_table(eb) -> dict:
    """name hash -> buffer spec, the hashes read from the player's eboot (None without it)."""
    out = {}
    if eb is None:
        return out
    for ref, spec in ENGINE_BUFFERS:
        h = eb.name_hash(ref)
        if h is not None:
            out.setdefault(h, spec)
    return out


class Tables:
    """The INFERRED tables written by `jobs_eval.py infer` (JSON of enums only)."""

    def __init__(self, data: Optional[dict] = None):
        data = data or {}
        self.source = data.get("source", INFERRED)
        # pass id -> [profile]: {"state": {"off", "cb": {cb: [fmt, ntype, swap, blend, mask,
        # frag_log2]}, "depth": [z, stencil, samples_log2, clamp], "raster": [prim, poly mode,
        # provoking, zclip, clip space, rop3], "vs": [clip planes, clip space, clip disable,
        # step rate 0, step rate 1, emulated prim]}, "VS": {key: witness}, "PS": {key: witness}}
        # with key "usage/api/dwords" (jobs.witness_of); one profile per captured draw
        self.engine = {int(k): v for k, v in data.get("engine", {}).items()}
        # [{key: witness}] of the compute dispatches
        self.compute = data.get("compute", [])
        # key -> witness: the most common value of each slot key over all profiles
        self.fallback = data.get("fallback", {})

    @classmethod
    def load(cls, path) -> "Tables":
        with open(path) as f:
            return cls(json.load(f))

    def dump(self) -> dict:
        return {"source": self.source, "engine": self.engine,
                "compute": self.compute, "fallback": self.fallback}


def slot_key(usage: int, api, dwords: int) -> str:
    return f"{usage}/{api}/{dwords}"


def parse_key(key: str) -> Tuple:
    a, b, c = key.split("/")
    return (a if a == "implicit" else int(a), int(b), int(c))
