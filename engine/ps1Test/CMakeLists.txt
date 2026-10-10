# PS1Recomp Tests
# Structure: tests/<component>/test_<module>.cpp

# ------------------------------------------
# Analyzer Tests
# ------------------------------------------
add_executable(ps1Analyzer_tests
    analyzer/test_elf_parser.cpp
    analyzer/test_function_finder.cpp
    analyzer/test_psyq_signatures.cpp
    analyzer/test_psyq_hash_matching.cpp
    analyzer/test_config_generator.cpp
    analyzer/test_overlay_scanner.cpp
    analyzer/test_function_boundaries.cpp
)

target_link_libraries(ps1Analyzer_tests
    PRIVATE
        GTest::gtest_main
        ps1Analyzer_lib
)

# Tests need the data dir (TOML signature DB) at runtime.
target_compile_definitions(ps1Analyzer_tests
    PRIVATE
        PS1RECOMP_DATA_DIR_DEFAULT="${CMAKE_SOURCE_DIR}/ps1Analyzer/data"
)

gtest_discover_tests(ps1Analyzer_tests)

# ------------------------------------------
# Recompiler Tests
# ------------------------------------------
add_executable(ps1Recomp_tests
    recompiler/test_mips_decoder.cpp
    recompiler/test_instruction_emitter.cpp
    recompiler/test_gte_emitter.cpp
    recompiler/test_overlay_handler.cpp
    recompiler/test_hle_emission.cpp
    recompiler/test_dispatch_emitter.cpp
    recompiler/test_forward_branch.cpp
)

target_link_libraries(ps1Recomp_tests
    PRIVATE
        GTest::gtest_main
        ps1Recomp_lib
)

gtest_discover_tests(ps1Recomp_tests)

# ------------------------------------------
# Runtime Tests
# ------------------------------------------
add_executable(mgsController_tests runtime/test_controller_hub.cpp runtime/test_stubs.cpp)
target_link_libraries(mgsController_tests PRIVATE ps1Runtime_lib mgs_controllers mgs_imgui GTest::gtest_main)
target_compile_definitions(mgsController_tests PRIVATE MGS_CONTROLLER_DB="${CMAKE_SOURCE_DIR}/third_party/SDL_GameControllerDB/gamecontrollerdb.txt")
configure_file(${CMAKE_SOURCE_DIR}/third_party/SDL_GameControllerDB/gamecontrollerdb.txt
    ${CMAKE_CURRENT_BINARY_DIR}/gamecontrollerdb.txt COPYONLY)
gtest_discover_tests(mgsController_tests)
if(MGS_ABI_LIBRARY_DIR)
    target_link_libraries(mgsController_tests PRIVATE
        "${MGS_ABI_LIBRARY_DIR}/libm.so.6" "${MGS_ABI_LIBRARY_DIR}/libstdc++.so.6")
    set_target_properties(mgsController_tests PROPERTIES SKIP_BUILD_RPATH TRUE)
endif()

if(NOT WIN32)
add_executable(ps1Runtime_tests
    runtime/test_stubs.cpp
    runtime/test_cpu_context.cpp
    runtime/test_emuptr.cpp
    runtime/test_memory.cpp
    runtime/test_memory_mmio.cpp
    runtime/test_runtime_macros.cpp
    runtime/test_mips_interpreter.cpp
    runtime/test_gte.cpp
    runtime/test_gte_color_oracle.cpp
    runtime/test_crash3_profile.cpp
    runtime/test_memory_card_files.cpp
    runtime/test_mgs_frame_interpolation.cpp
    runtime/test_mgs_profile.cpp
    runtime/test_mgs_asset_cache.cpp
    runtime/test_mgs_screen_effects.cpp
    runtime/test_mgs_surface_effects.cpp
    runtime/test_mgs_pose.cpp
    runtime/test_mgs_decomp.cpp
    runtime/test_crash3_scenery_detail.cpp
    runtime/test_source_surface_partition.cpp
    runtime/test_source_selection_program.cpp
    runtime/test_crash3_scenery_construction.cpp
    runtime/test_validated_index_cache.cpp
    runtime/test_crash3_background_depth.cpp
    runtime/test_cue_parser.cpp
    runtime/test_bin_reader.cpp
    runtime/test_iso9660.cpp
    runtime/test_exe_extractor.cpp
    runtime/test_virtual_fs.cpp
    runtime/test_heap.cpp
    runtime/test_event_system.cpp
    runtime/test_file_io.cpp
    runtime/test_bios.cpp
    runtime/test_bios_table_a.cpp
    runtime/test_bios_table_b.cpp
    runtime/test_bios_table_c.cpp
    runtime/test_bios_integration.cpp
    runtime/test_bios_custom_exception.cpp
    runtime/test_gpu_commands.cpp
    runtime/test_gpu_dma.cpp
    runtime/test_gpu_lines_rects.cpp
    runtime/test_gpu_rasterizer.cpp
    runtime/test_gpu_textures.cpp
    runtime/test_gpu_texture_window.cpp
    runtime/test_gpu_blend.cpp
    runtime/test_gpu_display.cpp
    runtime/test_gpu_display_geometry.cpp
    runtime/test_gpu_enhanced_surface.cpp
    runtime/test_gpu_modern_output.cpp
    runtime/test_gpu_modern_replay.cpp
    runtime/test_gpu_modern_replay_execution_plan.cpp
    runtime/test_gpu_modern_replay_vulkan_bake.cpp
    runtime/test_gpu_modern_replay_vulkan_copy.cpp
    runtime/test_gpu_modern_replay_vulkan_diagnostics.cpp
    runtime/test_gpu_modern_replay_depth_family.cpp
    runtime/test_gpu_modern_replay_visibility_manifest.cpp
    runtime/test_gpu_modern_replay_visibility_audit.cpp
    runtime/test_gpu_modern_replay_visibility_stack.cpp
    runtime/test_gpu_modern_replay_preflight.cpp
    runtime/test_gpu_modern_replay_recorder.cpp
    runtime/test_gpu_modern_replay_submission.cpp
    runtime/test_gpu_modern_replay_capture.cpp
    runtime/test_gpu_vram.cpp
    runtime/test_spu.cpp
    runtime/test_cdrom_controller.cpp
    runtime/test_dma.cpp
    runtime/test_input.cpp
    runtime/test_host_input.cpp
    runtime/test_tas_movie.cpp
    runtime/test_tas_route.cpp
    runtime/test_mdec.cpp
    runtime/test_timers.cpp
    runtime/test_vblank_scheduler.cpp
    runtime/test_save_state.cpp
    runtime/test_save_files.cpp
    runtime/test_config.cpp
    runtime/test_gpu_screenshot.cpp
    runtime/test_game_validation.cpp
    runtime/test_overlay_manager.cpp
    runtime/test_psyq_hle.cpp
    runtime/test_psyq_registry.cpp
    runtime/test_psyq_libapi.cpp
    runtime/test_psyq_gpu.cpp
    runtime/test_psyq_cd.cpp
    runtime/test_psyq_gte.cpp
    runtime/test_psyq_font.cpp
    runtime/test_psyq_pad.cpp
    runtime/test_psyq_libc.cpp
    runtime/test_psyq_state.cpp
    runtime/test_metrics.cpp
    runtime/test_gool.cpp
    runtime/test_golden_frame.cpp
)

target_include_directories(ps1Runtime_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/ps1Runtime/include
        ${CMAKE_SOURCE_DIR}/ps1Runtime/src
)

target_link_libraries(ps1Runtime_tests
    PRIVATE
        ps1Runtime_lib
        ps1Runtime_vulkan_renderer
        GTest::gtest_main
        fmt::fmt
)

target_link_options(ps1Runtime_tests PRIVATE "-Wl,--wrap=fsync")
gtest_discover_tests(ps1Runtime_tests)
if(MGS_ABI_LIBRARY_DIR)
    target_link_libraries(ps1Runtime_tests PRIVATE
        "${MGS_ABI_LIBRARY_DIR}/libm.so.6" "${MGS_ABI_LIBRARY_DIR}/libstdc++.so.6")
    set_target_properties(ps1Runtime_tests PROPERTIES SKIP_BUILD_RPATH TRUE)
endif()
endif() # POSIX runtime / syscall-fault suite
if(WIN32)
    # Exercise the actual Windows fibers, filesystem publication and unchanged
    # save/config formats. The POSIX fault-injection suite uses fork/fsync.
    add_executable(mgsWindows_tests runtime/test_stubs.cpp runtime/test_bios.cpp
        runtime/test_bios_table_b.cpp runtime/test_memory_card_files.cpp
        runtime/test_save_state.cpp runtime/test_config.cpp runtime/test_save_files_windows.cpp)
    target_link_libraries(mgsWindows_tests PRIVATE ps1Runtime_lib GTest::gtest_main fmt::fmt)
    target_sources(mgsWindows_tests PRIVATE "${CMAKE_SOURCE_DIR}/../packaging/windows/application.rc")
    target_include_directories(mgsWindows_tests PRIVATE "${CMAKE_SOURCE_DIR}/../packaging/windows")
    gtest_discover_tests(mgsWindows_tests)
endif()

# Focused renderer iteration binary.  Keeping this independent from the game
# executable is intentional: edits to GPU/replay policy must never rebuild or
# relink the multi-megabyte generated recompiled_out.cpp just to run the
# visibility oracles.  It also avoids compiling unrelated runtime test files
# when a shared renderer header changes.
add_executable(ps1Renderer_visibility_tests EXCLUDE_FROM_ALL
    runtime/test_stubs.cpp
    runtime/test_gpu_rasterizer.cpp
    runtime/test_gpu_textures.cpp
    runtime/test_gpu_modern_output.cpp
    runtime/test_gpu_modern_replay_execution_plan.cpp
    runtime/test_gpu_modern_replay_vulkan_bake.cpp
    runtime/test_gpu_modern_replay_preflight.cpp
    runtime/test_gpu_modern_replay_submission.cpp
    runtime/test_gpu_modern_replay_capture.cpp
    runtime/test_gpu_modern_replay_vulkan_copy.cpp
    runtime/test_gpu_modern_replay_depth_family.cpp
    runtime/test_gpu_modern_replay_visibility_manifest.cpp
    runtime/test_gpu_modern_replay_visibility_audit.cpp
    runtime/test_gpu_modern_replay_visibility_stack.cpp
)

target_include_directories(ps1Renderer_visibility_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/ps1Runtime/include
        ${CMAKE_SOURCE_DIR}/ps1Runtime/src
)

target_link_libraries(ps1Renderer_visibility_tests
    PRIVATE
        ps1Runtime_lib
        ps1Runtime_vulkan_renderer
        GTest::gtest_main
        fmt::fmt
)

add_custom_target(renderer_visibility_iteration
    COMMAND $<TARGET_FILE:ps1Renderer_visibility_tests>
            --gtest_filter=GpuRasterizerTest.Hybrid*:GpuRasterizerTest.Strict*:GpuTexturesTest.Hybrid*:GpuModernOutputTest.*:GpuModernReplayExecutionPlanTest.*:GpuModernReplayVulkanBakeTest.*:GpuModernReplayPreflightTest.*:GpuModernReplaySubmissionTest.*:GpuModernReplayCaptureTest.*:GpuModernReplayVulkanCopyTest.*:GpuModernReplayDepthFamilyTest.*:GpuModernReplayVisibilityAuditTest.*:GpuModernReplayVisibilityStackTest.*-GpuModernOutputTest.OptInGpu*:GpuModernReplayVulkanCopyTest.OptInGpu*
            --gtest_brief=1
    DEPENDS ps1Renderer_visibility_tests
    USES_TERMINAL
)

add_custom_target(renderer_visibility_gpu_oracle
    COMMAND ${CMAKE_COMMAND} -E env PS1_RUN_VULKAN_INTEGRATION=1
            $<TARGET_FILE:ps1Renderer_visibility_tests>
            --gtest_filter=GpuModernOutputTest.OptInGpu*:GpuModernReplayVulkanCopyTest.OptInGpu*:GpuModernReplayVulkanDepthTest.*
            --gtest_brief=1
    DEPENDS ps1Renderer_visibility_tests
    USES_TERMINAL
)

# Phase 5 E2E Tests
add_executable(ps1Runtime_e2e_tests
    pipeline/test_pipeline.cpp
    pipeline/test_elf_loader.cpp
    pipeline/test_function_resolver.cpp
)
target_link_libraries(ps1Runtime_e2e_tests PRIVATE ps1Runtime_lib ps1Analyzer_lib ps1Recomp_lib gtest gtest_main gmock)
gtest_discover_tests(ps1Runtime_e2e_tests)
