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

# VU1 microcode compiled ahead of time by ps2_vu_aot (scripts/recomp.sh): an object library both executables
# link, so it compiles once. -O3 unrolls the operand loops over each pair's constexpr decode; debug info is of no
# use for it and triples its build time. Each file takes GCC several GB: at most 4 compile at once.
file(GLOB TMB_VU1_AOT_SOURCES CONFIGURE_DEPENDS "${TMB_GENERATED_DIR}/vu1/*.cpp")
if(TMB_VU1_AOT_SOURCES)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS tmb_vu1_aot=4)
    add_library(tmb_vu1_aot OBJECT ${TMB_VU1_AOT_SOURCES})
    target_link_libraries(tmb_vu1_aot PRIVATE ps2_runtime)
    set_target_properties(tmb_vu1_aot PROPERTIES UNITY_BUILD OFF JOB_POOL_COMPILE tmb_vu1_aot)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(tmb_vu1_aot PRIVATE -O3 -g0)
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
endif()
