# The interface's languages (src/platform/ui_text.h, docs/languages.md "The
# interface's languages"), PC 2026-11-16.
#
# Every build first runs tools/lang_sync (built here for the build machine:
# no Python, nothing to install): it brings lang/*.po up to date with the
# sources' tr / trf / N_ strings, checks them, writes build/languages.txt
# (what is left to do and how) and languages_todo.txt, prints a one-line
# summary, and touches build/languages.stamp when a catalogue changed.  It
# never fails the build: an untranslated string shows in English.
#
# Each lang/<code>.po is built into an executable as a resource of the type
# GT3LANG named as the code upper case (lang/pt_BR.po: PT_BR); the resources
# depend on the files and on the stamp (a catalogue the sync changed during
# this build is built in at once).  The .po files are not the sync's
# declared outputs: a "clean" must never delete them.
#
# Off Windows there are no resources: cmake/embed_languages.cmake writes the
# .po files as byte arrays into one generated source,
# <build>/languages_catalogue.cpp (target gt3_lang_catalogue, after the sync;
# src/platform/ui_text.cpp reads it), which gt3pc_languages adds to the
# executable.  The variable GT3PC_LANG_CATALOGUE_CPP names it.
set(GT3PC_LANG_CATALOGUE_CPP ${CMAKE_BINARY_DIR}/languages_catalogue.cpp)
if(NOT TARGET gt3_lang_sync)
    add_executable(gt3_lang_sync
        ${GT3PC_ROOT}/tools/lang_sync/lang_sync.cpp
        ${GT3PC_ROOT}/src/platform/overlay_font.cpp
        ${GT3PC_ROOT}/src/platform/ui_text.cpp)
    target_include_directories(gt3_lang_sync PRIVATE ${GT3PC_ROOT}/src)
    set_target_properties(gt3_lang_sync PROPERTIES OUTPUT_NAME lang_sync
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)
    # (a target's name as the command: CMake runs it through
    # CMAKE_CROSSCOMPILING_EMULATOR when cross-building - the Windows
    # toolchain's skip; the i686 Linux toolchain cross-builds too, without an
    # emulator: its executable runs on the build machine, by its path)
    if(WIN32)
        set(sync gt3_lang_sync)
    else()
        set(sync $<TARGET_FILE:gt3_lang_sync>)
    endif()
    add_custom_target(gt3_lang_sync_run ALL
        COMMAND ${sync} --root ${GT3PC_ROOT} --build ${CMAKE_BINARY_DIR}
        BYPRODUCTS ${CMAKE_BINARY_DIR}/languages.stamp ${CMAKE_BINARY_DIR}/languages.txt
                   ${CMAKE_BINARY_DIR}/languages_todo.txt
        VERBATIM)
    if(NOT WIN32)
        # (the tool links ui_text.cpp without SDL: the locale's variables
        # are read there directly)
        target_compile_definitions(gt3_lang_sync PRIVATE GT3PC_UI_TEXT_NO_SDL)
        file(GLOB pos LIST_DIRECTORIES false CONFIGURE_DEPENDS ${GT3PC_ROOT}/lang/*.po)
        add_custom_command(OUTPUT ${GT3PC_LANG_CATALOGUE_CPP}
            COMMAND ${CMAKE_COMMAND} -DOUT=${GT3PC_LANG_CATALOGUE_CPP} -DLANG_DIR=${GT3PC_ROOT}/lang
                    -P ${GT3PC_ROOT}/cmake/embed_languages.cmake
            DEPENDS ${pos} ${CMAKE_BINARY_DIR}/languages.stamp ${GT3PC_ROOT}/cmake/embed_languages.cmake
            COMMENT "Embedding lang/*.po"
            VERBATIM)
        add_custom_target(gt3_lang_catalogue ALL DEPENDS ${GT3PC_LANG_CATALOGUE_CPP})
        add_dependencies(gt3_lang_catalogue gt3_lang_sync_run)
    endif()
endif()

function(gt3pc_languages target)
    add_dependencies(${target} gt3_lang_sync_run)
    if(NOT WIN32)
        target_sources(${target} PRIVATE ${GT3PC_LANG_CATALOGUE_CPP})
        add_dependencies(${target} gt3_lang_catalogue)
        return()
    endif()
    file(GLOB pos LIST_DIRECTORIES false CONFIGURE_DEPENDS ${GT3PC_ROOT}/lang/*.po)
    if(NOT pos)
        return()
    endif()
    set(rc "")
    foreach(po ${pos})
        get_filename_component(name ${po} NAME_WE)
        string(TOUPPER ${name} name)
        string(APPEND rc "${name} GT3LANG \"${po}\"\n")
    endforeach()
    set(out ${CMAKE_CURRENT_BINARY_DIR}/${target}_languages.rc)
    file(CONFIGURE OUTPUT ${out} CONTENT "${rc}" @ONLY)
    target_sources(${target} PRIVATE ${out})
    set_property(SOURCE ${out} APPEND PROPERTY OBJECT_DEPENDS ${pos} ${CMAKE_BINARY_DIR}/languages.stamp)
endfunction()
