"""What of a draw or dispatch the port's shader and pipeline caches can tell apart.

`signature(record, headers)` keeps only what changes the GraphicsPipelineKey and the
StageSpecialization of each stage (port/gpu/shadps4: video_core/renderer_vulkan/
vk_pipeline_cache.cpp RefreshGraphicsKey/BuildRuntimeInfo, shader_recompiler/specialization.h,
shader_recompiler/runtime_info.h). Addresses, sizes and every register the caches never read
drop out, so two records with the same signature compile to the same shaders and pipeline.

The signature is a dict of named parts (tuples of ints), so a miss can be blamed on a part:
    vs, ps, cs          program hashes (key.stage_hashes; the permutation index is a function of
                        the specialization below)
    vs_rt, ps_rt, cs_rt the RuntimeInfo fields the caches compare (runtime_info.h operator==)
    ps_in               SPI_PS_INPUT_CNTL_i, i < NUM_INTERP (BuildRuntimeInfo, fs.inputs)
    rt                  per color target: format, number type, swap, blend, write mask, samples
                        (RefreshGraphicsKey first and second pass); also fs.color_buffers
    depth               z/stencil formats when the depth attachment is needed, depth samples
    raster              primitive type, polygon mode, provoking vertex, clip control, logic op
    vs_res, ps_res, cs_res  one projected sharp per InputUsageSlot (F2: the sharp of a slot is at
                        flat[startRegister]) with only the fields specialization.h reads
    fetch               the fetch shader layout and, per vertex attribute, its V# number class
                        and dst_sel (specialization.h vs_attribs)

Not modelled here (constant for ISS or not decidable offline, see README):
    motion vectors (positions_address is 0 unless object motion is on), draw indirect sgprs,
    vertex_buffer_formats (only without VK_EXT_vertex_input_dynamic_state), `start` bindings
    (a function of the pair), mtype/add_tid_enable (change the SPIR-V but are not in the key,
    REVISAO B12: they cannot make a separate cache entry).
All numbers below are GCN register indices and AMD enum values (no game data).
"""
from __future__ import annotations

import hashlib
import struct
from typing import Callable, Dict, Optional, Tuple

# --- register dword indices (AmdGpu::Regs::reg_array, GCN numbering) ---
SPI_SHADER_PGM_LO_PS, SPI_SHADER_PGM_RSRC1_PS, SPI_SHADER_PGM_RSRC2_PS = 0x2C08, 0x2C0A, 0x2C0B
SPI_SHADER_USER_DATA_PS_0 = 0x2C0C
SPI_SHADER_PGM_LO_VS, SPI_SHADER_PGM_RSRC1_VS, SPI_SHADER_PGM_RSRC2_VS = 0x2C48, 0x2C4A, 0x2C4B
SPI_SHADER_USER_DATA_VS_0 = 0x2C4C
DB_RENDER_CONTROL, DB_RENDER_OVERRIDE = 0xA000, 0xA003
DB_Z_INFO, DB_STENCIL_INFO, DB_Z_READ_BASE, DB_STENCIL_READ_BASE = 0xA010, 0xA011, 0xA012, 0xA013
DB_Z_WRITE_BASE, DB_STENCIL_WRITE_BASE, DB_DEPTH_SIZE, DB_DEPTH_SLICE = 0xA014, 0xA015, 0xA016, 0xA017
CB_TARGET_MASK, CB_SHADER_MASK = 0xA08E, 0xA08F
DB_STENCIL_CONTROL, DB_STENCILREFMASK, DB_STENCILREFMASK_BF = 0xA10B, 0xA10C, 0xA10D
SPI_PS_INPUT_CNTL_0 = 0xA191
SPI_VS_OUT_CONFIG, SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR = 0xA1B1, 0xA1B3, 0xA1B4
SPI_PS_IN_CONTROL, SPI_BARYC_CNTL = 0xA1B6, 0xA1B8
SPI_SHADER_POS_FORMAT, SPI_SHADER_Z_FORMAT, SPI_SHADER_COL_FORMAT = 0xA1C3, 0xA1C4, 0xA1C5
CB_BLEND0_CONTROL = 0xA1E0
DB_DEPTH_CONTROL, CB_COLOR_CONTROL, DB_SHADER_CONTROL = 0xA200, 0xA202, 0xA203
PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VS_OUT_CNTL = 0xA204, 0xA205, 0xA207
VGT_INSTANCE_STEP_RATE_0, VGT_INSTANCE_STEP_RATE_1 = 0xA2A8, 0xA2A9
VGT_SHADER_STAGES_EN = 0xA2D5
PA_SC_AA_CONFIG = 0xA2F8
CB_COLOR0_BASE, CB_COLOR_STRIDE = 0xA318, 15   # +4 INFO, +5 ATTRIB
CB_INFO, CB_ATTRIB = 4, 5
VGT_PRIMITIVE_TYPE = 0xC242
NUM_CB = 8
# AmdGpu::ComputeProgram dwords (video_core/amdgpu/regs_shader.h)
CS_NUM_THREAD_X, CS_ADDRESS_LO, CS_RSRC1, CS_RSRC2, CS_RESOURCE_LIMITS = 7, 12, 18, 19, 21
CS_USER_DATA = 64

# Gnm header register words (gnm.py): order of the u32 at +0x10.
VS_HDR = ("RSRC1", "RSRC2", "VS_OUT_CONFIG", "POS_FORMAT", "PA_CL_VS_OUT_CNTL")
PS_HDR = ("RSRC1", "RSRC2", "Z_FORMAT", "COL_FORMAT", "INPUT_ENA", "INPUT_ADDR", "IN_CONTROL",
          "BARYC_CNTL", "DB_SHADER_CONTROL", "CB_SHADER_MASK")
CS_HDR = ("RSRC1", "RSRC2", "NUM_THREAD_X", "NUM_THREAD_Y", "NUM_THREAD_Z")
VS_HDR_REG = {"RSRC1": SPI_SHADER_PGM_RSRC1_VS, "RSRC2": SPI_SHADER_PGM_RSRC2_VS,
              "VS_OUT_CONFIG": SPI_VS_OUT_CONFIG, "POS_FORMAT": SPI_SHADER_POS_FORMAT,
              "PA_CL_VS_OUT_CNTL": PA_CL_VS_OUT_CNTL}
PS_HDR_REG = {"RSRC1": SPI_SHADER_PGM_RSRC1_PS, "RSRC2": SPI_SHADER_PGM_RSRC2_PS,
              "Z_FORMAT": SPI_SHADER_Z_FORMAT, "COL_FORMAT": SPI_SHADER_COL_FORMAT,
              "INPUT_ENA": SPI_PS_INPUT_ENA, "INPUT_ADDR": SPI_PS_INPUT_ADDR,
              "IN_CONTROL": SPI_PS_IN_CONTROL, "BARYC_CNTL": SPI_BARYC_CNTL,
              "DB_SHADER_CONTROL": DB_SHADER_CONTROL, "CB_SHADER_MASK": CB_SHADER_MASK}
CS_HDR_REG = {"RSRC1": CS_RSRC1, "RSRC2": CS_RSRC2, "NUM_THREAD_X": CS_NUM_THREAD_X,
              "NUM_THREAD_Y": CS_NUM_THREAD_X + 1, "NUM_THREAD_Z": CS_NUM_THREAD_X + 2}

# InputUsageSlot usage types (Gnm): immediate resource, sampler, constant buffer, rw resource;
# the pointers (fetch shader 0x12, vertex buffer table 0x17, extended user data 0x1B) hold no
# sharp of their own.
USAGE_RESOURCE, USAGE_SAMPLER, USAGE_CB, USAGE_RW = 0, 1, 2, 4
SHARP_USAGES = (USAGE_RESOURCE, USAGE_SAMPLER, USAGE_CB, USAGE_RW)
USAGE_FETCH, USAGE_VB_TABLE, USAGE_EUD = 0x12, 0x17, 0x1B
USAGE_GDS_COUNTER = 0x07


def bits(v: int, lo: int, n: int) -> int:
    return (v >> lo) & ((1 << n) - 1)


# --- AmdGpu enums and remaps (video_core/amdgpu/pixel_format.h) ---
DF_8, DF_16, DF_32, DF_32_32, DF_32_32_32, DF_32_32_32_32 = 1, 2, 4, 11, 13, 14
DF_10_11_11, DF_11_11_10, DF_10_10_10_2, DF_2_10_10_10 = 6, 7, 8, 9
DF_8_8, DF_16_16, DF_8_8_8_8, DF_16_16_16_16 = 3, 5, 10, 12
DF_5_6_5, DF_1_5_5_5, DF_5_5_5_1, DF_4_4_4_4 = 16, 17, 18, 19
DF_BC1, DF_BC4, DF_BC5, DF_BC6, DF_BC7 = 35, 38, 39, 40, 41
NF_UNORM, NF_SNORM, NF_USCALED, NF_SSCALED, NF_UINT, NF_SINT, NF_SNORMNZ, NF_FLOAT = range(8)
NF_SRGB, NF_UBNORM = 9, 10
IMG_1D, IMG_2D, IMG_3D, IMG_CUBE, IMG_1D_ARRAY, IMG_2D_ARRAY, IMG_2D_MSAA, IMG_2D_MSAA_ARRAY = range(8, 16)


def remap_data_format(df: int) -> int:                       # pixel_format.h:200
    return {DF_11_11_10: DF_10_11_11, DF_10_10_10_2: DF_2_10_10_10, DF_5_5_5_1: DF_1_5_5_5}.get(df, df)


def remap_number_format(nf: int, df: int) -> int:            # pixel_format.h:213
    if nf == NF_UNORM:
        return NF_UINT if df in (DF_32, DF_32_32, DF_32_32_32, DF_32_32_32_32) else nf
    if nf == NF_SRGB:
        return NF_UNORM if df in (DF_BC4, DF_BC5, DF_BC6) else nf
    if nf == NF_USCALED:
        return NF_UINT
    if nf in (NF_SSCALED, NF_SNORMNZ):
        return NF_SINT
    if nf == NF_UBNORM:
        return NF_UNORM
    if nf == NF_FLOAT and df == DF_8:
        return NF_UINT
    return nf


def number_conversion(nf: int, df: int) -> int:              # pixel_format.h:317
    if nf == NF_UNORM:
        return 6 if df in (DF_32, DF_32_32, DF_32_32_32, DF_32_32_32_32) else 0
    if nf == NF_SRGB:
        return 7 if df == DF_BC6 else 0
    if nf == NF_USCALED:
        return 1
    if nf == NF_SSCALED:
        return 2
    if nf == NF_UBNORM:
        return 3
    if nf == NF_SNORMNZ:
        if df in (DF_8, DF_8_8, DF_8_8_8_8):
            return 4
        if df in (DF_16, DF_16_16, DF_16_16_16_16):
            return 5
    return 0


def number_class(nf: int) -> int:                            # pixel_format.h:369
    return 1 if nf == NF_SINT else 2 if nf == NF_UINT else 0


def remap_swizzle(df: int, sel: Tuple[int, int, int, int]) -> Tuple[int, ...]:  # pixel_format.h
    r, g, b, a = sel
    if df in (DF_1_5_5_5, DF_11_11_10, DF_5_6_5):
        return (b, g, r, a)
    if df == DF_10_10_10_2:
        return (a, b, g, r)
    if df == DF_4_4_4_4:
        return (g, b, a, r)
    if df in (DF_8, DF_16, DF_32):
        return tuple(4 if c == 7 else c for c in sel)
    return sel


# --- sharps ---
def buffer_fields(w):
    """V# (resource.h:19). w = 4 u32."""
    d1, d2, d3 = w[1], w[2], w[3]
    return {
        "stride": bits(d1, 16, 14), "swizzle_enable": bits(d1, 31, 1), "num_records": d2,
        "dst_sel": (bits(d3, 0, 3), bits(d3, 3, 3), bits(d3, 6, 3), bits(d3, 9, 3)),
        "nfmt": bits(d3, 12, 3), "dfmt": bits(d3, 15, 4), "element_size": bits(d3, 19, 2),
        "index_stride": bits(d3, 21, 2), "add_tid": bits(d3, 23, 1), "mtype": bits(d3, 27, 3),
        "type": bits(d3, 30, 2),
    }


def image_fields(w):
    """T# (resource.h:141). w = 8 u32 (only the first 4 are read here)."""
    d0, d1, d2, d3 = w[0], w[1], w[2], w[3]
    return {
        "base": d0 | bits(d1, 0, 6) << 32, "dfmt": bits(d1, 20, 6), "nfmt": bits(d1, 26, 4),
        "width": bits(d2, 0, 14), "height": bits(d2, 14, 14),
        "dst_sel": (bits(d3, 0, 3), bits(d3, 3, 3), bits(d3, 6, 3), bits(d3, 9, 3)),
        "last_level": bits(d3, 16, 4), "type": bits(d3, 28, 4),
    }


def proj_buffer(w, cb: bool) -> tuple:
    """BufferSpecialization (specialization.h:117-131) + the binding bit (num_records != 0,
    resource.h:56). is_formatted is a property of the shader's use (desc), so the formatted
    fields are kept for every immediate resource/rw buffer and dropped for constant buffers."""
    f = buffer_fields(w)
    if f["num_records"] == 0:
        return ("B", 0)
    out = ("B", 1, f["stride"], f["swizzle_enable"])
    if f["swizzle_enable"]:
        out += (f["index_stride"], f["element_size"])
    if not cb:
        df = remap_data_format(f["dfmt"])
        out += (df, remap_number_format(f["nfmt"], f["dfmt"]),
                remap_swizzle(f["dfmt"], f["dst_sel"]), number_conversion(f["nfmt"], f["dfmt"]))
    return out


def is_block1d(f) -> bool:
    """resource.h:329 (block-coded 1D images become 2D of height 1 on most drivers)."""
    return f["type"] in (IMG_1D, IMG_1D_ARRAY) and DF_BC1 <= f["dfmt"] <= DF_BC7


def proj_image(w, storage: bool) -> tuple:
    """ImageSpecialization (specialization.h:133-146) + the binding bit (base != 0,
    resource.h:209). The host view type is a function of the T# type, IsBlock1D and the
    shader's is_array; the data format only matters for FMasks and depth (not projected)."""
    f = image_fields(w)
    if f["base"] == 0:
        return ("I", 0)
    nf = remap_number_format(f["nfmt"], f["dfmt"])
    out = ("I", 1, f["type"], int(is_block1d(f)), int(nf in (NF_SINT, NF_UINT)),
           number_conversion(f["nfmt"], f["dfmt"]))
    if storage:
        out += ("w", remap_swizzle(f["dfmt"], f["dst_sel"]))
    else:
        out += (int(nf == NF_SRGB),)
    return out


def proj_sampler(w) -> tuple:
    """SamplerSpecialization (specialization.h:152-156): force_unnormalized, force_degamma."""
    return ("S", bits(w[0], 15, 1), bits(w[0], 20, 1))


def slot_sharp_words(flat, reg: int, n: int) -> Optional[tuple]:
    if reg + n > len(flat):
        return None
    return tuple(flat[reg:reg + n])


def proj_slot(slot, flat) -> Optional[tuple]:
    """One InputUsageSlot (usageType, apiSlot, startRegister, flags) -> its projected sharp."""
    usage, _api, reg, flags = slot
    if usage not in SHARP_USAGES:
        return None
    if usage == USAGE_SAMPLER:
        w = slot_sharp_words(flat, reg, 4)
        return proj_sampler(w) if w else ("S", "missing")
    eight = bool(flags & 1)          # m_registerCount: 1 = 8 dwords (T#)
    w = slot_sharp_words(flat, reg, 8 if eight else 4)
    if w is None:
        return ("?", "missing")
    if eight:
        return proj_image(w, usage == USAGE_RW)
    return proj_buffer(w, usage == USAGE_CB)


def slot_dwords(slot) -> int:
    usage, _api, _reg, flags = slot
    if usage in SHARP_USAGES:
        return 8 if usage != USAGE_SAMPLER and flags & 1 else 4
    if usage in (USAGE_FETCH, USAGE_VB_TABLE, USAGE_EUD):
        return 2
    if usage == USAGE_GDS_COUNTER:
        return 1
    # Unknown types: in the compute headers the last entry is often {n, 0, 0, 0} with n = the
    # extended user data size in dwords, not a slot; it covers nothing.
    return 0


def stage_resources(slots: bytes, flat, num_user_regs: int = 0) -> tuple:
    """Projected sharps of the declared InputUsageSlots, then of the user data quads no slot
    covers ("implicit": measured on the capture, 326 of 413 compute dispatches read a V# in
    s[0:3] that their header does not declare; with it the signature separates exactly the 413
    compute pipelines the port built)."""
    out = []
    covered = set()
    for i in range(0, len(slots) - len(slots) % 4, 4):
        s = tuple(slots[i:i + 4])
        covered.update(range(s[2], s[2] + slot_dwords(s)))
        p = proj_slot(s, flat)
        if p is not None:
            out.append((s[0], s[1], p))
    for q in range(0, min(num_user_regs, 16) - 3, 4):
        if not covered.intersection(range(q, q + 4)) and q + 4 <= len(flat):
            out.append(("implicit", q, proj_buffer(flat[q:q + 4], False)))
    return tuple(out)


# --- headers ---
class Header:
    """The Gnm header of a shader, from the index (shaders.regs/slots/semantics)."""
    __slots__ = ("stage", "words", "slots", "semantics", "regs_raw")

    def __init__(self, stage: str, regs: bytes, slots: bytes, semantics: bytes):
        self.stage = stage
        self.regs_raw = regs
        names = {"VS": VS_HDR, "PS": PS_HDR, "CS": CS_HDR}[stage]
        vals = struct.unpack_from("<%dI" % len(names), regs)
        self.words = dict(zip(names, vals))
        self.slots = slots
        self.semantics = semantics

    def vs_exports(self):
        n_in, n_exp = self.regs_raw[0x14], self.regs_raw[0x15]
        return struct.unpack_from("<%dH" % n_exp, self.semantics, 4 * n_in)

    def ps_inputs(self):
        return struct.unpack("<%dH" % (len(self.semantics) // 2), self.semantics)


def load_headers(db) -> Dict[int, Header]:
    """hash (u64) -> Header, for every shader of the index."""
    out = {}
    for h, st, regs, slots, sem in db.execute("SELECT hash, stage, regs, slots, semantics FROM shaders"):
        if st in ("VS", "PS", "CS") and regs:
            out[h & 0xFFFFFFFFFFFFFFFF] = Header(st, regs, slots or b"", sem or b"")
    return out


# --- registers -> key and runtime info ---
def effective_depth(g: Callable[[int], int]) -> tuple:
    """vk_depth_stencil_state.h GetEffectiveDepthStencilState().needs_attachment, then the key
    fields of vk_pipeline_cache.cpp:1357-1375 (z/stencil format, depth samples)."""
    ctl, rc, ro = g(DB_DEPTH_CONTROL), g(DB_RENDER_CONTROL), g(DB_RENDER_OVERRIDE)
    zinfo, sinfo = g(DB_Z_INFO), g(DB_STENCIL_INFO)
    depth_valid = g(DB_Z_READ_BASE) != 0 and bits(zinfo, 0, 2) != 0
    stencil_valid = g(DB_STENCIL_READ_BASE) != 0 and bits(sinfo, 0, 1) != 0
    depth_en = bits(ctl, 1, 1) and depth_valid
    stencil_en = bits(ctl, 0, 1) and stencil_valid
    zfunc = bits(ctl, 4, 3)
    depth_write = depth_en and bits(ctl, 2, 1)
    can_reject = depth_en and ((zfunc != 7 and not bits(ctl, 30, 1)) or (zfunc != 0 and bits(ctl, 31, 1)))
    bounds = depth_valid and bits(ctl, 3, 1)
    sc, rf, rb = g(DB_STENCIL_CONTROL), g(DB_STENCILREFMASK), g(DB_STENCILREFMASK_BF)

    def op_writes(base, mask):
        return bits(mask, 16, 8) != 0 and any(bits(sc, base + 4 * k, 4) for k in range(3))
    s_reject = stencil_en and (bits(ctl, 8, 3) != 7 or (bits(ctl, 7, 1) and bits(ctl, 20, 3) != 7))
    s_write = stencil_en and (op_writes(0, rf) or (bits(ctl, 7, 1) and op_writes(12, rb)))
    d_maint = depth_valid and bits(ro, 29, 1) and (bits(rc, 4, 1) or bits(ro, 27, 1) or bits(ro, 19, 2) != 0)
    s_maint = stencil_valid and bits(ro, 30, 1) and bits(ro, 28, 1)
    need = bool(depth_write or can_reject or bounds or d_maint or s_reject or s_write or s_maint)
    zf = bits(zinfo, 0, 2) if need and depth_valid else 0
    sf = bits(sinfo, 0, 1) if need and stencil_valid else 0
    samples = bits(zinfo, 2, 2) if need else 0
    return (zf, sf, samples, 1 - bits(ro, 16, 1))            # last: key.depth_clamp_enable


def raster(g) -> tuple:
    clip, poly, prim = g(PA_CL_CLIP_CNTL), g(PA_SU_SC_MODE_CNTL), g(VGT_PRIMITIVE_TYPE)
    near, far = bits(clip, 26, 1), bits(clip, 27, 1)
    zclip = int(near == far and not near)
    poly_mode = bits(poly, 5, 3) if bits(poly, 3, 2) else 2
    return (prim, poly_mode, bits(poly, 19, 1), zclip, bits(clip, 19, 1),   # cache.cpp:1366-1370
            bits(g(CB_COLOR_CONTROL), 16, 8))                                # logic_op :1373


def color_targets(g, mrt_written: int) -> tuple:
    """RefreshGraphicsKey color targets (vk_pipeline_cache.cpp:1398-1497): per bound, unmasked
    target the shader writes: format, number type, swap (PsColorBuffer), the export format of
    SPI_SHADER_COL_FORMAT, blend control (when enabled and not bypassed), target mask, samples."""
    if bits(g(CB_COLOR_CONTROL), 4, 3) == 0:                  # OperationMode::Disable :1378
        return ("off",)
    tmask, colfmt = g(CB_TARGET_MASK), g(SPI_SHADER_COL_FORMAT)
    out = []
    for cb in range(NUM_CB):
        base = CB_COLOR0_BASE + CB_COLOR_STRIDE * cb
        info, attrib = g(base + CB_INFO), g(base + CB_ATTRIB)
        mask = bits(tmask, 4 * cb, 4)
        if not (g(base) and bits(info, 2, 5)) or not mask or not (mrt_written >> cb & 1):
            continue
        blend = g(CB_BLEND0_CONTROL + cb)
        blend = blend if bits(blend, 30, 1) and not bits(info, 16, 1) else 0
        out.append((cb, bits(info, 2, 5), bits(info, 8, 3), bits(info, 11, 2),
                    bits(colfmt, 4 * cb, 4), blend, mask, bits(attrib, 15, 2)))
    return tuple(out)


def mrt_written(ps: Optional[Header]) -> int:
    """MRTs the pixel shader exports: SPI_SHADER_COL_FORMAT nibbles != 0 (stands in for
    Info::mrt_mask, vk_pipeline_cache.cpp:1592)."""
    if ps is None:
        return 0
    col = ps.words["COL_FORMAT"]
    return sum(1 << i for i in range(NUM_CB) if bits(col, 4 * i, 4))


def vs_runtime(g, vs: Header) -> tuple:
    """BuildRuntimeInfo Vertex (vk_pipeline_cache.cpp:246-255, 326-336): program settings,
    user clip planes, outputs, clip space, clip disable, step rates, emulated primitives."""
    clip, prim = g(PA_CL_CLIP_CNTL), g(VGT_PRIMITIVE_TYPE)
    return (g(SPI_SHADER_PGM_RSRC1_VS), g(SPI_SHADER_PGM_RSRC2_VS), g(PA_CL_VS_OUT_CNTL),
            bits(clip, 0, 6), bits(clip, 19, 1), int(bits(clip, 16, 1) or prim == 0x11),
            g(VGT_INSTANCE_STEP_RATE_0), g(VGT_INSTANCE_STEP_RATE_1), int(prim in (0x11, 0x13)))


def ps_runtime(g, rt: tuple) -> tuple:
    """BuildRuntimeInfo Fragment (vk_pipeline_cache.cpp:257-305)."""
    dsc = g(DB_SHADER_CONTROL)
    ena, addr = g(SPI_PS_INPUT_ENA), g(SPI_PS_INPUT_ADDR)
    # sample_coverage_ena is bit 14 of SPI_PS_INPUT_ENA/ADDR (regs_shader.h PsInput)
    samples = bits(g(PA_SC_AA_CONFIG), 0, 3) if bits(ena, 14, 1) and bits(addr, 14, 1) else 0
    blend0 = g(CB_BLEND0_CONTROL)
    dual = 0
    if bits(blend0, 30, 1):
        dual_factors = (15, 16, 17, 18)
        dual = int(bits(blend0, 0, 5) in dual_factors or bits(blend0, 8, 5) in dual_factors or
                   (bits(blend0, 29, 1) and (bits(blend0, 16, 5) in dual_factors or
                                             bits(blend0, 24, 5) in dual_factors)))
    # DepthShaderControl (regs_depth.h:26): z export 0, stencil test/op 1-2, coverage to mask 7,
    # mask export 8, depth_before_shader 12 (BP12)
    mrtz = bits(dsc, 0, 1) | (bits(dsc, 1, 1) | bits(dsc, 2, 1)) << 1 | bits(dsc, 8, 1) << 2 | bits(dsc, 7, 1) << 3
    return (g(SPI_SHADER_PGM_RSRC1_PS), g(SPI_SHADER_PGM_RSRC2_PS), ena, addr,
            bits(g(SPI_PS_IN_CONTROL), 0, 6), bits(g(SPI_BARYC_CNTL), 24, 1), bits(dsc, 12, 1),
            samples, g(SPI_SHADER_Z_FORMAT), mrtz, dual)


def ps_inputs(g) -> tuple:
    n = bits(g(SPI_PS_IN_CONTROL), 0, 6)
    return tuple(g(SPI_PS_INPUT_CNTL_0 + i) & 0x7FF for i in range(n))


def cs_runtime(regs: dict) -> tuple:
    """BuildRuntimeInfo Compute (vk_pipeline_cache.cpp:307-320); the shared memory size is not
    compared (runtime_info.h HwComputeRuntimeInfo::operator==)."""
    g = regs.get
    r1, r2 = g(CS_RSRC1, 0), g(CS_RSRC2, 0)
    return (bits(r1, 0, 6), bits(r1, 12, 8), bits(r2, 1, 5), bits(r2, 7, 3),
            g(CS_NUM_THREAD_X, 0) & 0xFFFF, g(CS_NUM_THREAD_X + 1, 0) & 0xFFFF,
            g(CS_NUM_THREAD_X + 2, 0) & 0xFFFF)


def fetch_projection(stage) -> tuple:
    """The fetch shader (its code decides the attribute layout, specialization.h
    fetch_shader_data) and per attribute the V# number class and dst_sel (vs_attribs)."""
    code = bytes(stage.get("fetch") or b"")
    if not code:
        return ()
    attrs = []
    for sg, off, w in stage.get("vsharps") or ():
        f = buffer_fields(w)
        nf = remap_number_format(f["nfmt"], f["dfmt"])
        attrs.append((off, number_class(nf), remap_swizzle(f["dfmt"], f["dst_sel"])))
    return (hashlib.sha1(code).hexdigest()[:16], tuple(attrs))


PARTS_GRAPHICS = ("vs", "ps", "vs_rt", "ps_rt", "ps_in", "rt", "depth", "raster", "vs_res",
                  "ps_res", "fetch")
PARTS_COMPUTE = ("cs", "cs_rt", "cs_res")


def signature(rec: dict, headers: Dict[int, Header]) -> Optional[dict]:
    """The named parts of a record (None if one of its shaders is not in the index)."""
    st = {s["sw"]: s for s in rec["stages"]}
    if rec["kind"] == 1:
        cs = st.get(5)
        h = headers.get(cs["hash"]) if cs else None
        if h is None:
            return None
        return {"cs": cs["hash"], "cs_rt": cs_runtime(rec["regs"]),
                "cs_res": stage_resources(h.slots, cs["flat"], bits(h.words["RSRC2"], 1, 5))}
    regs = rec["regs"]

    def g(i):
        return regs.get(i, 0)
    vs, ps = st.get(3), st.get(0)
    vh = headers.get(vs["hash"]) if vs else None
    ph = headers.get(ps["hash"]) if ps else None
    if vs is None or vh is None or (ps is not None and ph is None):
        return None
    rt = color_targets(g, mrt_written(ph))
    sig = {"vs": vs["hash"], "ps": ps["hash"] if ps else 0,
           "vs_rt": vs_runtime(g, vh), "ps_rt": ps_runtime(g, rt) if ps else (),
           "ps_in": ps_inputs(g) if ps else (), "rt": rt, "depth": effective_depth(g),
           "raster": raster(g), "vs_res": stage_resources(vh.slots, vs["flat"], bits(vh.words["RSRC2"], 1, 5)),
           "ps_res": stage_resources(ph.slots, ps["flat"], bits(ph.words["RSRC2"], 1, 5)) if ps else (),
           "fetch": fetch_projection(vs)}
    return sig


def freeze(sig: dict) -> tuple:
    return tuple(sorted(sig.items()))
