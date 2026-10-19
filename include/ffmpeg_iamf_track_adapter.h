#pragma once

#include "standalone_iamf_reader.h"
#include "video_mapping.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace immersive_audio_muxer {

struct FfmpegIamfPlaceholderTrack {
    uint32_t track_id = 0;
    const StandaloneIamfReader* reader = nullptr;
};

struct Mp4TimecodeTrackFlags {
    uint32_t track_id = 0;
    uint32_t flags = 0;
};

// Replaces FFmpeg-written placeholder audio tracks with standards-compliant
// IAMF tracks after the unified packet scheduler has written their samples.
bool ReplaceFfmpegPlaceholderTracksWithIamf(
    const std::string& path,
    const std::vector<FfmpegIamfPlaceholderTrack>& tracks,
    std::string* error,
    const std::function<bool()>& cancellation_requested = {});

// Applies the requested spherical/stereo metadata to the video sample entry.
// The rewritten moov remains inside the front reservation made by the MP4 muxer.
bool PatchMp4VideoSpatialMetadata(const std::string& path,
                                  uint32_t target_track_id,
                                  const VideoSpatialMetadata& metadata,
                                  std::string* error);

// Restores the timecode flags that movenc does not copy from a stream-derived
// tmcd sample entry. Only the bounded moov metadata is read and patched.
bool PatchMp4TimecodeTrackFlags(const std::string& path,
                               const std::vector<Mp4TimecodeTrackFlags>& tracks,
                               std::string* error);

}  // namespace immersive_audio_muxer
