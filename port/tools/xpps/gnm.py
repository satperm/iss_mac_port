"""GCN shaders inside the packages: OrbShdr footer, code start and the Gnm VS/PS/CS headers.

OrbShdr footer (AmdGpu::BinaryInfo, 28 bytes), right after the code:
    0..6   "OrbShdr"            7  version
    8..11  u32: bit0 pssl_or_cg, bit1 cached, bits2-5 stage type, bits6-7 source, bits8-31 length
    12..15 usage info          16..23 u64 shader hash (the key the port uses)   24..27 crc32
The `length` field is NOT the distance from the code start. The code starts with
    s_mov_b32 vcc_hi, literal   (code[0] = 0xBEEB03FF, code[1] = literal)
and the footer sits at start + 8*(code[1] + 1) (the test of FindBinaryInfo in the port), so
    start = footer - 8*(code[1] + 1).

Gnm shader header (CPU memory, one per shader occurrence, pointed to by pass records):
    +0x00 u32 common: m_shaderSize:23 | m_shaderIsUsingSrt:1 | m_numInputUsageSlots:8
    +0x04 u16 embedded constant buffer size (DQW), u16 scratch size
    +0x08 u64 code pointer (offset of the code start; relocation type 4)
    VS: +0x10 RSRC1, RSRC2, SPI_VS_OUT_CONFIG, SPI_SHADER_POS_FORMAT, PA_CL_VS_OUT_CNTL;
        +0x24 u8 numInputSemantics, numExportSemantics, gsModeOrNumInputSemanticsCs, fetchControl;
        slots at +0x28, then numInputSemantics x {semantic, vgpr, sizeInElements, reserved},
        then numExportSemantics x u16
    PS: +0x10 RSRC1, RSRC2, SPI_SHADER_Z_FORMAT, SPI_SHADER_COL_FORMAT, SPI_PS_INPUT_ENA,
        SPI_PS_INPUT_ADDR, SPI_PS_IN_CONTROL, SPI_BARYC_CNTL, DB_SHADER_CONTROL, CB_SHADER_MASK;
        +0x38 u8 numInputSemantics; slots at +0x3C, then numInputSemantics x u16
    CS: +0x10 RSRC1, RSRC2, NUM_THREAD_X, Y, Z, ...; slots at +0x2C
    m_shaderSize = footer + 28 - code start (code + footer).
InputUsageSlot (4 bytes): {usageType, apiSlot, startRegister, flags}. Types seen: 0 resource,
1 sampler, 2 constant buffer, 4 writable resource, 0x12 fetch shader pointer, 0x17 vertex buffer
table, 0x1B pointer to the extended user data (registers >= 16 live there).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Optional

SIGNATURE = b"OrbShdr"
FOOTER_SIZE = 28
FLAGS_OFFSET = 8
HASH_OFFSET = 16
TYPE_SHIFT, TYPE_MASK = 2, 0xF
LENGTH_SHIFT = 8
CODE_MARKER = 0xBEEB03FF
CODE_MARKER_BYTES = struct.pack("<I", CODE_MARKER)
# The code start is searched this far before footer - length (it was enough for all 314,869
# occurrences of the game; anything farther is reported as "no start").
START_SEARCH_SLACK = 0x2000

STAGE_PS, STAGE_VS, STAGE_GS, STAGE_CS = 0, 1, 2, 4
STAGE_NAMES = {STAGE_PS: "PS", STAGE_VS: "VS", STAGE_GS: "GS", STAGE_CS: "CS"}

SHADER_SIZE_MASK = 0x7FFFFF
REGS_OFFSET = 0x10
SLOTS_OFFSET = {STAGE_VS: 0x28, STAGE_PS: 0x3C, STAGE_CS: 0x2C}
SLOT_SIZE = 4

# s_setpc_b64 (SOP1 opcode 0x20, any SDST/SSRC): ends a fetch shader.
SETPC_MASK, SETPC_VALUE = 0xFFFFFF00, 0xBE802000
FETCH_MAX = 0x1000


def stage_name(type_id: int) -> str:
    return STAGE_NAMES.get(type_id, f"T{type_id}")


@dataclass(frozen=True)
class Footer:
    pos: int            # where "OrbShdr" starts (same base as the buffer that was read)
    type: int
    length: int
    hash: int

    @property
    def stage(self) -> str:
        return stage_name(self.type)


def parse_footer(buf, pos: int) -> Optional[Footer]:
    """Decode the OrbShdr at buf[pos:]; None when it is cut by the end of the buffer."""
    if pos < 0 or pos + FOOTER_SIZE > len(buf) or bytes(buf[pos:pos + 7]) != SIGNATURE:
        return None
    (flags,) = struct.unpack_from("<I", buf, pos + FLAGS_OFFSET)
    (shader_hash,) = struct.unpack_from("<Q", buf, pos + HASH_OFFSET)
    return Footer(pos, (flags >> TYPE_SHIFT) & TYPE_MASK, flags >> LENGTH_SHIFT, shader_hash)


def code_starts(buf, footer_pos: int, lo: int = 0, length: Optional[int] = None) -> list:
    """Every 4-aligned p in [lo, footer) with code[0] = 0xBEEB03FF and p + 8*(code[1]+1) == footer.

    The search window starts START_SEARCH_SLACK bytes before footer - length (length = the
    footer's length field when not given). One result means a valid shader.
    """
    if length is None:
        (flags,) = struct.unpack_from("<I", buf, footer_pos + FLAGS_OFFSET)
        length = flags >> LENGTH_SHIFT
    lo = max(lo, footer_pos - length - START_SEARCH_SLACK)
    found = []
    k = footer_pos
    while True:
        k = buf.rfind(CODE_MARKER_BYTES, lo, k)
        if k < 0:
            break
        if (footer_pos - k) % 4 == 0 and k + 8 <= footer_pos:
            (w1,) = struct.unpack_from("<I", buf, k + 4)
            if (w1 + 1) * 8 == footer_pos - k:
                found.append(k)
        if k == lo:
            break
    return found


def footer_from_start(buf, start: int) -> Optional[int]:
    """Footer position for code that starts at `start`, or None if the code does not follow the rule."""
    if start < 0 or start + 8 > len(buf):
        return None
    w0, w1 = struct.unpack_from("<II", buf, start)
    if w0 != CODE_MARKER:
        return None
    pos = start + 8 * (w1 + 1)
    if pos + FOOTER_SIZE > len(buf) or bytes(buf[pos:pos + 7]) != SIGNATURE:
        return None
    return pos


@dataclass(frozen=True)
class GnmHeader:
    stage: int
    common: int
    code_ptr: int
    regs: bytes        # +0x10 .. slots offset (registers and the u8 counts; no code pointer)
    slots: bytes       # numInputUsageSlots x 4
    semantics: bytes   # input (and, for VS, export) semantics
    size: int          # total header bytes

    @property
    def shader_size(self) -> int:
        return self.common & SHADER_SIZE_MASK

    @property
    def uses_srt(self) -> bool:
        return bool(self.common >> 23 & 1)

    @property
    def num_slots(self) -> int:
        return self.common >> 24

    def input_usage_slots(self) -> list:
        """[(usageType, apiSlot, startRegister, flags), ...]"""
        return [tuple(self.slots[i:i + 4]) for i in range(0, len(self.slots), 4)]


def parse_header(buf, pos: int, stage: int) -> Optional[GnmHeader]:
    """Gnm header of a VS/PS/CS shader at buf[pos:]; None for other stages or a cut header."""
    so = SLOTS_OFFSET.get(stage)
    if so is None or pos < 0 or pos + so > len(buf):
        return None
    (common,) = struct.unpack_from("<I", buf, pos)
    (code_ptr,) = struct.unpack_from("<Q", buf, pos + 8)
    nslots = common >> 24
    sem_off = pos + so + SLOT_SIZE * nslots
    if stage == STAGE_VS:
        n_in, n_exp = buf[pos + 0x24], buf[pos + 0x25]
        sem_len = 4 * n_in + 2 * n_exp
    elif stage == STAGE_PS:
        sem_len = 2 * buf[pos + 0x38]
    else:
        sem_len = 0
    end = sem_off + sem_len
    if end > len(buf):
        return None
    return GnmHeader(stage, common, code_ptr, bytes(buf[pos + REGS_OFFSET:pos + so]),
                     bytes(buf[pos + so:sem_off]), bytes(buf[sem_off:end]), end - pos)


def fetch_size(buf, pos: int, limit: int = FETCH_MAX) -> Optional[int]:
    """Bytes of a fetch shader up to and including its s_setpc_b64; None if not found."""
    end = min(len(buf), pos + limit)
    for p in range(pos, end - 3, 4):
        (w,) = struct.unpack_from("<I", buf, p)
        if w & SETPC_MASK == SETPC_VALUE:
            return p + 4 - pos
    return None
