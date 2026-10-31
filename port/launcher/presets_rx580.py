# SPDX-License-Identifier: GPL-2.0-or-later
"""Opt-in RX 580 compatibility preset; based on the AMD preset plus image barrier merging."""
from presets_amd import AMD

RX580 = {
    'id': 'rx580',
    'exported': '2026-10-18',
    'names': {'en': 'RX 580 COMPATIBILITY (EXPERIMENTAL)', 'pt': 'COMPATIBILIDADE RX 580 (EXPERIMENTAL)'},
    'what': {
        'en': 'Recommended AMD settings plus an experimental workaround tested by two RX 580 users in an earlier build. '
              'This version still needs validation. Restart the game after applying.',
        'pt': 'Configuração recomendada para AMD mais um contorno experimental testado por dois usuários de RX 580 em um build anterior. '
              'Esta versão ainda precisa de validação. Reinicie o jogo após aplicar.',
    },
    'settings': dict(AMD['settings']),
    'items': dict(AMD['items'], TLG_BARRIER_MERGE='1', TLG_EXPLICIT_RANGE='1', TLG_BARRIER_SPLIT='0'),
}
