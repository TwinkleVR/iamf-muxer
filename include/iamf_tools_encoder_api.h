#pragma once

#include <cstddef>

extern "C" {

enum IamfToolsProbeResult {
    IAMF_TOOLS_PROBE_SUCCESS = 0,
    // The file was read successfully but does not contain a valid complete IAMF sequence.
    IAMF_TOOLS_PROBE_INVALID_IAMF = 1,
    // Probe could not determine validity because of an I/O, resource, or internal failure.
    IAMF_TOOLS_PROBE_OPERATIONAL_ERROR = 2,
};

typedef int (*IamfToolsEncoderCancelCallback)(void* opaque);
typedef void (*IamfToolsEncoderLogCallback)(int severity, const char* message,
                                             void* opaque);

struct IamfToolsEncoderRunOptions {
    const char* user_metadata_filename = nullptr;
    const char* input_wav_directory = nullptr;
    const char* output_iamf_directory = nullptr;
    IamfToolsEncoderCancelCallback cancellation_requested = nullptr;
    IamfToolsEncoderLogCallback log_callback = nullptr;
    void* opaque = nullptr;
};

int iamf_tools_encoder_run(const IamfToolsEncoderRunOptions* options, char* output_iamf_filename,
                           std::size_t output_iamf_filename_capacity, char* error_message,
                           std::size_t error_message_capacity);

}  // extern "C"
