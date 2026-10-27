#pragma once

#include "video_mapping.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace immersive_audio_muxer::app {

enum class InputKind {
    kUnknown,
    kVideoContainer,
    kStandaloneIamf,
    kAudioCandidate,
    kUnsupported,
};

enum class AudioRole {
    kUnset,
    kFirstOrderAmbisonics,
    kSecondOrderAmbisonics,
    kThirdOrderAmbisonics,
    kHeadLockedStereo,
};

enum class ChannelLayout {
    kUnset,
    kMono,
    kStereo,
    k3_1_2,
    k5_1,
    k5_1_2,
    k5_1_4,
    k7_1,
    k7_1_2,
    k7_1_4,
    k9_1_6,
};

enum class InputChannelOrderPreset {
    kUnset,
    kMonoSmpte,
    kStereoSmpte,
    k3_1_2Film,
    k3_1_2Smpte,
    k5_1Film,
    k5_1Smpte,
    k5_1_2Film,
    k5_1_2Smpte,
    k5_1_4Film,
    k5_1_4Smpte,
    k7_1Film,
    k7_1Smpte,
    k7_1Wav,
    k7_1_2Film,
    k7_1_2Smpte,
    k7_1_2Wav,
    k7_1_4Film,
    k7_1_4Smpte,
    k7_1_4Wav,
    k9_1_6DolbyAtmos,
};

// FFmpeg-independent channel identities retained by the probe layer for
// suggesting a likely input layout. The selected layout controls channel mapping.
enum class SpeakerPosition {
    kUnknown,
    kFrontLeft,
    kFrontRight,
    kFrontCenter,
    kFrontLeftOfCenter,
    kFrontRightOfCenter,
    kLowFrequency,
    kSideLeft,
    kSideRight,
    kBackLeft,
    kBackRight,
    kTopFrontLeft,
    kTopFrontRight,
    kTopMiddleLeft,
    kTopMiddleRight,
    kTopBackLeft,
    kTopBackRight,
};

enum class ProjectMode {
    kUnset,
    kMuxExistingIamf,
    kCreateIamfFromAudio,
};

enum class MediaStreamKind {
    kVideo,
    kAudio,
    kData,
    kOther,
};

struct MediaStreamInfo {
    int index = -1;
    MediaStreamKind kind = MediaStreamKind::kOther;
    std::string codec_name;
    int sample_rate = 0;
    int bit_depth = 0;
    int channel_count = 0;
    std::string channel_layout_name;
    std::vector<SpeakerPosition> channel_positions;
    bool channel_positions_complete = false;
    int width = 0;
    int height = 0;
    double frame_rate = 0.0;
    int64_t bit_rate = 0;
};

struct MediaProbeInfo {
    std::filesystem::path path;
    InputKind kind = InputKind::kUnknown;
    std::vector<MediaStreamInfo> streams;
    int64_t duration_ms = 0;
    AudioRole suggested_audio_role = AudioRole::kUnset;
    ChannelLayout suggested_channel_layout = ChannelLayout::kUnset;
    std::vector<ChannelLayout> channel_layout_candidates;
    bool channel_layout_detection_exact = false;
    std::string iamf_profile;
    std::string iamf_mix_presentation_annotation;
    uint32_t iamf_mix_presentation_count = 0;
    uint32_t iamf_audio_element_count = 0;
    std::string diagnostic;

    bool HasVideo() const;
    bool HasAudio() const;
};

struct ProjectAudioInput {
    MediaProbeInfo media;
    AudioRole role = AudioRole::kUnset;
    bool role_confirmed = false;
    ChannelLayout channel_layout = ChannelLayout::kUnset;
    InputChannelOrderPreset input_channel_order_preset = InputChannelOrderPreset::kUnset;
    bool input_channel_order_confirmed = false;
};

struct MuxProject {
    ProjectMode mode = ProjectMode::kUnset;
    std::optional<MediaProbeInfo> video;
    std::optional<MediaProbeInfo> standalone_iamf;
    std::vector<ProjectAudioInput> audio_inputs;
    bool keep_mp4_audio = false;
    bool overwrite_existing_files = true;
    VideoMappingOptions video_mapping;
    std::filesystem::path output_path;

    bool AddInput(const MediaProbeInfo& input, std::string* error);
    bool RemoveInput(const std::filesystem::path& path, std::string* error);
    bool AssignAudioRole(const std::filesystem::path& path, AudioRole role, bool confirmed, std::string* error);
    bool AssignInputChannelOrderPreset(const std::filesystem::path& path,
                                       InputChannelOrderPreset preset, bool confirmed,
                                       std::string* error);
    bool ValidateForExistingIamfMux(std::string* error) const;
    bool ValidateForAudioEncoding(std::string* error) const;
    bool ValidateForChannelBasedEncoding(std::string* error) const;
    void RecalculateMode();
};

bool IsAmbisonicsRole(AudioRole role);
int ChannelCount(ChannelLayout layout);
std::vector<ChannelLayout> ChannelLayoutsForChannelCount(int channel_count);
std::vector<InputChannelOrderPreset> InputChannelOrderPresetsForChannelCount(int channel_count);
InputChannelOrderPreset AutomaticInputChannelOrderPresetForChannelCount(int channel_count);
ChannelLayout LayoutForInputChannelOrderPreset(InputChannelOrderPreset preset);
const char* InputChannelOrderPresetName(InputChannelOrderPreset preset);
ChannelLayout DetectChannelLayout(int channel_count,
                                  const std::vector<SpeakerPosition>& positions,
                                  bool positions_complete);
bool BuildChannelOrderForIamf(InputChannelOrderPreset preset,
                              std::vector<int>* source_indices_for_output,
                              std::string* error);
const char* ToString(ChannelLayout layout);
const char* IamfLayoutName(ChannelLayout layout);
const char* IamfExpandedLayoutName(ChannelLayout layout);
std::string InputChannelOrderDescription(InputChannelOrderPreset preset);
const char* ToString(InputKind kind);
const char* ToString(AudioRole role);
const char* ToString(ProjectMode mode);

}  // namespace immersive_audio_muxer::app
