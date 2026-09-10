# Wraps the initializer list produced by "glslc -mfmt=c" into a small C++ header
# so the SPIR-V binary can be compiled straight into the native library.
#
# Run as: cmake -DSPV_INPUT=<file> -DSPV_OUTPUT=<file> -DSPV_ARRAY=<name> -P embed_spv.cmake
file(READ "${SPV_INPUT}" SPV_BODY)

file(WRITE "${SPV_OUTPUT}"
"// Generated from ${SPV_INPUT} by embed_spv.cmake - do not edit.
#pragma once
#include <cstddef>
#include <cstdint>
static const uint32_t ${SPV_ARRAY}[] = ${SPV_BODY};
static const size_t ${SPV_ARRAY}Words = sizeof(${SPV_ARRAY}) / sizeof(${SPV_ARRAY}[0]);
")
