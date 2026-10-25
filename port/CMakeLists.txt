cmake_minimum_required(VERSION 3.24)
project(MotorStormRecomp
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Recompiled PS3 game project (ps3recomp)"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: this project is still sitting inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# Four sampled functions, with original bodies retained for process-level A/B.
# The pass writes only into this platform's build tree; RECOMP_DIR is immutable.
option(MOTORSTORM_PPU_LOCALS "Conservative PPU register-local pilot on iOS" OFF)
if(MOTORSTORM_PPU_LOCALS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        message(FATAL_ERROR "The PPU locals pilot is iOS-only")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(PPU_LOCALS_UNITS 075 091 092)
    foreach(unit IN LISTS PPU_LOCALS_UNITS)
        set(original "${RECOMP_DIR}/ppu_recomp_${unit}.cpp")
        set(specialized "${CMAKE_CURRENT_BINARY_DIR}/ppu_locals/ppu_recomp_${unit}.cpp")
        add_custom_command(OUTPUT "${specialized}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_ppu_locals.py"
                --unit "${unit}" --input "${original}" --output "${specialized}"
            DEPENDS "${original}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_ppu_locals.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/third_party/ps3recomp/ppu_nv_dirty.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/ppu_locals.h"
            VERBATIM)
        list(REMOVE_ITEM RECOMP_SOURCES "${original}")
        list(APPEND RECOMP_SOURCES "${specialized}")
    endforeach()
endif()

# iOS scheduling for the measured physics counting-semaphore wait. The
# immutable macOS source/builder never consumes this generated copy.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(COUNTING_WAIT_ORIGINAL "${RECOMP_DIR}/ppu_recomp_083.cpp")
    set(COUNTING_WAIT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/counting_wait/ppu_recomp_083.cpp")
    add_custom_command(OUTPUT "${COUNTING_WAIT_SOURCE}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_counting_wait.py"
            --input "${COUNTING_WAIT_ORIGINAL}" --output "${COUNTING_WAIT_SOURCE}"
        DEPENDS "${COUNTING_WAIT_ORIGINAL}" "${CMAKE_CURRENT_SOURCE_DIR}/ppu_counting_wait.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_counting_wait.py"
        VERBATIM)
    list(REMOVE_ITEM RECOMP_SOURCES "${COUNTING_WAIT_ORIGINAL}")
    list(APPEND RECOMP_SOURCES "${COUNTING_WAIT_SOURCE}")
    set_property(SOURCE "${COUNTING_WAIT_SOURCE}" APPEND PROPERTY
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR};${RECOMP_DIR}")
endif()

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# Emit the runtime's zero-initialized C globals as ordinary definitions.
# Darwin's linker over-aligns large common blocks to 32 KB and then warns
# when clamping them to a 16 KB segment. Explicit BSS definitions preserve
# each variable's declared alignment without that common-block heuristic.
if(APPLE AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(ps3recomp_runtime PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:-fno-common>")
endif()

# Detailed per-draw timing is a deliberate diagnostic build, not a normal
# gameplay measurement. Keep its clocks and atomics out of default iOS builds.
option(MOTORSTORM_RSX_PHASE_ACCOUNT "Detailed iOS per-draw phase diagnostics" OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND MOTORSTORM_RSX_PHASE_ACCOUNT)
    set_property(SOURCE "${PS3RECOMP_DIR}/libs/video/rsx_draw_engine.c"
        TARGET_DIRECTORY ps3recomp_runtime
        APPEND PROPERTY COMPILE_DEFINITIONS MOTORSTORM_RSX_PHASE_ACCOUNT=1)
endif()

# Offline RPCS3 frame comparison. This target does not link the lifted game.
if(APPLE)
    add_executable(motorstorm_rsx_replay EXCLUDE_FROM_ALL ../tools/rsx_replay.c)
    target_include_directories(motorstorm_rsx_replay PRIVATE
        "${PS3RECOMP_DIR}/libs/video")
    target_link_libraries(motorstorm_rsx_replay PRIVATE ps3recomp_runtime)
    target_link_options(motorstorm_rsx_replay PRIVATE -Wl,-dead_strip)
    set_target_properties(motorstorm_rsx_replay PROPERTIES LINKER_LANGUAGE CXX)
endif()

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    # The generator discovers handlers from library definitions. Rebuild when
    # an existing module changes, and reconfigure when a new module is added.
    file(GLOB_RECURSE HLE_NID_SOURCES CONFIGURE_DEPENDS
        "${PS3RECOMP_DIR}/libs/*.c")
    # A removed input cannot make the output older than a remaining dependency.
    # Track the list itself so deletions also remove stale handler references.
    set(HLE_NID_SOURCE_LIST "${CMAKE_CURRENT_BINARY_DIR}/hle_nid_sources.txt")
    file(GENERATE OUTPUT "${HLE_NID_SOURCE_LIST}" CONTENT "${HLE_NID_SOURCES}\n")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
                ${HLE_NID_SOURCES} "${HLE_NID_SOURCE_LIST}"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(PORT_SPU_INPUTS "" CACHE STRING
    "Input files to scan for all embedded SPU ELF programs (semicolon-separated decrypted ELF/PRX/data paths)")
set(SPU_SOURCES "")
if(PORT_ENABLE_SPU AND PORT_SPU_INPUTS)
    include("${PS3RECOMP_DIR}/cmake/PS3RecompSPU.cmake")
    ps3recomp_generate_spu("${CMAKE_CURRENT_BINARY_DIR}/generated_spu" ${PORT_SPU_INPUTS})
    list(APPEND SPU_SOURCES ${SPU_GENERATED_SOURCES}
         "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
elseif(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c wws_policy_register.c motorstorm_audio_register.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

option(MOTORSTORM_CULL_SCOPE "Exact ARM64 RTZ specialization of measured WWS culling block" OFF)
if(MOTORSTORM_CULL_SCOPE)
    if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        message(FATAL_ERROR "The culling specialization requires Apple ARM64")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(CULL_ORIGINAL "${SPU_DIR}/jobmod_be896a6374ba_e12230/spu_recomp.c")
    set(CULL_SPECIALIZED "${CMAKE_CURRENT_BINARY_DIR}/spu_cull_specialized.c")
    add_custom_command(OUTPUT "${CULL_SPECIALIZED}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_spu_culling_scope.py"
            --generate "${CULL_SPECIALIZED}"
        DEPENDS "${CULL_ORIGINAL}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_spu_culling_scope.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/spu_float_scope.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/spu_cull_bounds.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/spu_cull_profile.h"
        VERBATIM)
    list(REMOVE_ITEM SPU_SOURCES "${CULL_ORIGINAL}")
    list(APPEND SPU_SOURCES "${CULL_SPECIALIZED}")
endif()

# Exact arithmetic segments measured on captured inputs on A18 Pro. The
# runtime mask can still select the original path for gameplay comparisons.
# Existing culling specialization is retained as input.
option(MOTORSTORM_JOB_ARITHMETIC "Build measured WWS arithmetic specializations" OFF)
if(MOTORSTORM_JOB_ARITHMETIC)
    if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        message(FATAL_ERROR "Job arithmetic scopes require Apple ARM64")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    foreach(module IN ITEMS jobmod_e5d3c4d056fe_e12E10 jobmod_be896a6374ba_e12230)
        set(JOB_ORIGINAL "${SPU_DIR}/${module}/spu_recomp.c")
        if(module STREQUAL "jobmod_be896a6374ba_e12230" AND MOTORSTORM_CULL_SCOPE)
            set(JOB_ORIGINAL "${CULL_SPECIALIZED}")
        endif()
        set(JOB_SPECIALIZED "${CMAKE_CURRENT_BINARY_DIR}/${module}_scopes.c")
        add_custom_command(OUTPUT "${JOB_SPECIALIZED}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_job_arithmetic.py"
                --input "${JOB_ORIGINAL}" --generate "${JOB_SPECIALIZED}"
            DEPENDS "${JOB_ORIGINAL}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_job_arithmetic.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/spu_job_scope.h" "${CMAKE_CURRENT_SOURCE_DIR}/spu_float_scope.h"
                "${SPU_DIR}/jobmod_e5d3c4d056fe_e12E10/spu_recomp.c"
                "${SPU_DIR}/jobmod_be896a6374ba_e12230/spu_recomp.c"
            VERBATIM)
        list(REMOVE_ITEM SPU_SOURCES "${JOB_ORIGINAL}")
        list(APPEND SPU_SOURCES "${JOB_SPECIALIZED}")
    endforeach()
endif()

# Preserve the lift on disk; the generated copy adds an env-gated host wait
# at one signature-checked semaphore delay. Disabled by default at runtime.
set(SEMAPHORE_IMAGE "spu_e4633a5354beb2ec6e89ced78ef31ef3076c119b62ac8e7b0302a1f0b540e559")
set(SEMAPHORE_ORIGINAL "${SPU_DIR}/${SEMAPHORE_IMAGE}/spu_recomp.c")
if(SEMAPHORE_ORIGINAL IN_LIST SPU_SOURCES)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(SEMAPHORE_SPECIALIZED "${CMAKE_CURRENT_BINARY_DIR}/spu_semaphore_specialized.c")
    add_custom_command(OUTPUT "${SEMAPHORE_SPECIALIZED}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_spu_semaphore.py"
            --generate "${SEMAPHORE_SPECIALIZED}"
        DEPENDS "${SEMAPHORE_ORIGINAL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_spu_semaphore.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/spu_semaphore_wait.h"
        VERBATIM)
    list(REMOVE_ITEM SPU_SOURCES "${SEMAPHORE_ORIGINAL}")
    list(APPEND SPU_SOURCES "${SEMAPHORE_SPECIALIZED}")
endif()

set(AUDIO_WORKER_IMAGE "spu_1136862334bb6208f794040f3a7f26e4991cd6b00eac6229331432566f1535d1")
set(AUDIO_WORKER_ORIGINAL "${SPU_DIR}/${AUDIO_WORKER_IMAGE}/spu_recomp.c")
if(AUDIO_WORKER_ORIGINAL IN_LIST SPU_SOURCES AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(AUDIO_WORKER_SPECIALIZED "${CMAKE_CURRENT_BINARY_DIR}/spu_audio_worker.c")
    add_custom_command(OUTPUT "${AUDIO_WORKER_SPECIALIZED}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_audio_worker.py"
            --generate "${AUDIO_WORKER_SPECIALIZED}"
        DEPENDS "${AUDIO_WORKER_ORIGINAL}" "${SPU_DIR}/${AUDIO_WORKER_IMAGE}/code.bin"
            "${SPU_DIR}/${AUDIO_WORKER_IMAGE}/functions.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_audio_worker.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_audio_bounds.py"
            "${PS3RECOMP_DIR}/tools/spu_lifter.py" "${PS3RECOMP_DIR}/tools/spu_disasm.py"
        VERBATIM)
    list(REMOVE_ITEM SPU_SOURCES "${AUDIO_WORKER_ORIGINAL}")
    list(APPEND SPU_SOURCES "${AUDIO_WORKER_SPECIALIZED}")
endif()

# Native block and legacy recurrence for the verified audio DSP filter. Keep generated game code
# in the iOS build directory and retain its original loop for A/B comparisons.
option(MOTORSTORM_AUDIO_FILTER_NATIVE "Native MotorStorm audio filter variants on iOS" OFF)
if(MOTORSTORM_AUDIO_FILTER_NATIVE)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        message(FATAL_ERROR "The native audio filter is an ARM64 iOS experiment")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(AUDIO_FILTER_ORIGINAL "${SPU_DIR}/motorstorm_audio_32770/spu_recomp.c")
    set(AUDIO_FILTER_SPECIALIZED "${CMAKE_CURRENT_BINARY_DIR}/spu_audio_filter.c")
    if(NOT AUDIO_FILTER_ORIGINAL IN_LIST SPU_SOURCES)
        message(FATAL_ERROR "Generate the MotorStorm audio overlays before enabling the native filter")
    endif()
    add_custom_command(OUTPUT "${AUDIO_FILTER_SPECIALIZED}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_audio_filter.py"
            --generate "${AUDIO_FILTER_SPECIALIZED}"
        DEPENDS "${AUDIO_FILTER_ORIGINAL}" "${CMAKE_CURRENT_SOURCE_DIR}/spu_audio_filter.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/spu_audio_block.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_audio_filter.py"
        VERBATIM)
    list(REMOVE_ITEM SPU_SOURCES "${AUDIO_FILTER_ORIGINAL}")
    list(APPEND SPU_SOURCES "${AUDIO_FILTER_SPECIALIZED}")
endif()

# Fuse only the two measured WWS modules, after arithmetic specialization.
# The immutable macOS baseline never consumes these generated iOS copies.
option(MOTORSTORM_JOB_MERGE "Fuse bounded WWS blocks in the iOS runtime" OFF)
if(MOTORSTORM_JOB_MERGE)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        message(FATAL_ERROR "WWS block fusion is an iOS-only experiment")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    foreach(module IN ITEMS jobmod_e5d3c4d056fe_e12E10 jobmod_be896a6374ba_e12230)
        set(MERGE_INPUT "${SPU_DIR}/${module}/spu_recomp.c")
        if(MOTORSTORM_JOB_ARITHMETIC)
            set(MERGE_INPUT "${CMAKE_CURRENT_BINARY_DIR}/${module}_scopes.c")
        elseif(module STREQUAL "jobmod_be896a6374ba_e12230" AND MOTORSTORM_CULL_SCOPE)
            set(MERGE_INPUT "${CULL_SPECIALIZED}")
        endif()
        set(MERGE_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${module}_merged.c")
        set(MERGE_BEFORE_PIPELINE "${CMAKE_CURRENT_BINARY_DIR}/${module}_merged_baseline.c")
        add_custom_command(OUTPUT "${MERGE_OUTPUT}"
            BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/${module}_merged.json"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_job_merge.py"
                --module "${module}" --input "${MERGE_INPUT}" --generate "${MERGE_BEFORE_PIPELINE}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_spu_pipeline.py"
                --kind jobs --input "${MERGE_BEFORE_PIPELINE}" --generate "${MERGE_OUTPUT}"
            DEPENDS "${MERGE_INPUT}" "${SPU_DIR}/${module}/spu_recomp.c"
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_job_merge.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_spu_pipeline.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/spu_audio_pipeline.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/spu_job_merge.h"
            VERBATIM)
        list(REMOVE_ITEM SPU_SOURCES "${MERGE_INPUT}")
        list(APPEND SPU_SOURCES "${MERGE_OUTPUT}")
    endforeach()
endif()

option(MOTORSTORM_NATIVE_GEOMETRY "Native ARM box/plane predicates for iOS" OFF)
if(MOTORSTORM_NATIVE_GEOMETRY)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        message(FATAL_ERROR "Native geometry is an iOS-only experiment")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(GEOMETRY_MODULE "jobmod_be896a6374ba_e12230")
    set(GEOMETRY_INPUT "${SPU_DIR}/${GEOMETRY_MODULE}/spu_recomp.c")
    if(MOTORSTORM_JOB_MERGE)
        set(GEOMETRY_INPUT "${CMAKE_CURRENT_BINARY_DIR}/${GEOMETRY_MODULE}_merged.c")
    elseif(MOTORSTORM_JOB_ARITHMETIC)
        set(GEOMETRY_INPUT "${CMAKE_CURRENT_BINARY_DIR}/${GEOMETRY_MODULE}_scopes.c")
    elseif(MOTORSTORM_CULL_SCOPE)
        set(GEOMETRY_INPUT "${CULL_SPECIALIZED}")
    endif()
    if(NOT GEOMETRY_INPUT IN_LIST SPU_SOURCES)
        message(FATAL_ERROR "Native geometry input is not in the iOS source list")
    endif()
    set(GEOMETRY_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_geometry/${GEOMETRY_MODULE}.c")
    add_custom_command(OUTPUT "${GEOMETRY_OUTPUT}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_native_geometry.py"
            --input "${GEOMETRY_INPUT}" --output "${GEOMETRY_OUTPUT}"
        DEPENDS "${GEOMETRY_INPUT}" "${SPU_DIR}/${GEOMETRY_MODULE}/spu_recomp.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/native_geometry.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_native_geometry.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_job_merge.py"
        VERBATIM)
    list(REMOVE_ITEM SPU_SOURCES "${GEOMETRY_INPUT}")
    list(APPEND SPU_SOURCES "${GEOMETRY_OUTPUT}")
endif()

# Whole-batch iOS adapter; baseline remains in the same generated translation
# unit. No shared runtime sources or pinned macOS build are changed.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND MOTORSTORM_NATIVE_GEOMETRY)
    set(WWS_CHAIN_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/wws_chain/${GEOMETRY_MODULE}.c")
    add_custom_command(OUTPUT "${WWS_CHAIN_OUTPUT}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_wws_chain.py"
            --input "${GEOMETRY_OUTPUT}" --output "${WWS_CHAIN_OUTPUT}"
        DEPENDS "${GEOMETRY_OUTPUT}" "${CMAKE_CURRENT_SOURCE_DIR}/wws_native_chain.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_wws_chain.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_native_geometry.py"
        VERBATIM)
    list(REMOVE_ITEM SPU_SOURCES "${GEOMETRY_OUTPUT}")
    list(APPEND SPU_SOURCES "${WWS_CHAIN_OUTPUT}" "${CMAKE_CURRENT_SOURCE_DIR}/wws_native_chain.c")
endif()

add_library(${PROJECT_NAME} STATIC
    main.cpp
    gpu_read_sync.cpp
    job_wait.cpp
    keyboard_context.cpp
    stubs.cpp
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)

option(MOTORSTORM_JOB_BENCHMARK "Include developer-only replay of captured CPU inputs" OFF)
if(MOTORSTORM_JOB_BENCHMARK)
    if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        message(FATAL_ERROR "Job arithmetic replay requires Apple ARM64")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(JOB_TEST_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/job_arithmetic.inc")
    add_custom_command(OUTPUT "${JOB_TEST_INCLUDE}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_job_arithmetic.py"
            --test-source "${JOB_TEST_INCLUDE}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_job_arithmetic.py"
            "${SPU_DIR}/jobmod_e5d3c4d056fe_e12E10/spu_recomp.c"
            "${SPU_DIR}/jobmod_be896a6374ba_e12230/spu_recomp.c"
        VERBATIM)
    target_sources(${PROJECT_NAME} PRIVATE tests/test_job_arithmetic.c "${JOB_TEST_INCLUDE}")
    set_source_files_properties(tests/test_job_arithmetic.c PROPERTIES
        OBJECT_DEPENDS "${JOB_TEST_INCLUDE}"
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR};${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS MOTORSTORM_JOB_BENCHMARK=1
        COMPILE_OPTIONS "-O2;-UNDEBUG;-ffp-contract=off")
    set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS MOTORSTORM_HAS_JOB_BENCHMARK=1)
endif()

option(MOTORSTORM_AUDIO_BENCHMARK "Include isolated developer audio replay" OFF)
if(MOTORSTORM_AUDIO_BENCHMARK)
    if(NOT AUDIO_WORKER_SPECIALIZED)
        message(FATAL_ERROR "Audio replay requires the checked ARM64 audio worker")
    endif()
    set(AUDIO_REPLAY_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/audio_worker_benchmark.c")
    set(AUDIO_REPLAY_TEST "${CMAKE_CURRENT_SOURCE_DIR}/../tests/audio_worker_replay.c")
    add_custom_command(OUTPUT "${AUDIO_REPLAY_SOURCE}"
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/audio_worker_benchmark.h"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_audio_worker.py"
            --generate "${AUDIO_REPLAY_SOURCE}" --benchmark
        DEPENDS "${AUDIO_WORKER_SPECIALIZED}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/specialize_audio_worker.py"
        VERBATIM)
    target_sources(${PROJECT_NAME} PRIVATE "${AUDIO_REPLAY_TEST}" "${AUDIO_REPLAY_SOURCE}")
    set_source_files_properties("${AUDIO_REPLAY_TEST}" "${AUDIO_REPLAY_SOURCE}" PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR};${PS3RECOMP_DIR}/runtime/spu;${SPU_DIR}/${AUDIO_WORKER_IMAGE}"
        COMPILE_DEFINITIONS "MOTORSTORM_AUDIO_WORKER_TEST=1;MOTORSTORM_AUDIO_BENCHMARK=1"
        COMPILE_OPTIONS "-O2;-UNDEBUG;-ffp-contract=off;-include;${CMAKE_CURRENT_SOURCE_DIR}/../tests/audio_worker_namespace.h")
    set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS MOTORSTORM_HAS_AUDIO_BENCHMARK=1)
endif()

# Native-job registration has an environment gate. App launchers load that
# environment after constructors, so also initialize it at game boot. Scope
# the definition to main.cpp to avoid rebuilding every generated game unit.
if("${SPU_DIR}/jobmods_register.c" IN_LIST SPU_SOURCES)
    set_property(SOURCE main.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS MOTORSTORM_HAS_NATIVE_JOBS=1)
endif()
if(AUDIO_WORKER_SPECIALIZED)
    set_property(SOURCE main.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS MOTORSTORM_HAS_AUDIO_WORKER=1)
endif()

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- keep the SPU compiler setting explicit, independent
#                          of the chosen configuration's PPU optimization level.
#                          The last -O on the command line wins.
if(SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
    endif()
endif()

if(MOTORSTORM_JOB_MERGE)
    foreach(module IN ITEMS jobmod_e5d3c4d056fe_e12E10 jobmod_be896a6374ba_e12230)
        set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/${module}_merged.c" APPEND PROPERTY
            INCLUDE_DIRECTORIES "${SPU_DIR}/${module}")
        set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/${module}_merged.c" APPEND PROPERTY
            COMPILE_OPTIONS "-ffp-contract=off")
    endforeach()
endif()

if(MOTORSTORM_NATIVE_GEOMETRY)
    set_property(SOURCE "${GEOMETRY_OUTPUT}" APPEND PROPERTY
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR};${SPU_DIR}/${GEOMETRY_MODULE}")
    set_property(SOURCE "${GEOMETRY_OUTPUT}" APPEND PROPERTY
        COMPILE_OPTIONS "-ffp-contract=off")
endif()

if(WWS_CHAIN_OUTPUT)
    set_property(SOURCE "${WWS_CHAIN_OUTPUT}" APPEND PROPERTY
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR};${SPU_DIR}/${GEOMETRY_MODULE}")
endif()

# Only the measured audio image and its checked DSP overlays use these exact
# NEON conversions. Other workloads and the interpreter keep the reference.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    foreach(audio_image IN ITEMS
        spu_1136862334bb6208f794040f3a7f26e4991cd6b00eac6229331432566f1535d1
        motorstorm_audio_dsp motorstorm_audio_2FEF0 motorstorm_audio_32770)
        set(audio_source "${SPU_DIR}/${audio_image}/spu_recomp.c")
        if(audio_source IN_LIST SPU_SOURCES)
            set_property(SOURCE "${audio_source}" APPEND PROPERTY
                COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/spu_audio_conversions.h")
        endif()
    endforeach()
endif()

# Keep the arithmetic specialization's include paths and flags local.
if(AUDIO_FILTER_SPECIALIZED)
    set_property(SOURCE "${AUDIO_FILTER_SPECIALIZED}" APPEND PROPERTY
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR};${SPU_DIR}/motorstorm_audio_32770")
    set_property(SOURCE "${AUDIO_FILTER_SPECIALIZED}" APPEND PROPERTY
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/spu_audio_conversions.h;-ffp-contract=off")
endif()
if(MOTORSTORM_JOB_ARITHMETIC)
    foreach(module IN ITEMS jobmod_e5d3c4d056fe_e12E10 jobmod_be896a6374ba_e12230)
        set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/${module}_scopes.c" APPEND PROPERTY
            INCLUDE_DIRECTORIES "${SPU_DIR}/${module}")
        set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/${module}_scopes.c" APPEND PROPERTY
            COMPILE_OPTIONS "-ffp-contract=off")
    endforeach()
endif()
# The checked audio worker also uses shared, exact ARM64 arithmetic helpers.
if(AUDIO_WORKER_SPECIALIZED)
    set_property(SOURCE "${AUDIO_WORKER_SPECIALIZED}" APPEND PROPERTY
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR};${SPU_DIR}/${AUDIO_WORKER_IMAGE}")
endif()
# Opt-in diagnostic; normal builds retain exact SPU arithmetic.
if(MOTORSTORM_CULL_SCOPE)
    set_property(SOURCE "${CULL_SPECIALIZED}" APPEND PROPERTY
        INCLUDE_DIRECTORIES "${SPU_DIR}/jobmod_be896a6374ba_e12230")
endif()
if(SEMAPHORE_SPECIALIZED)
    set_property(SOURCE "${SEMAPHORE_SPECIALIZED}" APPEND PROPERTY
        INCLUDE_DIRECTORIES "${SPU_DIR}/${SEMAPHORE_IMAGE}")
endif()
option(MOTORSTORM_APPROX_HOT_SPU "Experiment: IEEE single precision in hot SPU job (changes rounding)" OFF)
if(MOTORSTORM_CULL_SCOPE AND MOTORSTORM_APPROX_HOT_SPU)
    message(FATAL_ERROR "Exact culling specialization and approximate SPU experiment cannot be combined")
endif()
if(MOTORSTORM_APPROX_HOT_SPU)
    if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        message(FATAL_ERROR "The hot SPU arithmetic experiment requires Apple ARM64")
    endif()
    set_property(SOURCE "${SPU_DIR}/jobmod_be896a6374ba_e12230/spu_recomp.c"
        APPEND PROPERTY COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/spu_float_approx_experiment.h")
endif()
# Measurement-only: runtime-switched hardware single precision in every SPU
# unit, to bound the gain of exact native rounding. Changes results when on.
option(MOTORSTORM_SPU_FLOAT_PROBE "Measurement: runtime-switched IEEE single precision in all SPU units" OFF)
if(MOTORSTORM_SPU_FLOAT_PROBE)
    if(MOTORSTORM_APPROX_HOT_SPU)
        message(FATAL_ERROR "The SPU float probe and hot SPU experiment cannot be combined")
    endif()
    if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        message(FATAL_ERROR "The SPU float probe requires Apple ARM64")
    endif()
    target_sources(${PROJECT_NAME} PRIVATE spu_float_probe.c)
    set_property(SOURCE ${SPU_SOURCES}
        APPEND PROPERTY COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/spu_float_probe.h")
endif()

# Exact hardware single-precision while an SPU context is on the host
# thread (FPCR round-toward-zero). Interpreter and lifted units share the
# same helpers. The approximate probe above changes results; this path does
# not. macOS keeps the option off unless a task explicitly enables it.
option(MOTORSTORM_SPU_THREAD_RTZ "Exact native RTZ SPU floats during SPU execution" OFF)
if(MOTORSTORM_SPU_THREAD_RTZ)
    if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        message(FATAL_ERROR "Thread RTZ SPU floats require Apple ARM64")
    endif()
    if(MOTORSTORM_SPU_FLOAT_PROBE OR MOTORSTORM_APPROX_HOT_SPU)
        message(FATAL_ERROR "Thread RTZ cannot be combined with approximate SPU float probes")
    endif()
    target_sources(${PROJECT_NAME} PRIVATE spu_thread_rtz.c)
    if(SPU_SOURCES)
        set_property(SOURCE ${SPU_SOURCES} APPEND PROPERTY
            COMPILE_DEFINITIONS MOTORSTORM_SPU_THREAD_RTZ)
        set_property(SOURCE ${SPU_SOURCES} APPEND PROPERTY
            COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/spu_thread_rtz.h;-ffp-contract=off")
    endif()
    set_property(SOURCE "${PS3RECOMP_DIR}/runtime/spu/spu_interp.c"
        TARGET_DIRECTORY ps3recomp_runtime
        APPEND PROPERTY COMPILE_DEFINITIONS MOTORSTORM_SPU_THREAD_RTZ)
    set_property(SOURCE "${PS3RECOMP_DIR}/runtime/spu/spu_interp.c"
        TARGET_DIRECTORY ps3recomp_runtime
        APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
    set_property(SOURCE "${PS3RECOMP_DIR}/runtime/spu/spu_interp.c"
        TARGET_DIRECTORY ps3recomp_runtime
        APPEND PROPERTY COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/spu_thread_rtz.h;-ffp-contract=off")
endif()

# Byte-identical NEON local-store loads and stores. Off unless the iOS
# builder asks for it, so the macOS app keeps the scalar accessors.
option(MOTORSTORM_SPU_LS_NEON "NEON local-store accessors in SPU code" OFF)
if(MOTORSTORM_SPU_LS_NEON)
    if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        message(FATAL_ERROR "NEON local-store accessors require Apple ARM64")
    endif()
    if(SPU_SOURCES)
        set_property(SOURCE ${SPU_SOURCES} APPEND PROPERTY
            COMPILE_DEFINITIONS SPU_LS_NEON)
    endif()
    set_property(SOURCE "${PS3RECOMP_DIR}/runtime/spu/spu_interp.c"
        TARGET_DIRECTORY ps3recomp_runtime
        APPEND PROPERTY COMPILE_DEFINITIONS SPU_LS_NEON)
endif()

# Remove observer-only LS probes from normal iOS execution. The SDK and other
# platforms retain their old defaults. Apply the same definition to generated
# SPU code and runtime helpers, without rebuilding unrelated generated PPU TUs.
option(MOTORSTORM_SPU_LS_DIAGNOSTICS "Legacy per-access SPU local-store probes" ON)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    if(MOTORSTORM_SPU_LS_DIAGNOSTICS)
        set(SPU_LS_DIAGNOSTICS_VALUE 1)
    else()
        set(SPU_LS_DIAGNOSTICS_VALUE 0)
    endif()
    if(SPU_SOURCES)
        set_property(SOURCE ${SPU_SOURCES} APPEND PROPERTY
            COMPILE_DEFINITIONS SPU_LS_DIAGNOSTICS=${SPU_LS_DIAGNOSTICS_VALUE})
    endif()
    target_compile_definitions(ps3recomp_runtime PRIVATE
        SPU_LS_DIAGNOSTICS=${SPU_LS_DIAGNOSTICS_VALUE})
endif()

# Exact ARM64 table-based SHUFB, scoped to iOS SPU units and its interpreter.
# The build-time switch keeps the hot helper free of per-instruction branches.
option(MOTORSTORM_SPU_SHUFB_NEON "Compact exact ARM64 SHUFB lowering for iOS" OFF)
if(MOTORSTORM_SPU_SHUFB_NEON)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        message(FATAL_ERROR "The compact SHUFB lowering is limited to ARM64 iOS")
    endif()
    if(SPU_SOURCES)
        set_property(SOURCE ${SPU_SOURCES} APPEND PROPERTY
            COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/spu_shufb_neon.h")
    endif()
    set_property(SOURCE "${PS3RECOMP_DIR}/runtime/spu/spu_interp.c"
        TARGET_DIRECTORY ps3recomp_runtime APPEND PROPERTY
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/spu_shufb_neon.h")
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# iOS-only experiment: optimize generated game code together with the runtime
# at the final app link. Keep per-source arithmetic/aliasing flags intact and
# leave prebuilt SDL/codec/shader dependencies as ordinary native objects.
option(MOTORSTORM_IOS_THINLTO "ThinLTO for the iOS game and runtime" OFF)
if(MOTORSTORM_IOS_THINLTO)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" OR
       NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MotorStorm ThinLTO is restricted to Clang iOS builds")
    endif()
    foreach(lto_target IN ITEMS ${PROJECT_NAME} ps3recomp_runtime)
        target_compile_options(${lto_target} PRIVATE -flto=thin)
        target_link_options(${lto_target} INTERFACE -flto=thin)
    endforeach()
endif()

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
    )
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
        # The lift spells PPC arithmetic as signed C arithmetic, which must wrap;
        # without this, clang exploits overflow UB (e.g. folds "x*x < 0" to false).
        -fwrapv
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
if(MSVC)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off")
endif()

# The post-window samples in profile-67 place VMX FMA work in these units.
# Limit the first experiment to those measured callers; leave generated
# sources and other units intact, and preserve all normal floating semantics.
option(MOTORSTORM_NEON_VMX "Exact ARM64 SIMD VMX multiply-add in sampled hot PPU units" OFF)
option(MOTORSTORM_NEON_ARITHMETIC "Experimental exact SIMD add/subtract/negative FMA in the same PPU units" OFF)
set(MOTORSTORM_NEON_VMX_UNITS "002;057;075;076;082" CACHE STRING
    "Generated PPU units selected by VMX caller profiles (three-digit IDs)")
if(MOTORSTORM_NEON_VMX)
    if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        message(FATAL_ERROR "The VMX specialization requires Apple ARM64")
    endif()
    foreach(unit IN LISTS MOTORSTORM_NEON_VMX_UNITS)
        if(NOT unit MATCHES "^[0-9][0-9][0-9]$" OR
           NOT EXISTS "${RECOMP_DIR}/ppu_recomp_${unit}.cpp")
            message(FATAL_ERROR "Invalid VMX profile unit: ${unit}")
        endif()
        set_property(SOURCE "${RECOMP_DIR}/ppu_recomp_${unit}.cpp" APPEND PROPERTY
            COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/ppu_vmx_neon.h")
        if(MOTORSTORM_NEON_ARITHMETIC)
            set_property(SOURCE "${RECOMP_DIR}/ppu_recomp_${unit}.cpp" APPEND PROPERTY
                COMPILE_DEFINITIONS MOTORSTORM_NEON_ARITHMETIC=1)
        endif()
    endforeach()
endif()

# Plain lifted guest stores inline; watched, null-page, out-of-range, raw-SPU
# and SPU-reserved stores keep the runtime's out-of-line vm_write* path.
option(MOTORSTORM_INLINE_STORES "Inline ordinary lifted PPU guest stores" OFF)
set(MOTORSTORM_INLINE_STORE_UNITS "" CACHE STRING
    "Generated PPU units selected by store caller profiles (three-digit IDs)")
if(MOTORSTORM_INLINE_STORES)
    if(NOT MOTORSTORM_INLINE_STORE_UNITS)
        message(FATAL_ERROR "MOTORSTORM_INLINE_STORES needs MOTORSTORM_INLINE_STORE_UNITS")
    endif()
    foreach(unit IN LISTS MOTORSTORM_INLINE_STORE_UNITS)
        if(NOT unit MATCHES "^[0-9][0-9][0-9]$" OR
           NOT EXISTS "${RECOMP_DIR}/ppu_recomp_${unit}.cpp")
            message(FATAL_ERROR "Invalid inline store profile unit: ${unit}")
        endif()
        set_property(SOURCE "${RECOMP_DIR}/ppu_recomp_${unit}.cpp" APPEND PROPERTY
            COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/ppu_store_inline.h")
    endforeach()
endif()

# Carry the reviewed per-unit VMX/store options onto the generated iOS copies.
# The common lifted flags above already apply through RECOMP_SOURCES.
foreach(unit IN LISTS PPU_LOCALS_UNITS)
    foreach(property COMPILE_OPTIONS COMPILE_DEFINITIONS)
        get_source_file_property(value "${RECOMP_DIR}/ppu_recomp_${unit}.cpp" ${property})
        if(value)
            set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/ppu_locals/ppu_recomp_${unit}.cpp"
                APPEND PROPERTY ${property} "${value}")
        endif()
    endforeach()
endforeach()

if(COUNTING_WAIT_SOURCE)
    foreach(property COMPILE_OPTIONS COMPILE_DEFINITIONS)
        get_source_file_property(value "${COUNTING_WAIT_ORIGINAL}" ${property})
        if(value)
            set_property(SOURCE "${COUNTING_WAIT_SOURCE}" APPEND PROPERTY ${property} "${value}")
        endif()
    endforeach()
endif()

option(MOTORSTORM_VMX_BENCHMARK "Include isolated developer VMX equivalence/CPU test" OFF)
if(MOTORSTORM_VMX_BENCHMARK)
    if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        message(FATAL_ERROR "VMX benchmark requires Apple ARM64")
    endif()
    target_sources(${PROJECT_NAME} PRIVATE tests/test_ppu_vmx_neon.c)
    set_source_files_properties(tests/test_ppu_vmx_neon.c PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR};${PS3RECOMP_DIR}/runtime/ppu"
        COMPILE_DEFINITIONS MOTORSTORM_VMX_BENCHMARK=1
        COMPILE_OPTIONS "-O2;-UNDEBUG;-ffp-contract=off")
    set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS MOTORSTORM_HAS_VMX_BENCHMARK=1)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
# Keep the standalone runner and Xcode app on the same compiled game objects.
# Whole-archive is required: generated SPU registries install themselves from
# constructors, so ordinary archive extraction would silently omit them.
add_executable(motorstorm_runner cli_main.cpp)
target_link_libraries(motorstorm_runner PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,${PROJECT_NAME}>")
set_target_properties(motorstorm_runner PROPERTIES OUTPUT_NAME MotorStormRecomp)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # Only a link-dependency description for Xcode on iOS; UIKit owns main.
    set_target_properties(motorstorm_runner PROPERTIES EXCLUDE_FROM_ALL TRUE)
else()
    install(TARGETS motorstorm_runner RUNTIME DESTINATION bin)
endif()

if(PS3RECOMP_BUILD_TESTS AND APPLE AND SEMAPHORE_SPECIALIZED)
    enable_testing()
    add_test(NAME motorstorm_spu_semaphore_wait
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_spu_semaphore_wait.py")
endif()
