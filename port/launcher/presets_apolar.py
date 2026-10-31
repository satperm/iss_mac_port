# SPDX-License-Identifier: GPL-2.0-or-later
"""Hardware presets contributed by Apolar (https://github.com/sapoguruoficial-cmd).

These are complete profiles imported from the three launcher exports supplied on 2026-10-19.
Machine-specific choices such as the VRAM budget and window mode are intentionally absent from the
export format and remain under the player's control.
"""

_SETTINGS = {
    'show_fps': True, 'eager_readback': False, 'write_merge': True,
    'precompile_shaders': True, 'gpu_readbacks': '2',
}

_COMMON_ITEMS = {
    'menu:rcas_strength': '60', 'menu:region_lod': '0', 'menu:near_clip': '10',
    'menu:frames_queued': '1', 'menu:sensor_aim': '100', 'menu:overlay_fps': '1',
    'menu:overlay_frametime': '0', 'menu:overlay_upscaler': '0', 'menu:overlay_gpu': '0',
    'menu:overlay_cpu': '0', 'menu:overlay_ram': '0', 'menu:overlay_vram': '0',
    'BP01': '1', 'BP02': '1', 'BP03': '1', 'BP04': '1', 'BP05': '1', 'BP06': '1',
    'BP07': '0', 'BP08': '0', 'BP09': '0', 'BP10': '1', 'BP11': '1', 'BP12': '0',
    'TLG_WQM': '1', 'TLG_LDS_SMALL': '1', 'TLG_LDS_MULTI': '1', 'TLG_LDS_READLANE': '1',
    'TLG_LANE_DIVERGENCE': '1', 'TLG_RFL_ALL': '0', 'TLG_ASYNC_SHADERS': '1',
    'TLG_DEFERRED_READBACK': '1', 'TLG_STALE_READBACK': '1', 'TLG_BACKGROUND_WARMUP': '0',
    'TLG_DRAIN_SLEEP': '1', 'TLG_PIPELINE_BINARIES': '0', 'TLG_OCCLUSION_MODE': '4',
    'TLG_VERBOSE_LOG': '0', 'TLG_FULL_BARRIERS': '0', 'TLG_GPU_TIMESTAMP': 'host',
    'TLG_GPU_TIMESTAMP_STEP_US': '20', 'TLG_ASYNC_FENCES': '0', 'TLG_PRESENT_THREAD': '1',
    'TLG_VK_RECORD_THREAD': '1', 'TLG_SUBMIT_DRAWS': '1024', 'TLG_PIPE_VERIFY': '0',
    'TLG_EAGER_READBACK': '0', 'TLG_PARTIAL_HEIGHT_VIEWS': '0', 'TLG_BACKING_INVALIDATE': '0',
    'TLG_WRITE_MERGE': '1', 'TLG_FAST_READ_FAULT': '1', 'TLG_STREAM_MEMORY': 'cached',
    'TLG_VARIANT_REUSE': '0', 'TLG_HEAP_WARM': '1', 'TLG_INDIRECT_CHECK': '1',
    'TLG_ASYNC_WAIT_MS': '0', 'TLG_PARALLEL_WARMUP': '0', 'TLG_COMPUTE_ASYNC_BUILD': '1',
    'TLG_SQUARE_PASS': '1', 'TLG_1D_NO_STORAGE': '0', 'TLG_1D_NO_MUTABLE': '0',
    'TLG_PERF_DIAG': '0', 'TLG_REGION_TRACE': '0', 'TLG_OCCLUSION_LOG': '0',
    'TLG_BC1D_LOG': '0', 'TLG_WAVE_STATS': '0', 'TLG_PREP_WORKERS': '8', 'TLG_COPY_THREADS': '4',
}


def _profile(profile_id, names, what, vsync, items):
    return {
        'id': profile_id,
        'exported': '2026-10-18',
        'names': names,
        'what': what,
        'credit': 'Apolar',
        'credit_url': 'https://github.com/sapoguruoficial-cmd',
        'settings': dict(_SETTINGS, vsync=vsync),
        'items': dict(_COMMON_ITEMS, **items),
    }


APOLAR_AMD = _profile(
    'apolar_amd',
    {'en': 'AMD RX 6600', 'pt': 'AMD RX 6600'},
    {
        'en': 'RX 6600 profile with higher image quality and linear readback disabled for AMD.',
        'pt': 'Perfil para RX 6600 com qualidade gráfica maior e readback linear desligado para AMD.',
    },
    True,
    {
        'menu:fsr1': '0', 'menu:rcas': '1', 'menu:lod_scale': '0.65', 'menu:region_fade': '0.8',
        'menu:far_clip': '1500000', 'menu:mip_lod_bias': '0.5', 'menu:fps_limit': '30',
        'menu:overlay_scale': '100', 'menu:overlay_corner': '1', 'menu:overlay_opacity': '50',
        'menu:overlay_layout': '0', 'TLG_READBACK_LINEAR': '0', 'TLG_DOWNLOAD_WINDOW_KB': '512',
        'TLG_FAULT_WINDOW': '256', 'TLG_COPY_MERGE_KB': '64', 'TLG_STAGING_KEEP_MB': '512',
        'TLG_ASYNC_WAIT_FRAME_MS': '8', 'TLG_DRIVER_CACHE_SAVE_S': '60',
    },
)

APOLAR_RTX = _profile(
    'apolar_rtx',
    {'en': 'RTX + RYZEN 5 4600G', 'pt': 'RTX + RYZEN 5 4600G'},
    {
        'en': 'RTX 3060 and Ryzen 5 4600G profile with linear readback and a shorter draw distance.',
        'pt': 'Perfil para RTX 3060 e Ryzen 5 4600G com readback linear e distância de visão reduzida.',
    },
    True,
    {
        'menu:fsr1': '0', 'menu:rcas': '0', 'menu:lod_scale': '0.45', 'menu:region_fade': '0.8',
        'menu:far_clip': '600000', 'menu:mip_lod_bias': '1.5', 'menu:fps_limit': '30',
        'menu:overlay_scale': '100', 'menu:overlay_corner': '1', 'menu:overlay_opacity': '50',
        'menu:overlay_layout': '0', 'TLG_READBACK_LINEAR': '1', 'TLG_DOWNLOAD_WINDOW_KB': '512',
        'TLG_FAULT_WINDOW': '256', 'TLG_COPY_MERGE_KB': '64', 'TLG_STAGING_KEEP_MB': '512',
        'TLG_ASYNC_WAIT_FRAME_MS': '8', 'TLG_DRIVER_CACHE_SAVE_S': '60',
    },
)

APOLAR_GTX = _profile(
    'apolar_gtx',
    {'en': 'GTX 4–6 GB', 'pt': 'GTX 4–6 GB'},
    {
        'en': 'Profile for GTX cards with 4–6 GB: FSR 1 and RCAS on, linear readback on and VSync off.',
        'pt': 'Perfil para placas GTX de 4–6 GB: FSR 1 e RCAS ligados, readback linear ligado e VSync desligado.',
    },
    False,
    {
        'menu:fsr1': '1', 'menu:rcas': '1', 'menu:lod_scale': '0.45', 'menu:region_fade': '0.7',
        'menu:far_clip': '400000', 'menu:mip_lod_bias': '0.8', 'menu:fps_limit': '24',
        'TLG_READBACK_LINEAR': '1', 'TLG_DOWNLOAD_WINDOW_KB': '256', 'TLG_FAULT_WINDOW': '128',
        'TLG_COPY_MERGE_KB': '32', 'TLG_STAGING_KEEP_MB': '256', 'TLG_ASYNC_WAIT_FRAME_MS': '4',
        'TLG_DRIVER_CACHE_SAVE_S': '120',
    },
)

APOLAR_PRESETS = (APOLAR_AMD, APOLAR_RTX, APOLAR_GTX)
