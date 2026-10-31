# SPDX-License-Identifier: GPL-2.0-or-later
"""The "AMD cards" preset: the currently recommended setup minus what is known or suspected to hurt on AMD.

A full preset (see presets_current.py): it owns only the keys it lists. Differences from the currently
recommended preset:
- BP06 (maximal reconvergence) off: it became a default only on 2026-10-16 and a player with an RX 7600
  (driver 26.8.1) cannot open the game since; suspected, not proven. It is on in the other preset.
- BP08 (the driver's own pipeline cache file) off: not verified on AMD drivers, so the cautious choice.
- BLOCK1D, BC_TINY and NO_MSAA forced on instead of Automatic: the workarounds for the 1D-image crash of
  AMD's Windows driver, which are Automatic only for Windows + AMD/Intel.
Not included, on purpose: BP09 (1D images as 2D) is known not to fix the RX 580 crash (it skips the CPU
decode and fails at the same address). For RX 580 / Polaris, a separate experimental preset opts into barrier merging (#232/#278).
"""
from presets_current import CURRENT

AMD = {
    'id': 'amd',
    'exported': '2026-10-16',
    'names': {'en': 'AMD CARDS (RECOMMENDED)', 'pt': 'PLACAS AMD (RECOMENDADA)'},
    'what': {
        'en': 'The recommended setup with the safest choices for AMD cards: maximal reconvergence and the driver '
              'pipeline cache off, the 1D-image workarounds on. For RX 580, try the separate experimental compatibility preset.',
        'pt': 'A configuração recomendada com as escolhas mais seguras para placas AMD: reconvergência máxima e cache '
              'de pipelines do driver desligados, contornos de imagem 1D ligados. Para RX 580, experimente o perfil separado de '
              'compatibilidade experimental.',
    },
    'settings': dict(CURRENT['settings']),
    'items': dict(CURRENT['items'], BP06='0', BP08='0', TLG_BLOCK1D='1', TLG_BC_TINY='1', TLG_NO_MSAA='1'),
}
