cmake_minimum_required(VERSION 3.21)
project(TMB_Recomp LANGUAGES C CXX)

# Twisted Metal: Black (PAL, SCES-50360) static recompilation on top of PS2Recomp.
#
# Build the tools first (scripts/build_tools.sh), generate code (scripts/recomp.sh),
# then configure this project to get the `ps2EntryRunner` executable with the
# recompiled game linked in.

set(TMB_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH
    "Directory containing the output of ps2_recomp for the game ELF")

# Only the runtime is needed here; the recompiler/analyzer are built separately.
set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)

add_subdirectory(PS2Recomp)

file(GLOB TMB_GENERATED_SOURCES CONFIGURE_DEPENDS "${TMB_GENERATED_DIR}/*.cpp")
if(NOT TMB_GENERATED_SOURCES)
    message(FATAL_ERROR
        "No generated sources in ${TMB_GENERATED_DIR}. Run scripts/recomp.sh first.")
endif()

# The runtime ships a placeholder register_functions.cpp that defines the same
# function-table globals as the generated one; drop it from the runner.
get_target_property(_runner_sources ps2EntryRunner SOURCES)
list(FILTER _runner_sources EXCLUDE REGEX "src/runner/register_functions\\.cpp$")
set_property(TARGET ps2EntryRunner PROPERTY SOURCES ${_runner_sources})

file(GLOB TMB_RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/*.cpp")
# The host OS layer includes <windows.h>, which clashes with raylib names: outside the unity batches and the PCH.
set_source_files_properties("${CMAKE_SOURCE_DIR}/runtime/tmb_host.cpp" PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)

# VU1 microcode compiled ahead of time by ps2_vu_aot (scripts/recomp.sh): an object library both executables
# link, so it compiles once. -O3 unrolls the operand loops over each pair's constexpr decode; debug info is of no
# use for it and triples its build time. Each file takes GCC several GB: at most 4 compile at once.
file(GLOB TMB_VU1_AOT_SOURCES CONFIGURE_DEPENDS "${TMB_GENERATED_DIR}/vu1/*.cpp")
if(TMB_VU1_AOT_SOURCES)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS tmb_vu1_aot=4)
    # With it, microcode loops lifted to native code by hand (runtime/vu1, VU1Interpreter::registerLiftedEntry).
    file(GLOB TMB_VU1_LIFTED_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/vu1/*.cpp")
    add_library(tmb_vu1_aot OBJECT ${TMB_VU1_AOT_SOURCES} ${TMB_VU1_LIFTED_SOURCES})
    target_link_libraries(tmb_vu1_aot PRIVATE ps2_runtime)
    set_target_properties(tmb_vu1_aot PROPERTIES UNITY_BUILD OFF JOB_POOL_COMPILE tmb_vu1_aot)
    # TMB_VU1_AOT_LINES=ON: line tables only (-g1), so a profile of the replay maps samples to the jit helpers.
    option(TMB_VU1_AOT_LINES "Line tables in the compiled VU1 microcode, for profiling" OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if(TMB_VU1_AOT_LINES)
            target_compile_options(tmb_vu1_aot PRIVATE -O3 -g1)
        else()
            target_compile_options(tmb_vu1_aot PRIVATE -O3 -g0)
        endif()
    endif()
    target_sources(ps2EntryRunner PRIVATE $<TARGET_OBJECTS:tmb_vu1_aot>)
endif()

target_sources(ps2EntryRunner PRIVATE ${TMB_GENERATED_SOURCES} ${TMB_RUNTIME_SOURCES})
target_include_directories(ps2EntryRunner PRIVATE "${TMB_GENERATED_DIR}")

list(LENGTH TMB_GENERATED_SOURCES _tmb_count)
message(STATUS "TMB_Recomp: linking ${_tmb_count} generated source files from ${TMB_GENERATED_DIR}")

# The VU1 replay tool with the compiled microcode linked in: checks it against runs recorded with
# PS2X_VU1_RECORD (build/game/tmb_vu1_replay <trace> [repeat]).
if(TMB_VU1_AOT_SOURCES)
    add_executable(tmb_vu1_replay PS2Recomp/ps2xRuntime/tools/ps2_vu1_replay.cpp $<TARGET_OBJECTS:tmb_vu1_aot>)
    target_link_libraries(tmb_vu1_replay PRIVATE ps2_runtime)
    if(WIN32)
        # PS2X_PROFILE=<seconds>: the fork's sampling profiler, to see where the compiled microcode spends its time.
        target_sources(tmb_vu1_replay PRIVATE PS2Recomp/ps2xRuntime/src/profiling/sampling_profiler.cpp)
        target_link_libraries(tmb_vu1_replay PRIVATE raylib winmm) # raylib first: winmm has a PlaySound too
    endif()
endif()

# Profile-guided optimization (GCC) of the runtime, the IOP and the compiled VU1 microcode, the code the frame time
# goes to; the generated game code keeps its flags (it would all rebuild). scripts/pgo.sh: GENERATE, a training run
# into the match, then USE. The profile is specific to this build tree and machine, never committed.
set(TMB_PGO "OFF" CACHE STRING "Profile-guided optimization: OFF, GENERATE or USE (scripts/pgo.sh)")
set_property(CACHE TMB_PGO PROPERTY STRINGS OFF GENERATE USE)
set(TMB_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-profile" CACHE PATH "Where TMB_PGO=GENERATE writes the profile USE reads")
if(NOT TMB_PGO STREQUAL "OFF" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(TMB_PGO STREQUAL "GENERATE")
        set(_tmb_pgo_flags "-fprofile-generate=${TMB_PGO_DIR}" -fprofile-update=atomic)
        target_link_options(ps2EntryRunner PRIVATE "-fprofile-generate=${TMB_PGO_DIR}")
        target_sources(ps2EntryRunner PRIVATE "${CMAKE_SOURCE_DIR}/runtime/pgo/tmb_pgo_train.cpp")
        set_source_files_properties("${CMAKE_SOURCE_DIR}/runtime/pgo/tmb_pgo_train.cpp" PROPERTIES
            SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
    else()
        set(_tmb_pgo_flags "-fprofile-use=${TMB_PGO_DIR}" -fprofile-partial-training -fprofile-correction -Wno-missing-profile)
    endif()
    foreach(_tmb_pgo_target ps2_runtime ps2_iop tmb_vu1_aot)
        if(TARGET ${_tmb_pgo_target})
            target_compile_options(${_tmb_pgo_target} PRIVATE ${_tmb_pgo_flags})
        endif()
    endforeach()
    if(TMB_PGO STREQUAL "GENERATE" AND TARGET tmb_vu1_replay)
        target_link_options(tmb_vu1_replay PRIVATE "-fprofile-generate=${TMB_PGO_DIR}")
    endif()
    message(STATUS "TMB_Recomp: profile-guided optimization ${TMB_PGO} (${TMB_PGO_DIR})")
endif()
