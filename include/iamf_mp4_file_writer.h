#pragma once

#include "standalone_iamf_reader.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace immersive_audio_muxer {

class IamfMp4FileWriter final {
public:
    static bool WriteIamfOnly(const std::string& output_path, const StandaloneIamfReader& iamf, std::string* error,
                              const std::function<bool()>& cancellation_requested = {});

    // Builds an IAMF trak using the physical offset of every
    // temporal-unit sample. This is used when another muxer has already
    // interleaved the samples with video/audio/data packets.
    static bool BuildIamfTrackBoxWithSampleOffsets(const StandaloneIamfReader& iamf, uint32_t track_id,
                                                   const std::vector<uint64_t>& sample_offsets,
                                                   std::vector<uint8_t>* track_box, std::string* error,
                                                   uint32_t movie_timescale = 1000);
};
}  // namespace immersive_audio_muxer
