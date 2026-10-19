#pragma once

#include <functional>
#include <string>
#include <vector>

namespace immersive_audio_muxer {

// Extracts only IAMF audio payloads from an MP4 into temporary standalone
// IAMF files and keeps tracks accepted by iamf-tools Probe. Video mdat ranges
// are never copied.
bool ExtractValidatedIamfAudioTracks(
    const std::string& mp4_path, std::vector<std::string>* iamf_paths,
    const std::function<bool()>& cancellation_requested, std::string* error);

void RemoveTemporaryIamfAudioTracks(std::vector<std::string>* iamf_paths);

}  // namespace immersive_audio_muxer
