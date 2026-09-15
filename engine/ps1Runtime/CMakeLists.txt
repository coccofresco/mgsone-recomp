find_package(SDL2 REQUIRED)

# Prefer a complete system Vulkan SDK, while allowing an explicit header tree
# for distributions which ship the loader independently from Vulkan-Headers.
# The fallback is pinned to an immutable upstream Khronos commit so a clean
# build does not depend on headers borrowed from one of the renderer donors.
set(PS1RECOMP_VULKAN_HEADERS_ROOT "" CACHE PATH
    "Vulkan-Headers checkout or include directory (optional override)")
set(PS1RECOMP_VULKAN_LIBRARY "" CACHE FILEPATH
    "Path to the Vulkan loader library (optional override)")
set(PS1RECOMP_GLSLC_EXECUTABLE "" CACHE FILEPATH
    "Path to glslc (optional override)")

find_package(Vulkan QUIET)

unset(_ps1recomp_vulkan_include_dir CACHE)
unset(_ps1recomp_vulkan_include_dir)
if(PS1RECOMP_VULKAN_HEADERS_ROOT)
    find_path(_ps1recomp_vulkan_include_dir
        NAMES vulkan/vulkan.h
        PATHS
            "${PS1RECOMP_VULKAN_HEADERS_ROOT}"
            "${PS1RECOMP_VULKAN_HEADERS_ROOT}/include"
        NO_DEFAULT_PATH
    )
    if(NOT _ps1recomp_vulkan_include_dir)
        message(FATAL_ERROR
            "PS1RECOMP_VULKAN_HEADERS_ROOT does not contain vulkan/vulkan.h: "
            "${PS1RECOMP_VULKAN_HEADERS_ROOT}")
    endif()
elseif(Vulkan_FOUND)
    list(GET Vulkan_INCLUDE_DIRS 0 _ps1recomp_vulkan_include_dir)
else()
    find_path(_ps1recomp_vulkan_include_dir NAMES vulkan/vulkan.h)
endif()

if(NOT _ps1recomp_vulkan_include_dir)
    include(FetchContent)
    FetchContent_Declare(vulkan_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG e3b1eec08173d6b825cd3ac88c885a63b621504a
        GIT_SHALLOW FALSE
        UPDATE_DISCONNECTED TRUE
        # Populate the official source without adding its CMake project. This
        # keeps this project's documented CMake 3.20 minimum intact.
        SOURCE_SUBDIR _ps1recomp_headers_only
    )
    FetchContent_MakeAvailable(vulkan_headers)
    set(_ps1recomp_vulkan_include_dir "${vulkan_headers_SOURCE_DIR}/include")
    message(STATUS
        "System Vulkan headers unavailable; using pinned Khronos Vulkan-Headers")
endif()

if(NOT PS1RECOMP_VULKAN_LIBRARY)
    if(Vulkan_FOUND AND Vulkan_LIBRARY)
        set(_ps1recomp_vulkan_library "${Vulkan_LIBRARY}")
    else()
        find_library(_ps1recomp_vulkan_library NAMES vulkan)
    endif()
    if(NOT _ps1recomp_vulkan_library)
        message(FATAL_ERROR "Vulkan loader library (libvulkan) not found")
    endif()
    set(PS1RECOMP_VULKAN_LIBRARY "${_ps1recomp_vulkan_library}" CACHE FILEPATH
        "Path to the Vulkan loader library (optional override)" FORCE)
endif()

if(NOT PS1RECOMP_GLSLC_EXECUTABLE)
    if(Vulkan_GLSLC_EXECUTABLE)
        set(_ps1recomp_glslc "${Vulkan_GLSLC_EXECUTABLE}")
    else()
        find_program(_ps1recomp_glslc NAMES glslc)
    endif()
    if(NOT _ps1recomp_glslc)
        message(FATAL_ERROR "glslc is required to compile the Vulkan shaders")
    endif()
    set(PS1RECOMP_GLSLC_EXECUTABLE "${_ps1recomp_glslc}" CACHE FILEPATH
        "Path to glslc (optional override)" FORCE)
endif()

add_library(ps1Runtime_vulkan INTERFACE)
target_include_directories(ps1Runtime_vulkan INTERFACE
    "${_ps1recomp_vulkan_include_dir}"
)
target_link_libraries(ps1Runtime_vulkan INTERFACE
    "${PS1RECOMP_VULKAN_LIBRARY}"
)

set(PS1RECOMP_VULKAN_SHADER_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(PS1RECOMP_VULKAN_SHADER_BINARY_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/compiled_shaders")
set(PS1RECOMP_VULKAN_SHADER_SOURCES
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/present_fullscreen.vert"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/present_native.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/present_replay.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/present_replay_temporal.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/present_replay_depth.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/present_rgba.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/replay_seed.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/replay_native_expand.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/replay_copy_scratch.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/replay_precise.vert"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/replay_visibility_precise.vert"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/replay_precise.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/replay_opaque_untextured.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/replay_opaque_textured.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/replay_semi_textured.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/crash3_semi_untextured.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/crash3_semi_textured.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/crash3_sidecar.vert"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/crash3_sidecar_pulled.vert"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/crash3_sidecar_model_pulled.vert"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/crash3_sidecar_raw_pulled.vert"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/crash3_sidecar_pulled_interpolated.vert"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/crash3_sidecar_midpoint.comp"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/crash3_sidecar_midpoint_fp32.comp"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/crash3_sidecar_opaque.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/crash3_sidecar_semi_untextured.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/crash3_sidecar_semi_textured.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/crash3_sidecar_semi_interlock.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/replay_visibility_owner.frag"
    "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/replay_visibility_stack.frag"
)
file(MAKE_DIRECTORY "${PS1RECOMP_VULKAN_SHADER_BINARY_DIR}")

set(PS1RECOMP_VULKAN_SHADER_BINARIES)
set(PS1RECOMP_VULKAN_SHADER_RUNTIME_BINARIES)
foreach(shader_source IN LISTS PS1RECOMP_VULKAN_SHADER_SOURCES)
    get_filename_component(shader_name "${shader_source}" NAME)
    set(shader_dependencies "${shader_source}")
    # Both direct and GPU-pulled shaders share the fragment projective-depth
    # contract. Header-only iterations must rebuild every consuming module.
    list(APPEND shader_dependencies
        "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/mgs_original_mesh.glsl"
        "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/mgs_asset_storage.glsl"
        "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/mgs_original_texture.glsl"
        "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/mgs_screen_texture.glsl"
        "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/replay_depth_vertex.glsl"
        "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/replay_depth_fragment.glsl")
    if(shader_name STREQUAL "replay_precise.vert" OR
            shader_name MATCHES "^crash3_sidecar.*[.]vert$")
        list(APPEND shader_dependencies
            "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/games/crash3_background_depth.glsl")
    endif()
    set(shader_binary
        "${PS1RECOMP_VULKAN_SHADER_BINARY_DIR}/${shader_name}.spv")
    if(shader_name STREQUAL "crash3_sidecar_raw_pulled.vert")
        list(APPEND shader_dependencies
            "${PS1RECOMP_VULKAN_SHADER_SOURCE_DIR}/gte_color_math.glsl")
    endif()
    add_custom_command(
        OUTPUT "${shader_binary}"
        COMMAND "${PS1RECOMP_GLSLC_EXECUTABLE}"
            --target-env=vulkan1.0 -O
            -o "${shader_binary}" "${shader_source}"
        DEPENDS ${shader_dependencies}
        COMMENT "Compiling Vulkan shader ${shader_name}"
        VERBATIM
    )
    list(APPEND PS1RECOMP_VULKAN_SHADER_BINARIES "${shader_binary}")
    set(shader_runtime_binary
        "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader_name}.spv")
    add_custom_command(
        OUTPUT "${shader_runtime_binary}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${shader_binary}" "${shader_runtime_binary}"
        DEPENDS "${shader_binary}"
        COMMENT "Staging Vulkan shader ${shader_name} beside the runtime"
        VERBATIM
    )
    list(APPEND PS1RECOMP_VULKAN_SHADER_RUNTIME_BINARIES
        "${shader_runtime_binary}")
endforeach()

add_custom_target(ps1Runtime_vulkan_shaders
    DEPENDS ${PS1RECOMP_VULKAN_SHADER_RUNTIME_BINARIES}
)

# Runtime library (shared between exe and tests)
add_library(ps1Runtime_lib STATIC
    src/gte.cpp
    src/games/crash3_object_presenter.cpp
    src/games/crash3_scenery_detail.cpp
    src/games/crash3_scenery_refinement.cpp
    src/games/crash3_scenery_construction.cpp
    src/games/crash3_profile.cpp
    src/games/mgs_profile.cpp
    src/games/mgs_decomp.cpp
    src/games/mgs_decomp/texture.c
    src/games/mgs_asset_cache.cpp
    src/games/mgs_scene.cpp
    src/games/mgs_pose.cpp
    src/games/mgs_mesh_replay.cpp
    src/games/mgs_source_material.cpp
    src/memory_access.cpp
    src/mips_interpreter.cpp
    src/mips_construction_lineage.cpp
    src/mips_value_flow.cpp
    src/metrics.cpp
    src/pgxp.cpp
    src/gool/emu_compat.cpp
    src/gool/gool_interp.cpp
    src/cdrom/cue_parser.cpp
    src/cdrom/bin_reader.cpp
    src/cdrom/iso9660.cpp
    src/cdrom/exe_extractor.cpp
    src/cdrom/virtual_fs.cpp
    src/cdrom/cdrom_controller.cpp
    src/bios/bios.cpp
    src/bios/kernel_threads.cpp
    src/bios/bios_access.cpp
    src/bios/syscall_a.cpp
    src/bios/syscall_b.cpp
    src/bios/syscall_c.cpp
    src/bios/heap.cpp
    src/bios/event_system.cpp
    src/bios/file_io.cpp
    src/gpu/gpu.cpp
    src/gpu/display_geometry.cpp
    src/gpu/enhanced_surface.cpp
    src/gpu/modern_frame_interpolation.cpp
    src/gpu/modern_replay.cpp
    src/gpu/modern_replay_execution_plan.cpp
    src/gpu/modern_replay_vulkan_bake.cpp
    src/games/mgs_screen_effects.cpp
    src/gpu/modern_replay_vulkan_copy.cpp
    src/gpu/modern_replay_vulkan_diagnostics.cpp
    src/gpu/modern_replay_depth_family.cpp
    src/gpu/modern_replay_visibility_manifest.cpp
    src/gpu/modern_replay_visibility_audit.cpp
    src/gpu/modern_replay_visibility_stack.cpp
    src/gpu/modern_replay_preflight.cpp
    src/gpu/modern_replay_recorder.cpp
    src/gpu/modern_replay_submission.cpp
    src/gpu/vulkan_replay_capability.cpp
    src/spu/spu.cpp
    src/dma/dma.cpp
    src/input/memory_card_files.cpp
    src/input/input.cpp
    src/input/input_script.cpp
    src/mdec/mdec.cpp
    src/timers/timers.cpp
    src/save_state.cpp
    src/config.cpp
    src/overlay_manager.cpp
    src/psyq/psyq_hle.cpp
    src/psyq/psyq_registry.cpp
    src/psyq/psyq_libapi.cpp
    src/psyq/psyq_libcd.cpp
    src/psyq/psyq_libetc.cpp
    src/psyq/psyq_libgte.cpp
    src/psyq/psyq_libgpu.cpp
    src/psyq/psyq_font.cpp
    src/psyq/psyq_pad.cpp
    src/psyq/psyq_libc.cpp
    src/psyq/psyq_state.cpp
)

# These bounded hand-written runtime paths dominate the native and precision
# profiles but are tiny compared with the generated game translation. Giving
# them -O3 costs only seconds during an incremental build and avoids making the
# much larger shards pay the clean-build time/memory cliff measured at -O2 and
# above.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        src/gpu/gpu.cpp
        src/games/crash3_profile.cpp
        src/gte.cpp
        src/memory_access.cpp
        src/metrics.cpp
        src/pgxp.cpp
        PROPERTIES
        COMPILE_OPTIONS "-O3")
endif()

target_include_directories(ps1Runtime_lib
    PUBLIC include
    PRIVATE src
)

target_link_libraries(ps1Runtime_lib
    PUBLIC fmt::fmt
)

# Recompiled output generation (manual; see CONTRIBUTING.md)
# Auto-generation from spinningCube is disabled when using custom game configs.
# To regenerate manually:
#   ./build/ps1Recomp/ps1Recomp <game_config.toml> ps1Runtime/src/recompiled_out.cpp
#
# add_custom_command(
#     OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/recompiled_out.cpp
#     COMMAND ps1Analyzer ${CMAKE_SOURCE_DIR}/test_roms/example08_spinningCube.elf config.toml
#     COMMAND ps1Recomp config.toml ${CMAKE_CURRENT_SOURCE_DIR}/src/recompiled_out.cpp
#     DEPENDS ps1Analyzer ps1Recomp ${CMAKE_SOURCE_DIR}/test_roms/example08_spinningCube.elf
#     WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
#     COMMENT "Auto-generating recompiled_out.cpp via ps1Analyzer and ps1Recomp"
# )

# Runtime executable
# PS1RECOMP_USE_STUB: force stub mode even when recompiled_out.cpp exists.
# Useful for CI pipelines and contributors who don't have a game ROM.
option(PS1RECOMP_USE_STUB "Use stub recompiled_out instead of real game" OFF)
option(PS1RECOMP_DIAGNOSTIC_FAST_COMPILE
    "Compile generated game shards quickly while retaining useful optimization" OFF)

if(PS1RECOMP_USE_STUB)
    set(RECOMP_SRC src/recompiled_out_stub.cpp)
    message(STATUS "PS1RECOMP_USE_STUB=ON -- using stub (no ROM required)")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/recompiled_out.cpp")
    set(RECOMP_SRC src/recompiled_out.cpp)
    set(RECOMP_SHARD_HEADER
        "${CMAKE_CURRENT_SOURCE_DIR}/src/recompiled_out.generated.h")
    if(EXISTS "${RECOMP_SHARD_HEADER}")
        file(GLOB RECOMP_SHARD_SRC CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/recompiled_out.part*.cpp")
        if(NOT RECOMP_SHARD_SRC)
            message(FATAL_ERROR
                "recompiled_out.generated.h exists but no translation shards were found")
        endif()
        list(APPEND RECOMP_SRC ${RECOMP_SHARD_SRC})
        list(LENGTH RECOMP_SHARD_SRC RECOMP_SHARD_COUNT)
        message(STATUS "Using ${RECOMP_SHARD_COUNT} generated translation shards")
    endif()
else()
    set(RECOMP_SRC src/recompiled_out_stub.cpp)
    message(STATUS "recompiled_out.cpp not found -- using stub (CI/test build)")
endif()

add_library(ps1Runtime_vulkan_renderer STATIC
    src/gpu/renderer_vulkan.cpp
    src/gpu/mgs_asset_cache_vulkan.cpp
)

# The sidecar's capture/profile and Vulkan presentation sources are bounded
# hand-written hot paths.  Keeping them at -O3 is independent from the cheap
# -O1 generated-game shards and adds only a short incremental compile, while
# preserving the fast-build contract for recomp iteration.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/gpu/renderer_vulkan.cpp PROPERTIES
        COMPILE_OPTIONS "-O3")
endif()
add_dependencies(ps1Runtime_vulkan_renderer ps1Runtime_vulkan_shaders)
target_include_directories(ps1Runtime_vulkan_renderer
    PUBLIC include
    PRIVATE src
)
target_link_libraries(ps1Runtime_vulkan_renderer
    PUBLIC ps1Runtime_lib
    PRIVATE
        fmt::fmt
        SDL2::SDL2
        ps1Runtime_vulkan
)
target_compile_definitions(ps1Runtime_vulkan_renderer PRIVATE
    PS1RECOMP_VULKAN_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders"
)

add_executable(ps1Runtime
    src/main_host.cpp
    ${RECOMP_SRC}
)
# Generated translations are exceptionally large and are not a practical
# source-level debugging surface.  Suppressing their DWARF in debug-info build
# types shortens the one unavoidable game-object rebuild and keeps renderer
# iteration from spending minutes in debug-info generation. Runtime libraries
# and the hand-written host retain the selected build type's normal symbols.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(RECOMP_COMPILE_OPTIONS "-g0")
    if(PS1RECOMP_DIAGNOSTIC_FAST_COMPILE AND NOT PS1RECOMP_USE_STUB)
        # A monolithic generated translation still needs -O0 to avoid the
        # compiler memory/time cliff. Sharded translations are small enough
        # for -O1, which retains useful optimization while keeping a clean
        # six-job build near three minutes. Measured -O2 builds took almost
        # eleven minutes for only about three percent more guest throughput.
        if(RECOMP_SHARD_SRC)
            list(APPEND RECOMP_COMPILE_OPTIONS "-O1")
            message(STATUS
                "PS1RECOMP_DIAGNOSTIC_FAST_COMPILE=ON -- generated shards use -O1")
        else()
            list(APPEND RECOMP_COMPILE_OPTIONS "-O0")
            message(STATUS
                "PS1RECOMP_DIAGNOSTIC_FAST_COMPILE=ON -- monolithic game TU uses -O0")
        endif()
    endif()
    set_source_files_properties(${RECOMP_SRC} PROPERTIES
        COMPILE_OPTIONS "${RECOMP_COMPILE_OPTIONS}")
endif()
add_dependencies(ps1Runtime ps1Runtime_vulkan_shaders)

target_include_directories(ps1Runtime
    PUBLIC include
    PRIVATE src
)

target_link_libraries(ps1Runtime
    PRIVATE
        ps1Runtime_lib
        ps1Runtime_vulkan_renderer
        fmt::fmt
        SDL2::SDL2
        ps1Runtime_vulkan
        ps1Analyzer_lib
)
