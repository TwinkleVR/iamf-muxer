#pragma once

#include "iamf_tools_encoder_api.h"

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define IAMF_TOOLS_PROBE_IMPORT __declspec(dllimport)
#elif defined(__GNUC__)
#define IAMF_TOOLS_PROBE_IMPORT __attribute__((visibility("default")))
#else
#define IAMF_TOOLS_PROBE_IMPORT
#endif

extern "C" {

// Returns IamfToolsProbeResult. Invalid embedded IAMF is distinguishable from
// an operational failure while reading or probing the extracted track.
IAMF_TOOLS_PROBE_IMPORT int iamf_tools_probe_file(
    const char* iamf_filename, std::size_t expected_descriptor_bytes,
    std::uint32_t* temporal_unit_count, char* error_message,
    std::size_t error_message_capacity);

}
