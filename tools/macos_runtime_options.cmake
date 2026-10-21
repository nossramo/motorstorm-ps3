# Explicit macOS build fixes layered over the immutable source baseline.
# Loaded by CMAKE_PROJECT_MotorStormRecomp_INCLUDE, after project() enables C.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "MotorStorm's macOS runtime options require Apple Clang on macOS")
endif()

function(motorstorm_macos_runtime_options)
    # Large tentative C definitions otherwise become common symbols, which
    # Darwin's linker over-aligns to 32 KB before clamping them to 16 KB.
    # Ordinary BSS definitions preserve declared alignments without clamping.
    # Match the previous fix: runtime C only, not generated game/SPU arithmetic.
    target_compile_options(ps3recomp_runtime PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:-fno-common>")
endfunction()

# The runtime target is created later by the pinned project's add_subdirectory.
cmake_language(DEFER CALL motorstorm_macos_runtime_options)
