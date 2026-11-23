# GT3 PC: the game, the engine, the platform layer and the executable.

# Options shared by the decompiled code (game and kept engine files):
# /FI: MSVC spellings of the decomp's GNU C++ and the C library mapping
# (include/pc_prelude.h); /permissive: pre-standard C++ in places (the egcs
# dialect); /bigobj: a few translation units are large;
# /Zc:threadSafeInit-: function-local statics without the C++11 guard (its
# lock would deadlock when the game scheduler switches threads inside a
# static's constructor; one game thread runs at a time, as on the EE).
# GCC / Clang (cmake/toolchains/linux-i686.cmake): the same prelude by
# -include; -w as /W0; -fno-threadsafe-statics as /Zc:threadSafeInit-;
# the egcs code's assumptions of overflow, aliasing and null checks kept
# (-fwrapv -fno-strict-aliasing -fno-delete-null-pointer-checks); SSE2 scalar
# floating point without contraction (-msse2 -mfpmath=sse -ffp-contract=off):
# i686 code otherwise computes on the x87 stack, with extended precision, and
# the physics drifts from the MSVC build and the PS2; -fpermissive (GCC only)
# for the pre-standard C++; GNU C++17 (CXX_EXTENSIONS on the targets).
if(MSVC)
    set(GT3_DECOMP_OPTIONS /FI${GT3PC_ROOT}/include/pc_prelude.h /permissive /bigobj /EHsc /W0 /Zc:threadSafeInit-)
else()
    set(GT3_DECOMP_OPTIONS
        -include ${GT3PC_ROOT}/include/pc_prelude.h
        -w -fno-threadsafe-statics -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks
        -msse2 -mfpmath=sse -ffp-contract=off -march=pentium4
        $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-fpermissive>)
endif()

# ---- the game: the decompiled C++ of src/game --------------------------------
file(GLOB_RECURSE GT3_GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/game/*.cpp)
# Files still being ported (VU0 / MMI inline assembly): none left (the
# rewritten asm: docs/asm.md).
set(GT3_GAME_PENDING
)
foreach(f ${GT3_GAME_PENDING})
    list(REMOVE_ITEM GT3_GAME_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/game/${f})
endforeach()

add_library(gt3_game OBJECT ${GT3_GAME_SOURCES})
target_include_directories(gt3_game PUBLIC ${GT3PC_ROOT}/include ${GT3PC_ROOT}/src)
target_compile_options(gt3_game PRIVATE ${GT3_DECOMP_OPTIONS})
target_compile_definitions(gt3_game PRIVATE GT3_PC=1)

# ---- the engine: Polyphony's libraries, kept where portable --------------------
# The portable C++ of src/engine compiles as the game does; the hardware
# parts (graphics, IOP clients, movies, sound output, memory card, pads) are
# the native replacements under src/engine/*/native and src/platform.
set(GT3_ENGINE_SOURCES
    engine/pdistd/T432_bignum.cpp
    engine/pdistd/T433_safer.cpp
    engine/pdistd/T471_unit_arena.cpp
    engine/pdistd/T497_hash.cpp
    engine/pdistd/T530_allocator.cpp
    engine/pdistd/T530a_printf_tables.cpp
    engine/pdistd/T531_random.cpp
    engine/pdistd/T532_heap.cpp
    engine/pdistd/T533_printf.cpp
    engine/pdistd/T534_sprintf.cpp
    engine/pdistd/T535_vsprintf.cpp
    engine/pdistd/T536_bit_stream_buffer.cpp
    engine/pdistd/T537_gzip_inflate.cpp
    engine/pdistd/T538_fmod_wrap.cpp
    engine/pdistd/T539_print_format.cpp
    engine/io/T500_vol_toc.cpp
    engine/io/T501_memorycard_manager.cpp
    engine/io/T502_file_resolve.cpp
    engine/io/T503_vol_thread.cpp
    engine/io/T504_file_queue.cpp
    engine/io/T505_host_files.cpp
    engine/io/T506_image_pack.cpp
    engine/io/T507_async_loader.cpp
    engine/io/T508_loader_thread.cpp
    engine/io/T509_serialize.cpp
    engine/io/T510_vol_device.cpp
    engine/io/T511_inflate_loader.cpp
    engine/io/T525_sector_cache.cpp
    engine/io/T526_iso9660.cpp
    engine/text/T496_font.cpp
    engine/text/T498_font_print.cpp
    engine/text/T499_font_metrics.cpp
    engine/text/T517_realtime_text.cpp
    engine/gfx/T451_model_set.cpp
    engine/gfx/T452_model_anim.cpp
    engine/gfx/T454_sprite_model.cpp
    engine/gfx/T512_render_context.cpp
    engine/gfx/T513_vram_allocator.cpp
    engine/gfx/T514_gs_buffer.cpp
    engine/gfx/T515_tex_set.cpp
    engine/gfx/pc_model_walk.cpp
    engine/input/T472_analog_curve.cpp
    engine/input/T474_control.cpp
    engine/input/T475_pad_manager.cpp
    engine/input/T476_port_base.cpp
    engine/input/T477_ps_port.cpp
    engine/input/T478_steering_ps_port.cpp
    engine/input/T479_usb_steering_port.cpp
    engine/input/T480_ieee1394_port.cpp
    engine/media/T529_mpegif.cpp
    # the G002 player's interface and pdimpeg over the native movie code
    # (gt3_media below; docs/movies.md)
    engine/media/mpeg_player_pc.cpp
    engine/pdi/T522_pdimpeg.cpp
    engine/system/T473_link_session.cpp
    engine/system/thread_util.cpp
    engine/system/scf_lock.cpp
    engine/io/hostfs_io.cpp
    engine/pdi/pdistd_misc.cpp
    engine/pdi/T519_pdicdvd.cpp
    engine/pdi/T523_pdipipe.cpp
    engine/pdi/T524_iop_load_module_base.cpp
    engine/pdi/T541_pdi1394.cpp
    engine/pdi/pdi1394_rpc.cpp
    engine/input/usb_pc.cpp
    engine/io/mcard_io.cpp
    engine/pdistd/fastmath.cpp
    engine/io/file_missing.cpp
    # the sound: the EE voice manager / sequencer (decomp) and the IOP clients
    # rewritten over the native SPU2 (engine/sound/native, docs/sound.md)
    engine/sound/sound_driver.cpp
    engine/sound/sound_seq.cpp
    engine/pdi/T520_pdispu2.cpp
    engine/pdi/T521_pdibgm.cpp
    # pgl, the graphics library: the decomp's C with its packet writing
    # replaced by recordings for the native renderer (gt3_render below;
    # docs/pgl.md, docs/renderer.md)
    engine/pgl/pgl_driver.cpp
    engine/pgl/pgl_core.cpp
    engine/pgl/pgl_vu0.cpp
    engine/pgl/pgl_lighting.cpp
    engine/pgl/pgl_prim.cpp
    engine/pgl/pgl_tex1.cpp
    engine/pgl/pgl_font.cpp
    engine/pgl/pgl_flare.cpp
    engine/pgl/pgl_particles.cpp
    engine/pgl/pgl_color_filter.cpp
)
add_library(gt3_engine OBJECT ${GT3_ENGINE_SOURCES})
target_include_directories(gt3_engine PUBLIC ${GT3PC_ROOT}/include ${GT3PC_ROOT}/src)
target_compile_options(gt3_engine PRIVATE ${GT3_DECOMP_OPTIONS})
target_compile_definitions(gt3_engine PRIVATE GT3_PC=1)

if(NOT MSVC)
    # GCC / Clang: the decompiled code (gnu++17) and the platform layer's
    # files that are ported (the window and the sound output on SDL3); the
    # controllers, Direct3D (the renderer and the panels drawn with it), the
    # movies and the executables are still Windows-only.
    set_target_properties(gt3_game gt3_engine PROPERTIES CXX_EXTENSIONS ON)
    # The interface's languages: tools/lang_sync and the catalogues' generated
    # source (cmake/languages.cmake), built already.
    include(${GT3PC_ROOT}/cmake/languages.cmake)

    # ---- the platform layer off Windows: the portable files and the POSIX
    # backends of platform/posix and platform/os.  SSE2 scalar floating point
    # (-msse2 -mfpmath=sse) as the MSVC x86 build has by default: i686 code
    # otherwise computes on the x87 stack, with extended precision and the
    # x87 control word's rounding, and the values it hands the game (the
    # settings, the input curves, the timing) would differ from Windows'.
    set(GT3_PLATFORM_FLOAT_OPTIONS -msse2 -mfpmath=sse)
    add_library(gt3_platform STATIC
        # the files and folders (UTF-8 paths, names matched without case),
        # the INI calls and the EUC-JP code page of the Windows API
        platform/os/fs_posix.cpp
        platform/os/ini_posix.cpp
        platform/os/eucjp.cpp
        # the log (and the standard error), the program's entry, the crash
        # report, the heap's arena, the memory line, the clock
        platform/posix/log_posix.cpp
        platform/posix/run_posix.cpp
        platform/posix/crash_posix.cpp
        platform/posix/heap_arena_posix.cpp
        platform/posix/memstat_posix.cpp
        platform/posix/pc_time_posix.cpp
        platform/posix/sample_prof_posix.cpp
        platform/posix/file_alert_posix.cpp
        platform/pc_libc.cpp
        platform/paths.cpp
        platform/settings.cpp
        platform/language.cpp
        platform/scheduler.cpp
        platform/ee_kernel.cpp
        platform/disc.cpp
        platform/host_file.cpp
        platform/disc_image.cpp
        launcher/iso9660.cpp
        launcher/volume.cpp
        launcher/inflate.cpp
        launcher/util.cpp
        platform/input_map.cpp
        platform/wheel.cpp
        platform/clutch_input.cpp
        platform/debug_camera.cpp
        platform/memcard.cpp
        platform/save_folders.cpp
        platform/frame_step.cpp
        platform/overlay_font.cpp
        platform/osd.cpp
        platform/hotkey_hint.cpp
        platform/challenge_osd.cpp
        platform/gcf_tables.cpp
        platform/param_db.cpp
        platform/param_schema.cpp
        platform/sqlite_file.cpp
        platform/play_time.cpp
        platform/png_image.cpp
        platform/ui_text.cpp
        # the game window, its keyboard and mouse (SDL3; the icon gt3pc.ico
        # built in, as the Windows resource has it)
        platform/sdl/window_sdl.cpp
        # the race's song panel (its text folded without the Windows calls)
        platform/song_osd.cpp
        # the sound output: SDL3 in pull mode through the output stage
        # shared with Windows, and the .mp3 decoder of the BGM (dr_mp3)
        platform/audio_stage.cpp
        platform/sdl/audio_sdl.cpp
        ${GT3PC_ROOT}/third_party_src/dr_mp3/dr_mp3.c
    )
    set_source_files_properties(platform/sdl/window_sdl.cpp PROPERTIES
        COMPILE_DEFINITIONS GT3PC_WINDOW_ICON="${GT3PC_ROOT}/gt3pc.ico"
        OBJECT_DEPENDS ${GT3PC_ROOT}/gt3pc.ico)
    target_include_directories(gt3_platform PUBLIC ${GT3PC_ROOT}/include ${GT3PC_ROOT}/src)
    target_compile_options(gt3_platform PRIVATE ${GT3_PLATFORM_FLOAT_OPTIONS})
    set_target_properties(gt3_platform PROPERTIES CXX_EXTENSIONS ON)
    # SDL3 (the locale, the message box, the window; the controllers later):
    # a system package, else the i686 build in ~/opt/i386
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        find_package(SDL3 CONFIG REQUIRED PATHS $ENV{HOME}/opt/i386 NO_DEFAULT_PATH)
    endif()
    include(${GT3PC_ROOT}/cmake/sqlite.cmake)
    include(${GT3PC_ROOT}/cmake/lodepng.cmake)
    find_package(Threads REQUIRED)
    target_link_libraries(gt3_platform PUBLIC gt3_sqlite gt3_lodepng SDL3::SDL3 Threads::Threads ${CMAKE_DL_LIBS})
    # the native SPU2 / BGM player (standard C++, its own options), with the
    # platform layer's SSE2 floating point
    add_subdirectory(engine/sound/native)
    target_compile_options(gt3_sound_native PRIVATE ${GT3_PLATFORM_FLOAT_OPTIONS})
    target_link_libraries(gt3_platform PUBLIC gt3_sound_native)
    return()
endif()

# ---- the platform layer -------------------------------------------------------
add_library(gt3_platform STATIC
    # the files and folders (UTF-8 paths) through the Win32 calls
    platform/os/fs_win.cpp
    platform/log.cpp
    platform/pc_libc.cpp
    platform/paths.cpp
    platform/settings.cpp
    platform/language.cpp
    platform/scheduler.cpp
    platform/ee_kernel.cpp
    platform/window.cpp
    platform/disc.cpp
    platform/host_file.cpp
    # the game files read straight from a disc image (the launcher's Play from
    # disc...): the launcher's own readers of the image and its volume
    platform/disc_image.cpp
    launcher/iso9660.cpp
    launcher/volume.cpp
    launcher/inflate.cpp
    launcher/util.cpp
    platform/file_alert.cpp
    platform/pc_time.cpp
    platform/pad_pc.cpp
    platform/gamepad.cpp
    platform/input_map.cpp
    platform/wheel.cpp
    platform/clutch_input.cpp
    platform/debug_camera.cpp
    platform/memcard.cpp
    platform/save_folders.cpp
    platform/heap_arena.cpp
    platform/memstat.cpp
    platform/gpu.cpp
    platform/frame_monitor.cpp
    platform/frame_step.cpp
    platform/overlay.cpp
    platform/overlay_font.cpp
    platform/osd.cpp
    platform/hotkey_hint.cpp
    platform/song_osd.cpp
    platform/cockpit_osd.cpp
    platform/challenge_osd.cpp
    platform/texture_menu.cpp
    platform/sample_prof.cpp
    platform/settings_menu.cpp
    platform/system_menu.cpp
    platform/audio.cpp
    platform/audio_stage.cpp
    platform/audio_ks.cpp
    platform/crash.cpp
    platform/gcf_tables.cpp
    platform/param_db.cpp
    platform/param_schema.cpp
    platform/sqlite_file.cpp
    platform/play_time.cpp
    platform/png_image.cpp
    platform/screenshot.cpp
    # PC (2026-11-16): the interface's languages (the catalogues: resources of
    # each executable, cmake/languages.cmake)
    platform/ui_text.cpp
)
target_include_directories(gt3_platform PUBLIC ${GT3PC_ROOT}/include ${GT3PC_ROOT}/src)
target_link_libraries(gt3_platform PUBLIC winmm d3d11 dxgi d3dcompiler dbghelp windowscodecs)

# ---- the controllers: SDL3, delay-loaded (cmake/sdl3.cmake) -----------------------
include(${GT3PC_ROOT}/cmake/sdl3.cmake)
target_link_libraries(gt3_platform PUBLIC gt3_sdl3)

# ---- the game configs' and the database folder's tables: SQLite, a static
# library (cmake/sqlite.cmake) ----------------------------------------------------
include(${GT3PC_ROOT}/cmake/sqlite.cmake)
target_link_libraries(gt3_platform PUBLIC gt3_sqlite)

# ---- the sound: the native SPU2 / BGM player (standard C++, its own options:
# not the decomp's prelude, guarded statics) and the output above (kernel
# streaming, WASAPI) ------------------------------------------------------------
add_subdirectory(engine/sound/native)
target_link_libraries(gt3_platform PUBLIC gt3_sound_native ole32 avrt setupapi ksuser)

# ---- the movies: the native MPEG-2 / IPU decoder and the movie services
# (standard C++, not the decomp's prelude; docs/movies.md) ------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/engine/media/native/sources.cmake)
add_library(gt3_media STATIC ${GT3_MEDIA_NATIVE_SOURCES} ${GT3_MEDIA_SERVICE_SOURCES})
target_include_directories(gt3_media PUBLIC ${GT3PC_ROOT}/src)
target_link_libraries(gt3_media PUBLIC gt3_platform gt3_sound_native)

# ---- the renderer: pgl's native side on Direct3D 11 - the command lists, the
# GS state executor, render targets, the VRAM registry and texture decoding
# (gstex), the VU1 model decoder (gt3vif), pgl_math (standard C++, not the
# decomp's prelude; docs/renderer.md) ------------------------------------------------
add_library(gt3_render STATIC
    engine/pgl/native/pgl_render.cpp
    engine/pgl/native/pgl_exec.cpp
    engine/pgl/native/pgl_targets.cpp
    engine/pgl/native/pgl_textures.cpp
    engine/pgl/native/pgl_host_image.cpp
    engine/pgl/native/pgl_movie.cpp
    engine/pgl/native/pgl_vfont.cpp
    engine/pgl/native/pgl_flicker.cpp
    engine/pgl/native/pgl_capture.cpp
    engine/pgl/native/pgl_models.cpp
    engine/pgl/native/pgl_d3d.cpp
    engine/pgl/native/pgl_queue.cpp
    engine/pgl/native/pgl_prewarm.cpp
    engine/pgl/native/pgl_texrepl.cpp
    engine/pgl/native/pgl_math.cpp
    engine/pgl/native/pgl_state_gs.cpp
    engine/pgl/native/pgl_state_prim.cpp
    engine/pgl/native/gs_tex1.cpp
    engine/pgl/native/gs_tex_decode.cpp
    engine/pgl/native/gs_tex_format.cpp
    engine/pgl/native/gs_tex_layout.cpp
    engine/pgl/native/gs_tex_space.cpp
    engine/pgl/native/gs_imgs.cpp
    engine/pgl/native/vif_model.cpp
)
target_include_directories(gt3_render PUBLIC ${GT3PC_ROOT}/src)
target_link_libraries(gt3_render PUBLIC gt3_platform dbghelp dwrite)

# ---- the executable -------------------------------------------------------------
add_executable(gt3pc WIN32
    main/main.cpp
    main/static_inits.cpp
    main/gt3pc.rc
    $<TARGET_OBJECTS:gt3_game>
    $<TARGET_OBJECTS:gt3_engine>
)
target_include_directories(gt3pc PRIVATE ${GT3PC_ROOT}/include ${GT3PC_ROOT}/src ${CMAKE_CURRENT_SOURCE_DIR}/main)
target_link_libraries(gt3pc PRIVATE gt3_platform gt3_media gt3_render)
target_link_options(gt3pc PRIVATE /LARGEADDRESSAWARE)
# PC (2026-11-16): the interface's languages, lang/*.po (platform/ui_text.h)
include(${GT3PC_ROOT}/cmake/languages.cmake)
gt3pc_languages(gt3pc)
add_custom_command(TARGET gt3pc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GT3PC_SDL3_DLL} $<TARGET_FILE_DIR:gt3pc>)
add_subdirectory(launcher)
