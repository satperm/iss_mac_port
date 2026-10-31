# tlg GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(tlggpu LANGUAGES C CXX)
# Standalone diagnostics tests can be built without the GPU dependency stack:
# cmake -S port/tests/perf -B <test-build>

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# tlg: link-time optimization (TLG_LTO, default on) and profile-guided optimization
# (TLG_PGO: off, generate = instrumented build writing into TLG_PGO_DIR, use = build with that
# profile). Covers tlggpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(TLG_LTO "Link-time optimization of tlggpu" ON)
set(TLG_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(TLG_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (TLG_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT tlg_ipo_ok OUTPUT tlg_ipo_error)
    if (tlg_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${tlg_ipo_error}")
    endif()
endif()
if (TLG_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${TLG_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${TLG_PGO_DIR})
    add_compile_definitions(TLG_PGO_GENERATE)
elseif (TLG_PGO STREQUAL "use" AND APPLE)
    # macOS: clang's profile, merged from the .profraw files of a generate run:
    # xcrun llvm-profdata merge -o pgo/tlggpu.profdata pgo/*.profraw
    add_compile_options(-fprofile-use=${TLG_PGO_DIR}/tlggpu.profdata -Wno-profile-instr-out-of-date
                        -Wno-profile-instr-unprofiled -Wno-profile-instr-missing)
elseif (TLG_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, tlggpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${TLG_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

find_package(Vulkan REQUIRED)
# Pinned Vulkan-Headers (submodule) ahead of the system ones: distro headers can lag behind the
# extensions the shadPS4 video core uses (e.g. Fedora 44 ships 1.4.341, VALVE mixed float dot is newer).
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers/include/vulkan/vulkan.hpp)
    include_directories(BEFORE SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers/include)
endif()
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
# The pinned submodule is preferred: Fedora's magic_enum installs flat headers (no magic_enum/ dir).
if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/magic_enum/include/magic_enum/magic_enum.hpp)
    find_package(magic_enum QUIET)
endif()
if (NOT magic_enum_FOUND)
    add_library(magic_enum INTERFACE)
    add_library(magic_enum::magic_enum ALIAS magic_enum)
    target_include_directories(magic_enum INTERFACE third_party/magic_enum/include
                                                    third_party/magic_enum/include/magic_enum)
endif()
find_package(tsl-robin-map REQUIRED)
find_package(VulkanMemoryAllocator CONFIG QUIET)
if (NOT VulkanMemoryAllocator_FOUND)
    # Some distributions ship the header (include/vma/vk_mem_alloc.h) without a CMake package.
    find_path(VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)
    add_library(VulkanMemoryAllocator INTERFACE)
    add_library(GPUOpen::VulkanMemoryAllocator ALIAS VulkanMemoryAllocator)
    target_include_directories(VulkanMemoryAllocator INTERFACE ${VMA_INCLUDE_DIR})
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
if (NOT APPLE)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()
find_package(miniz QUIET)
if (NOT miniz_FOUND)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(third_party/miniz EXCLUDE_FROM_ALL)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    add_library(miniz::miniz ALIAS miniz)
endif()
# tlg: no Zydis: the SRT walker is portable bytecode now (bloodborne_mac_port), and nothing else
# decoded x86 code (common/decoder.cpp was unused), so the library also builds for arm64.
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# tlg: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_fallback_frag SAMPLE_FALLBACK=1)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(quad_indirect.comp quad_indirect_comp)
add_host_shader(quad_index_expand.comp quad_index_expand_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
# tlg: the shader cache key: a hash of every source that shapes the generated SPIR-V. The
# shader cache directory carries it, so a changed recompiler starts a fresh cache instead of
# reusing shaders translated by the old code (build.sh configures on every build).
file(GLOB_RECURSE TLG_SHADER_KEY_SOURCES shadps4/shader_recompiler/*.cpp shadps4/shader_recompiler/*.h)
list(APPEND TLG_SHADER_KEY_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/amdgpu/resource.h
    # tlg: the 1D/BC promotion predicates that resource.h and the SPIR-V image emitter read
    # (IsBlock1D): their code changes which texels a shader samples, without changing the profile
    # the cache compares.
    ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/amdgpu/tlg_block1d.h
    ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/amdgpu/tlg_bc_tiny.h
    # tlg: the graphics pipeline writes the host tessellation/discard SPIR-V into its serialized
    # pipeline entry and decides when to emit it.
    ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp)
list(SORT TLG_SHADER_KEY_SOURCES)
set(TLG_SHADER_KEY_DATA "")
foreach(source ${TLG_SHADER_KEY_SOURCES})
    file(SHA1 ${source} source_hash)
    string(APPEND TLG_SHADER_KEY_DATA ${source_hash})
endforeach()
string(SHA1 TLG_SHADER_KEY "${TLG_SHADER_KEY_DATA}")
string(SUBSTRING ${TLG_SHADER_KEY} 0 12 TLG_SHADER_KEY)
add_compile_definitions(TLG_SHADER_CACHE_KEY="${TLG_SHADER_KEY}")
message(STATUS "Shader cache key: ${TLG_SHADER_KEY}")

list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
add_library(tlggpu SHARED ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
    shadps4/core/libraries/videodec/video_utils.cpp)
# Identify diagnostic captures without exposing a build directory.
include(cmake/PerfBuildRevision.cmake)
tlg_perf_build_revision("${CMAKE_CURRENT_SOURCE_DIR}" TLG_TRACE_REV)
target_compile_definitions(tlggpu PRIVATE TLG_PERF_BUILD_ID="${TLG_TRACE_REV}")
add_dependencies(tlggpu host_shaders)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(tlggpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(tlggpu PRIVATE third_party/imgui third_party/imgui/backends)
target_compile_definitions(tlggpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    TLG_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/tlg_overlay.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf)
target_include_directories(tlggpu PRIVATE shim shadps4 .. ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include)
target_compile_definitions(tlggpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(tlggpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
# Runtime symbols (runtime_memory_*, runtime_process_*) resolve against tlg-probe.
if (APPLE)
    # macOS: resolved at load time against tlg-probe; the window is a Metal layer (MoltenVK).
    target_link_options(tlggpu PRIVATE -Wl,-undefined,dynamic_lookup)
    target_compile_definitions(tlggpu PRIVATE VK_USE_PLATFORM_METAL_EXT)
else()
    target_link_options(tlggpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
target_link_libraries(tlggpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan)
target_link_libraries(tlggpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz sirit)
# xbyak is header-only: the distribution's copy when there is one (Fedora xbyak-devel), else the
# submodule (Ubuntu does not package it).
find_path(XBYAK_SYSTEM_INCLUDE xbyak/xbyak.h)
if (NOT XBYAK_SYSTEM_INCLUDE)
    target_include_directories(tlggpu PRIVATE third_party/xbyak)
endif()
if (APPLE)
    target_link_libraries(tlggpu PUBLIC "-framework Metal" "-framework QuartzCore")
else()
    target_link_libraries(tlggpu PUBLIC PkgConfig::X11)
endif()

# Explicit test targets; neither launches the game nor needs a Vulkan device.
add_executable(pipeline-binary-cache-test EXCLUDE_FROM_ALL ../tests/test_pipeline_binary_cache.cpp
    ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(pipeline-binary-cache-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(pipeline-binary-cache-test PRIVATE tlggpu)
target_compile_options(pipeline-binary-cache-test PRIVATE -fPIC)
add_executable(pipeline-binary-initialization-test EXCLUDE_FROM_ALL
    ../tests/test_pipeline_binary_initialization.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(pipeline-binary-initialization-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(pipeline-binary-initialization-test PRIVATE tlggpu)
target_compile_options(pipeline-binary-initialization-test PRIVATE -fPIC)
set_source_files_properties(../tests/test_pipeline_binary_cache.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_executable(motion-history-test EXCLUDE_FROM_ALL ../tests/test_motion_history.cpp)
target_compile_options(motion-history-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(motion-history-test PRIVATE ..)
add_executable(motion-shader-test EXCLUDE_FROM_ALL ../tests/test_motion_shaders.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(motion-shader-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(motion-shader-test PRIVATE tlggpu)
add_executable(png-decode-test EXCLUDE_FROM_ALL ../tests/test_png_decode.cpp shim/tlg_png.cpp)
target_compile_options(png-decode-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_link_libraries(png-decode-test PRIVATE PkgConfig::FFMPEG)
add_executable(aliasing-test EXCLUDE_FROM_ALL ../tests/test_aliasing.cpp
    shadps4/video_core/texture_cache/aliasing.cpp shadps4/video_core/texture_cache/host_compatibility.cpp)
target_compile_options(aliasing-test PRIVATE -UNDEBUG)
target_include_directories(aliasing-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(aliasing-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)
add_executable(blend-rewrite-test EXCLUDE_FROM_ALL ../tests/test_blend_rewrite.cpp)
target_compile_options(blend-rewrite-test PRIVATE -UNDEBUG)
target_include_directories(blend-rewrite-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(blend-rewrite-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)
add_executable(depth-stencil-state-test EXCLUDE_FROM_ALL ../tests/test_depth_stencil_state.cpp)
add_executable(async-compiler-test EXCLUDE_FROM_ALL ../tests/test_async_compiler.cpp)
target_compile_options(async-compiler-test PRIVATE -UNDEBUG)
target_include_directories(async-compiler-test PRIVATE shadps4)
add_executable(warmup-inbox-test EXCLUDE_FROM_ALL ../tests/test_warmup_inbox.cpp)
target_compile_options(warmup-inbox-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(warmup-inbox-test PRIVATE shadps4)
add_executable(wave-stats-test EXCLUDE_FROM_ALL ../tests/test_wave_stats.cpp)
target_compile_options(wave-stats-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(wave-stats-test PRIVATE shadps4)
# tlg: SotC recompiler fixes (LDS barriers, S_WQM_B64); see the test for its env runs.
add_executable(recompiler-fixes-test EXCLUDE_FROM_ALL ../tests/test_recompiler_fixes.cpp
    ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(recompiler-fixes-test PRIVATE shim shadps4 third_party/gcn/include)
target_compile_options(recompiler-fixes-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(recompiler-fixes-test PRIVATE tlggpu)
# tlg: first-run shader compilation, gate G4.1: translates a shader cache again from its .meta
# files alone and compares the SPIR-V (local tool: needs the player's game files).
add_executable(shader-replay EXCLUDE_FROM_ALL ../tests/shader_replay.cpp
    ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(shader-replay PRIVATE . shim shadps4 third_party/gcn/include)
target_compile_options(shader-replay PRIVATE -UNDEBUG -fPIC)
target_link_libraries(shader-replay PRIVATE tlggpu)
add_executable(pass-census-test EXCLUDE_FROM_ALL ../tests/test_pass_census.cpp)
target_compile_options(pass-census-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(pass-census-test PRIVATE shadps4)
add_executable(frame-capture-test EXCLUDE_FROM_ALL ../tests/test_frame_capture.cpp
    shadps4/video_core/renderer_vulkan/vk_frame_capture.cpp)
target_compile_options(frame-capture-test PRIVATE -UNDEBUG)
target_include_directories(frame-capture-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(frame-capture-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)
add_executable(lighting-test-knob-test EXCLUDE_FROM_ALL ../tests/test_lighting_test.cpp)
target_compile_options(lighting-test-knob-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(lighting-test-knob-test PRIVATE shadps4)
add_executable(bc-decode-test EXCLUDE_FROM_ALL ../tests/test_bc_decode.cpp)
target_compile_options(bc-decode-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(bc-decode-test PRIVATE shadps4)
add_executable(amd-paths-test EXCLUDE_FROM_ALL ../tests/test_amd_paths.cpp)
target_compile_options(amd-paths-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(amd-paths-test PRIVATE shadps4)
add_executable(no-msaa-test EXCLUDE_FROM_ALL ../tests/test_no_msaa.cpp
    shadps4/video_core/amdgpu/pixel_format.cpp)
target_compile_options(no-msaa-test PRIVATE -UNDEBUG)
target_include_directories(no-msaa-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(no-msaa-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)
add_executable(async-pipeline-cache-test EXCLUDE_FROM_ALL ../tests/test_async_pipeline_cache.cpp)
target_compile_options(async-pipeline-cache-test PRIVATE -UNDEBUG)
target_include_directories(async-pipeline-cache-test PRIVATE shadps4)
target_link_libraries(async-pipeline-cache-test PRIVATE Vulkan::Vulkan)
add_executable(async-fetch-snapshot-test EXCLUDE_FROM_ALL ../tests/test_async_fetch_snapshot.cpp
    shadps4/shader_recompiler/frontend/fetch_shader.cpp
    shadps4/shader_recompiler/frontend/decode.cpp
    shadps4/shader_recompiler/frontend/format.cpp)
target_compile_options(async-fetch-snapshot-test PRIVATE -UNDEBUG)
target_include_directories(async-fetch-snapshot-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(async-fetch-snapshot-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)
target_compile_options(depth-stencil-state-test PRIVATE -UNDEBUG)
target_include_directories(depth-stencil-state-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(depth-stencil-state-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ..)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../tests/test_scene_resolution.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_options(scene-resolution-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE tlggpu)
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../tests/test_taa_shader.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(taa-shader-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_taa_shader.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(taa-shader-test PRIVATE tlggpu)
add_dependencies(taa-shader-test host_shaders)

add_executable(camera-motion-test EXCLUDE_FROM_ALL ../tests/test_camera_motion.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(camera-motion-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(camera-motion-test PRIVATE tlggpu)
add_dependencies(camera-motion-test host_shaders)

# tlg: FSR 4 benchmark outside the game (tools/fsr4_bench.cpp): ninja -C out/gpu fsr4-bench.
add_executable(upscaler-support-test EXCLUDE_FROM_ALL ../tests/test_upscaler_support.cpp
    shim/tlg_settings.cpp)
target_include_directories(upscaler-support-test PRIVATE shim)
target_compile_options(upscaler-support-test PRIVATE -UNDEBUG)
add_executable(orb-multiplier-test EXCLUDE_FROM_ALL ../tests/test_orb_multiplier.cpp
    shim/tlg_orbs.cpp shim/tlg_settings.cpp shim/tlg_view.cpp)
target_include_directories(orb-multiplier-test PRIVATE shim)
target_compile_options(orb-multiplier-test PRIVATE -UNDEBUG)
add_executable(stat-multipliers-test EXCLUDE_FROM_ALL ../tests/test_stat_multipliers.cpp
    shim/tlg_settings.cpp)
target_include_directories(stat-multipliers-test PRIVATE shim)
target_compile_options(stat-multipliers-test PRIVATE -UNDEBUG)
add_executable(perf-overlay-test EXCLUDE_FROM_ALL ../tests/test_perf_overlay.cpp
    shim/tlg_settings.cpp)
target_include_directories(perf-overlay-test PRIVATE shim)
target_compile_options(perf-overlay-test PRIVATE -UNDEBUG)
add_executable(cheat-hooks-test EXCLUDE_FROM_ALL ../tests/test_cheat_hooks.cpp)
target_compile_options(cheat-hooks-test PRIVATE -UNDEBUG)
add_executable(readback-queue-test EXCLUDE_FROM_ALL ../tests/test_readback_queue.cpp)

# RX580 opt-in image-barrier coalescing, using Vulkan-Hpp without a GPU/driver.
add_executable(barrier-merge-test EXCLUDE_FROM_ALL ../tests/test_barrier_merge.cpp)
target_include_directories(barrier-merge-test PRIVATE shadps4 third_party/vulkan-headers/include)
target_compile_options(barrier-merge-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(readback-queue-test PRIVATE shadps4)
target_compile_options(readback-queue-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_executable(wait-diagnostics-test EXCLUDE_FROM_ALL ../tests/test_wait_diagnostics.cpp)
target_include_directories(wait-diagnostics-test PRIVATE shadps4)
target_compile_options(wait-diagnostics-test PRIVATE -UNDEBUG)
add_executable(graphics-settings-test EXCLUDE_FROM_ALL ../tests/test_graphics_settings.cpp
    shim/tlg_settings.cpp)
target_include_directories(graphics-settings-test PRIVATE shim)
target_compile_options(graphics-settings-test PRIVATE -UNDEBUG)
add_executable(actor-hook-test EXCLUDE_FROM_ALL ../tests/test_actor_hook.cpp)
target_include_directories(actor-hook-test PRIVATE shim)
target_compile_options(actor-hook-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_executable(readback-sources-test EXCLUDE_FROM_ALL ../tests/test_readback_sources.cpp)
target_include_directories(readback-sources-test PRIVATE shadps4)
target_compile_options(readback-sources-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_executable(bit-array-test EXCLUDE_FROM_ALL ../tests/test_bit_array.cpp)
target_include_directories(bit-array-test PRIVATE ../tests/stubs shim shadps4)
target_compile_options(bit-array-test PRIVATE -UNDEBUG -Wall -Wextra -Werror -mavx2)

add_executable(page-state-test EXCLUDE_FROM_ALL ../tests/test_page_state.cpp)
target_include_directories(page-state-test PRIVATE ../tests/stubs shim shadps4)
target_link_libraries(page-state-test PRIVATE Boost::headers)
target_compile_options(page-state-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)

add_executable(fsr4-bench EXCLUDE_FROM_ALL tools/fsr4_bench.cpp
    shadps4/video_core/renderer_vulkan/fsr411/fsr411.cpp)
target_include_directories(fsr4-bench PRIVATE shadps4/video_core/renderer_vulkan/fsr411)
target_link_libraries(fsr4-bench PRIVATE ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)

add_executable(cheat-hook-test EXCLUDE_FROM_ALL ../tests/test_cheat_hook.cpp)
target_compile_options(cheat-hook-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
