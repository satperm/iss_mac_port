# SPDX-License-Identifier: GPL-2.0-or-later
# Renderer v2 on Linux, behind this port's GPU library interface (gpu/tlggpu.h). Origin and credit:
#   shadps4/  the weshayoub92 shadPS4-perf fork (153a5aa7) with the Castlesea port's local patch
#             (pc-port/third_party/shadPS4-perf-local.patch), the files this build uses
#   gpu/      Castlesea/iss-second-son-pc-port 8e167dc pc-port/gpu, adapted to Linux and to
#             this port's runtime (gpu_host.cpp, precompile_jobs.inl)
#   compat/   stb and nlohmann/json headers, tracy stubs; compat/linux/ the Win32 subset it uses,
#             over POSIX
# The player-facing interface is this port's own (gpu/shim/tlg_overlay.cpp and its settings).
# Sources as pc-port/scripts/build_gpu.py lists them. See README.md.
cmake_minimum_required(VERSION 3.22)
project(issgpu CXX C)
include(CTest)
if (BUILD_TESTING)
    add_executable(test_mouse_aim ../tests/test_mouse_aim.cpp ../src/runtime_mouse_aim.c)
    target_compile_definitions(test_mouse_aim PRIVATE ISS_MOUSE_AIM_TEST_CLOCK)
    target_compile_features(test_mouse_aim PRIVATE cxx_std_20)
    target_compile_options(test_mouse_aim PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    add_test(NAME mouse_aim COMMAND test_mouse_aim)
    add_executable(test_hud_trace ../tests/test_hud_trace.cpp)
    target_compile_features(test_hud_trace PRIVATE cxx_std_20)
    target_compile_options(test_hud_trace PRIVATE -UNDEBUG -mavx -Wall -Wextra -Werror)
    add_test(NAME hud_trace COMMAND test_hud_trace)
    add_executable(test_native_resolution ../tests/test_native_resolution.cpp ../src/runtime_native_resolution.c)
    target_compile_features(test_native_resolution PRIVATE cxx_std_20)
    target_compile_options(test_native_resolution PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    add_test(NAME native_resolution COMMAND test_native_resolution)
    add_executable(test_karma_regression ../tests/test_iss_karma.cpp ../src/runtime_karma.c)
    target_compile_features(test_karma_regression PRIVATE cxx_std_20)
    target_compile_options(test_karma_regression PRIVATE -UNDEBUG -mavx)
    add_test(NAME karma_regression COMMAND test_karma_regression)
    add_executable(test_karma_quarantine ../tests/test_karma_quarantine.cpp ../gpu/shim/tlg_karma_store.cpp ../gpu/shim/tlg_karma_save_session.cpp)
    target_compile_features(test_karma_quarantine PRIVATE cxx_std_20)
    target_compile_options(test_karma_quarantine PRIVATE -UNDEBUG)
    add_test(NAME karma_quarantine COMMAND test_karma_quarantine)
    add_executable(test_karma_save_versions ../tests/test_karma_save_versions.cpp)
    target_compile_features(test_karma_save_versions PRIVATE cxx_std_20)
    target_compile_options(test_karma_save_versions PRIVATE -UNDEBUG)
    add_test(NAME karma_save_versions COMMAND test_karma_save_versions)
    add_executable(test_karma_save_session ../tests/test_karma_save_session.cpp)
    target_compile_features(test_karma_save_session PRIVATE cxx_std_20)
    target_compile_options(test_karma_save_session PRIVATE -UNDEBUG)
    add_test(NAME karma_save_session COMMAND test_karma_save_session)
    add_executable(test_karma_profiles ../tests/test_karma_profiles.cpp)
    target_compile_features(test_karma_profiles PRIVATE cxx_std_20)
    target_compile_options(test_karma_profiles PRIVATE -UNDEBUG)
    add_test(NAME karma_profiles COMMAND test_karma_profiles)
    add_executable(test_exclusive_presentation ../tests/test_exclusive_presentation.cpp)
    target_compile_features(test_exclusive_presentation PRIVATE cxx_std_20)
    target_compile_options(test_exclusive_presentation PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_link_libraries(test_exclusive_presentation PRIVATE Vulkan::Vulkan)
    add_test(NAME exclusive_presentation COMMAND test_exclusive_presentation)
    add_executable(test_graphics_settings ../tests/test_graphics_settings.cpp ../gpu/shim/tlg_settings.cpp)
    target_compile_features(test_graphics_settings PRIVATE cxx_std_20)
    target_compile_options(test_graphics_settings PRIVATE -UNDEBUG)
    target_include_directories(test_graphics_settings PRIVATE ../gpu/shim)
    add_test(NAME graphics_settings COMMAND test_graphics_settings)
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PORT ${CMAKE_CURRENT_SOURCE_DIR}/../gpu)
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4)
set(CS ${CMAKE_CURRENT_SOURCE_DIR}/gpu)

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
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
find_package(Zydis QUIET)
if (NOT Zydis_FOUND)
    pkg_check_modules(ZYDIS REQUIRED IMPORTED_TARGET zydis)
    add_library(Zydis::Zydis ALIAS PkgConfig::ZYDIS)
endif()
if (NOT APPLE) # macOS: CommonCrypto
    pkg_check_modules(CRYPTO REQUIRED IMPORTED_TARGET libcrypto) # BCrypt's SHA-256 (compat/linux/bcrypt.h)
endif()
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
find_package(miniz QUIET)
if (NOT miniz_FOUND)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(${PORT}/third_party/miniz miniz EXCLUDE_FROM_ALL)
    add_library(miniz::miniz ALIAS miniz)
endif()
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)
set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(${PORT}/third_party/sirit sirit EXCLUDE_FROM_ALL)

# Host shaders (the fork's list).
set(SHADER_SRC ${SRC}/video_core/host_shaders)
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
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

# Sources as build_gpu.py: the fork's video core and recompiler, 24 files replaced by overrides.
file(GLOB_RECURSE VIDEO_CORE ${SRC}/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER ${SRC}/shader_recompiler/*.cpp)
set(ALL ${VIDEO_CORE} ${RECOMPILER})
list(FILTER ALL EXCLUDE REGEX "renderdoc\\.cpp$")
set(OVERRIDES
    video_core/buffer_cache/buffer_cache.cpp=buffer_cache.cpp
    video_core/renderer_vulkan/vk_rasterizer.cpp=vk_rasterizer.cpp
    video_core/renderer_vulkan/vk_pipeline_cache.cpp=vk_pipeline_cache.cpp
    video_core/renderer_vulkan/vk_graphics_pipeline.cpp=vk_graphics_pipeline.cpp
    video_core/renderer_vulkan/vk_pipeline_serialization.cpp=vk_pipeline_serialization.cpp
    video_core/renderer_vulkan/vk_runtime.cpp=vk_runtime.cpp
    video_core/amdgpu/liverpool.cpp=liverpool.cpp
    video_core/page_manager.cpp=page_manager.cpp
    video_core/renderer_vulkan/vk_instance.cpp=vk_instance.cpp
    video_core/renderer_vulkan/vk_resource_pool.cpp=vk_resource_pool.cpp
    video_core/buffer_cache/buffer.cpp=buffer.cpp
    video_core/renderer_vulkan/vk_presenter.cpp=vk_presenter.cpp
    video_core/renderer_vulkan/vk_scheduler.cpp=vk_scheduler.cpp
    video_core/renderer_vulkan/vk_staging_buffer_pool.cpp=vk_staging_buffer_pool.cpp
    video_core/texture_cache/texture_cache.cpp=texture_cache.cpp
    shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp=flatten_extended_userdata_pass.cpp
    shader_recompiler/recompiler.cpp=recompiler.cpp
    shader_recompiler/ir/passes/resource_patching_pass.cpp=resource_patching_pass.cpp
    shader_recompiler/frontend/fetch_shader.cpp=fetch_shader.cpp
    video_core/cache_storage.cpp=cache_storage.cpp
    video_core/renderer_vulkan/vk_swapchain.cpp=vk_swapchain.cpp
    video_core/texture_cache/image.cpp=image.cpp)
foreach(pair ${OVERRIDES})
    string(REPLACE "=" ";" parts ${pair})
    list(GET parts 0 original)
    list(GET parts 1 replacement)
    list(REMOVE_ITEM ALL ${SRC}/${original})
    list(APPEND ALL ${CS}/overrides/${replacement})
endforeach()
foreach(n decoder error io_file native_clock number_utils rdtsc spin_lock recursive_lock string_util thread signal_context)
    list(APPEND ALL ${SRC}/common/${n}.cpp)
endforeach()
list(APPEND ALL ${SRC}/common/logging/classes.cpp ${CS}/overrides/gnmdriver.cpp
    ${CS}/overrides/videoout_driver.cpp ${SRC}/core/libraries/videoout/video_out.cpp
    ${SRC}/core/libraries/kernel/equeue.cpp
    ${SRC}/imgui/renderer/imgui_impl_vulkan.cpp ${SRC}/imgui/renderer/texture_manager.cpp ${SRC}/common/stb.cpp)
# Ajm, Videodec: this port's runtime implements them (runtime_*.c).
list(APPEND ALL ${SRC}/core/libraries/videodec/video_utils.cpp)
file(GLOB CS_SOURCES ${CS}/*.cpp)
# This port's own interface (its look): the overlay, menu, settings, save dialogs, game hooks.
set(OURS_SOURCES ${PORT}/shim/tlg_overlay.cpp ${PORT}/shim/tlg_settings.cpp
    ${PORT}/shim/tlg_perf_stats.cpp ${PORT}/shim/tlg_png.cpp ${PORT}/shim/tlg_orbs.cpp
    ${PORT}/shim/tlg_actors.cpp ${PORT}/shim/tlg_view.cpp
    ${PORT}/shim/tlg_karma.cpp ${PORT}/shim/tlg_karma_store.cpp ${PORT}/shim/tlg_karma_save_session.cpp)
# The renderer's own bridge, window and save dialogs: replaced by this port's own.
list(REMOVE_ITEM CS_SOURCES ${CS}/save_dialog_ui.cpp)
list(APPEND ALL ${CS_SOURCES})

set(ISSGPU_KIND SHARED)
add_library(issgpu ${ISSGPU_KIND} ${ALL} ${OURS_SOURCES}
    ${PORT}/third_party/imgui/backends/imgui_impl_vulkan.cpp
    ${PORT}/third_party/imgui/imgui.cpp ${PORT}/third_party/imgui/imgui_draw.cpp
    ${PORT}/third_party/imgui/imgui_tables.cpp ${PORT}/third_party/imgui/imgui_widgets.cpp)
add_dependencies(issgpu host_shaders)
if (TRUE)
    target_include_directories(issgpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat/linux)
endif()
target_include_directories(issgpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat ${CS}/shim ${CS}
    ${PORT}/third_party/imgui/backends ${PORT}/.. ${SRC}/core/libraries/gnmdriver ${SRC} ${SRC}/..
    ${SHADER_INCLUDE} ${PORT}/third_party/gcn/include ${PORT}/third_party/half/include
    ${PORT}/third_party/magic_enum/include ${PORT}/third_party/magic_enum/include/magic_enum
    ${PORT}/third_party/imgui ${PORT}/third_party/xbyak ${PORT} ${PORT}/third_party/miniz ${PORT}/third_party/vulkan-headers/include)
# This port's interface headers (tlg_overlay.h...), after the renderer's own shims: the names they
# share (core/memory.h, sdl_window.h) are the renderer's.
target_include_directories(issgpu PRIVATE ${PORT}/shim)
target_compile_definitions(issgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG IMGUI_USER_CONFIG="imgui/imgui_config.h")
target_compile_options(issgpu PRIVATE -O2 -g1 -fdeclspec -Wno-deprecated-declarations)
target_link_libraries(issgpu PUBLIC Vulkan::Vulkan fmt::fmt Boost::headers tsl::robin_map PkgConfig::XXHASH
    PkgConfig::SDL3 Zydis::Zydis GPUOpen::VulkanMemoryAllocator miniz::miniz PkgConfig::FFMPEG sirit)
target_compile_options(issgpu PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/compat/linux/iss_win_compat.h)
if (APPLE)
    # macOS: runtime symbols resolve at load time against tlg-probe (as port/gpu).
    target_link_options(issgpu PRIVATE -Wl,-undefined,dynamic_lookup)
    target_link_libraries(issgpu PUBLIC "-framework Metal" "-framework QuartzCore")
    target_compile_definitions(issgpu PRIVATE VK_USE_PLATFORM_METAL_EXT)
else()
    target_link_options(issgpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
if (NOT APPLE)
    target_link_libraries(issgpu PRIVATE PkgConfig::CRYPTO)
endif()

# Our overlay's ImGui Vulkan backend loads its functions itself (as in this port's own build).
set_source_files_properties(${OURS_SOURCES} ${PORT}/third_party/imgui/backends/imgui_impl_vulkan.cpp
    PROPERTIES COMPILE_DEFINITIONS "IMGUI_IMPL_VULKAN_NO_PROTOTYPES")
target_compile_definitions(issgpu PRIVATE TLG_RENDERER_V2
    TLG_FONT_PATH="${PORT}/third_party/fonts/DejaVuSans.ttf")

