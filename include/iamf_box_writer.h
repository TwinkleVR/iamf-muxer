#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace immersive_audio_muxer {

class IamfBoxWriter final {
public:
    // Builds the IAMF configuration box payload. Descriptor bytes are copied
    // verbatim after the ISO BMFF IAMF configuration header.
    static bool BuildConfigurationBox(const std::vector<uint8_t>& descriptor_bytes, std::vector<uint8_t>* box, std::string* error);

    // Builds the base IAMF audio sample entry. Descriptor data is carried by the
    // iacb child box, not duplicated in the sample payload.
    static bool BuildSampleEntry(const std::vector<uint8_t>& configuration_box, std::vector<uint8_t>* sample_entry, std::string* error);

};
}  // namespace immersive_audio_muxer
