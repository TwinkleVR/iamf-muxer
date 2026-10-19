#include "app/project_types.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <iterator>
#include <utility>

namespace immersive_audio_muxer::app {
namespace {

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    if (left.empty() || right.empty())
        return false;
    std::error_code left_error;
    std::error_code right_error;
    std::filesystem::path normalized_left = std::filesystem::absolute(left, left_error).lexically_normal();
    std::filesystem::path normalized_right = std::filesystem::absolute(right, right_error).lexically_normal();
    if (left_error)
        normalized_left = left.lexically_normal();
    if (right_error)
        normalized_right = right.lexically_normal();
    std::string left_key = normalized_left.u8string();
    std::string right_key = normalized_right.u8string();
#if defined(_WIN32)
    std::transform(left_key.begin(), left_key.end(), left_key.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    std::transform(right_key.begin(), right_key.end(), right_key.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
#endif
    return left_key == right_key;
}

void SetError(std::string* error, const std::string& message) {
    if (error)
        *error = message;
}

using Position = SpeakerPosition;

bool SamePositions(const std::vector<Position>& actual,
                   std::initializer_list<Position> expected) {
    if (actual.size() != expected.size())
        return false;
    std::vector<Position> sorted_actual = actual;
    std::vector<Position> sorted_expected(expected);
    std::sort(sorted_actual.begin(), sorted_actual.end());
    std::sort(sorted_expected.begin(), sorted_expected.end());
    return sorted_actual == sorted_expected;
}

bool HasFrontAndLfe(const std::vector<Position>& positions) {
    return std::find(positions.begin(), positions.end(), Position::kFrontLeft) != positions.end() &&
           std::find(positions.begin(), positions.end(), Position::kFrontCenter) != positions.end() &&
           std::find(positions.begin(), positions.end(), Position::kFrontRight) != positions.end() &&
           std::find(positions.begin(), positions.end(), Position::kLowFrequency) != positions.end();
}

bool HasPair(const std::vector<Position>& positions, Position left, Position right) {
    return std::find(positions.begin(), positions.end(), left) != positions.end() &&
           std::find(positions.begin(), positions.end(), right) != positions.end();
}

bool MatchesSurroundPair(const std::vector<Position>& positions) {
    return HasPair(positions, Position::kSideLeft, Position::kSideRight) ||
           HasPair(positions, Position::kBackLeft, Position::kBackRight);
}

bool MatchesLayout(ChannelLayout layout, const std::vector<Position>& positions) {
    switch (layout) {
        case ChannelLayout::kMono:
            return SamePositions(positions, {Position::kFrontCenter});
        case ChannelLayout::kStereo:
            return SamePositions(positions, {Position::kFrontLeft, Position::kFrontRight});
        case ChannelLayout::k3_1_2:
            return SamePositions(positions, {Position::kFrontLeft, Position::kFrontCenter,
                                              Position::kFrontRight, Position::kTopFrontLeft,
                                              Position::kTopFrontRight, Position::kLowFrequency});
        case ChannelLayout::k5_1:
            return HasFrontAndLfe(positions) && positions.size() == 6 && MatchesSurroundPair(positions);
        case ChannelLayout::k5_1_2:
            return HasFrontAndLfe(positions) && positions.size() == 8 && MatchesSurroundPair(positions) &&
                   HasPair(positions, Position::kTopFrontLeft, Position::kTopFrontRight);
        case ChannelLayout::k5_1_4:
            return HasFrontAndLfe(positions) && positions.size() == 10 && MatchesSurroundPair(positions) &&
                   HasPair(positions, Position::kTopFrontLeft, Position::kTopFrontRight) &&
                   HasPair(positions, Position::kTopBackLeft, Position::kTopBackRight);
        case ChannelLayout::k7_1:
            return HasFrontAndLfe(positions) && positions.size() == 8 &&
                   HasPair(positions, Position::kSideLeft, Position::kSideRight) &&
                   HasPair(positions, Position::kBackLeft, Position::kBackRight);
        case ChannelLayout::k7_1_2:
            return HasFrontAndLfe(positions) && positions.size() == 10 &&
                   HasPair(positions, Position::kSideLeft, Position::kSideRight) &&
                   HasPair(positions, Position::kBackLeft, Position::kBackRight) &&
                   (HasPair(positions, Position::kTopFrontLeft, Position::kTopFrontRight) ||
                    HasPair(positions, Position::kTopMiddleLeft, Position::kTopMiddleRight));
        case ChannelLayout::k7_1_4:
            return HasFrontAndLfe(positions) && positions.size() == 12 &&
                   HasPair(positions, Position::kSideLeft, Position::kSideRight) &&
                   HasPair(positions, Position::kBackLeft, Position::kBackRight) &&
                   HasPair(positions, Position::kTopFrontLeft, Position::kTopFrontRight) &&
                   HasPair(positions, Position::kTopBackLeft, Position::kTopBackRight);
        case ChannelLayout::kUnset:
            return false;
    }
    return false;
}

int FindPosition(const std::vector<Position>& positions, Position wanted) {
    const auto it = std::find(positions.begin(), positions.end(), wanted);
    return it == positions.end() ? -1 : static_cast<int>(std::distance(positions.begin(), it));
}

std::vector<Position> TargetPositions(ChannelLayout layout) {
    switch (layout) {
        case ChannelLayout::kMono:
            return {Position::kFrontCenter};
        case ChannelLayout::kStereo:
            return {Position::kFrontLeft, Position::kFrontRight};
        case ChannelLayout::k3_1_2:
            return {Position::kFrontLeft, Position::kFrontRight, Position::kFrontCenter,
                    Position::kLowFrequency, Position::kTopFrontLeft, Position::kTopFrontRight};
        case ChannelLayout::k5_1:
            return {Position::kFrontLeft, Position::kFrontRight, Position::kFrontCenter,
                    Position::kLowFrequency, Position::kSideLeft, Position::kSideRight};
        case ChannelLayout::k5_1_2:
            return {Position::kFrontLeft, Position::kFrontRight, Position::kFrontCenter,
                    Position::kLowFrequency, Position::kSideLeft, Position::kSideRight,
                    Position::kTopFrontLeft, Position::kTopFrontRight};
        case ChannelLayout::k5_1_4:
            return {Position::kFrontLeft, Position::kFrontRight, Position::kFrontCenter,
                    Position::kLowFrequency, Position::kSideLeft, Position::kSideRight,
                    Position::kTopFrontLeft, Position::kTopFrontRight,
                    Position::kTopBackLeft, Position::kTopBackRight};
        case ChannelLayout::k7_1:
            return {Position::kFrontLeft, Position::kFrontRight, Position::kFrontCenter,
                    Position::kLowFrequency, Position::kSideLeft, Position::kSideRight,
                    Position::kBackLeft, Position::kBackRight};
        case ChannelLayout::k7_1_2:
            return {Position::kFrontLeft, Position::kFrontRight, Position::kFrontCenter,
                    Position::kLowFrequency, Position::kSideLeft, Position::kSideRight,
                    Position::kBackLeft, Position::kBackRight,
                    Position::kTopFrontLeft, Position::kTopFrontRight};
        case ChannelLayout::k7_1_4:
            return {Position::kFrontLeft, Position::kFrontRight, Position::kFrontCenter,
                    Position::kLowFrequency, Position::kSideLeft, Position::kSideRight,
                    Position::kBackLeft, Position::kBackRight,
                    Position::kTopFrontLeft, Position::kTopFrontRight,
                    Position::kTopBackLeft, Position::kTopBackRight};
        case ChannelLayout::kUnset:
            return {};
    }
    return {};
}

std::vector<Position> InputPositions(ChannelLayout layout) {
    switch (layout) {
        case ChannelLayout::kMono:
            return {Position::kFrontCenter};
        case ChannelLayout::kStereo:
            return {Position::kFrontLeft, Position::kFrontRight};
        case ChannelLayout::k3_1_2:
            return {Position::kFrontLeft, Position::kFrontRight, Position::kFrontCenter,
                    Position::kLowFrequency, Position::kTopFrontLeft, Position::kTopFrontRight};
        case ChannelLayout::k5_1:
            return {Position::kFrontLeft, Position::kFrontRight, Position::kFrontCenter,
                    Position::kLowFrequency, Position::kBackLeft, Position::kBackRight};
        case ChannelLayout::k5_1_2:
            return {Position::kFrontLeft, Position::kFrontRight, Position::kFrontCenter,
                    Position::kLowFrequency, Position::kBackLeft, Position::kBackRight,
                    Position::kTopFrontLeft, Position::kTopFrontRight};
        case ChannelLayout::k5_1_4:
            return {Position::kFrontLeft, Position::kFrontRight, Position::kFrontCenter,
                    Position::kLowFrequency, Position::kBackLeft, Position::kBackRight,
                    Position::kTopFrontLeft, Position::kTopFrontRight,
                    Position::kTopBackLeft, Position::kTopBackRight};
        case ChannelLayout::k7_1:
            return {Position::kFrontLeft, Position::kFrontRight, Position::kFrontCenter,
                    Position::kLowFrequency, Position::kBackLeft, Position::kBackRight,
                    Position::kSideLeft, Position::kSideRight};
        case ChannelLayout::k7_1_2:
            return {Position::kFrontLeft, Position::kFrontRight, Position::kFrontCenter,
                    Position::kLowFrequency, Position::kBackLeft, Position::kBackRight,
                    Position::kSideLeft, Position::kSideRight,
                    Position::kTopFrontLeft, Position::kTopFrontRight};
        case ChannelLayout::k7_1_4:
            return {Position::kFrontLeft, Position::kFrontRight, Position::kFrontCenter,
                    Position::kLowFrequency, Position::kBackLeft, Position::kBackRight,
                    Position::kSideLeft, Position::kSideRight,
                    Position::kTopFrontLeft, Position::kTopFrontRight,
                    Position::kTopBackLeft, Position::kTopBackRight};
        case ChannelLayout::kUnset:
            return {};
    }
    return {};
}

const char* InputPositionShortName(ChannelLayout layout, Position position) {
    switch (position) {
        case Position::kFrontLeft:
            return "L";
        case Position::kFrontRight:
            return "R";
        case Position::kFrontCenter:
            return "C";
        case Position::kLowFrequency:
            return "LFE";
        case Position::kSideLeft:
            return "Lss";
        case Position::kSideRight:
            return "Rss";
        case Position::kBackLeft:
            switch (layout) {
                case ChannelLayout::k5_1:
                case ChannelLayout::k5_1_2:
                case ChannelLayout::k5_1_4:
                    return "Ls";
                case ChannelLayout::k7_1:
                case ChannelLayout::k7_1_2:
                case ChannelLayout::k7_1_4:
                    return "Lrs";
                default:
                    return "?";
            }
        case Position::kBackRight:
            switch (layout) {
                case ChannelLayout::k5_1:
                case ChannelLayout::k5_1_2:
                case ChannelLayout::k5_1_4:
                    return "Rs";
                case ChannelLayout::k7_1:
                case ChannelLayout::k7_1_2:
                case ChannelLayout::k7_1_4:
                    return "Rrs";
                default:
                    return "?";
            }
        case Position::kTopFrontLeft:
            return "Ltf";
        case Position::kTopFrontRight:
            return "Rtf";
        case Position::kTopMiddleLeft:
            return layout == ChannelLayout::k7_1_2 ? "Ltf" : "?";
        case Position::kTopMiddleRight:
            return layout == ChannelLayout::k7_1_2 ? "Rtf" : "?";
        case Position::kTopBackLeft:
            if (layout == ChannelLayout::k5_1_4)
                return "Ltr";
            if (layout == ChannelLayout::k7_1_4)
                return "Ltb";
            return "?";
        case Position::kTopBackRight:
            if (layout == ChannelLayout::k5_1_4)
                return "Rtr";
            if (layout == ChannelLayout::k7_1_4)
                return "Rtb";
            return "?";
        case Position::kUnknown:
            return "?";
    }
    return "?";
}

int FindTargetPosition(ChannelLayout layout, const std::vector<Position>& positions,
                       Position wanted) {
    if ((layout == ChannelLayout::k5_1 || layout == ChannelLayout::k5_1_2 ||
         layout == ChannelLayout::k5_1_4) && wanted == Position::kSideLeft &&
        FindPosition(positions, wanted) < 0)
        return FindPosition(positions, Position::kBackLeft);
    if ((layout == ChannelLayout::k5_1 || layout == ChannelLayout::k5_1_2 ||
         layout == ChannelLayout::k5_1_4) && wanted == Position::kSideRight &&
        FindPosition(positions, wanted) < 0)
        return FindPosition(positions, Position::kBackRight);
    if (layout == ChannelLayout::k7_1_2 && wanted == Position::kTopFrontLeft &&
        FindPosition(positions, wanted) < 0)
        return FindPosition(positions, Position::kTopMiddleLeft);
    if (layout == ChannelLayout::k7_1_2 && wanted == Position::kTopFrontRight &&
        FindPosition(positions, wanted) < 0)
        return FindPosition(positions, Position::kTopMiddleRight);
    return FindPosition(positions, wanted);
}

}  // namespace

bool MediaProbeInfo::HasVideo() const {
    return std::any_of(streams.begin(), streams.end(), [](const MediaStreamInfo& stream) {
        return stream.kind == MediaStreamKind::kVideo;
    });
}

bool MediaProbeInfo::HasAudio() const {
    return std::any_of(streams.begin(), streams.end(), [](const MediaStreamInfo& stream) {
        return stream.kind == MediaStreamKind::kAudio;
    });
}

bool IsAmbisonicsRole(AudioRole role) {
    return role == AudioRole::kFirstOrderAmbisonics || role == AudioRole::kSecondOrderAmbisonics ||
           role == AudioRole::kThirdOrderAmbisonics;
}

int ChannelCount(ChannelLayout layout) {
    switch (layout) {
        case ChannelLayout::kMono:
            return 1;
        case ChannelLayout::kStereo:
            return 2;
        case ChannelLayout::k3_1_2:
        case ChannelLayout::k5_1:
            return 6;
        case ChannelLayout::k5_1_2:
        case ChannelLayout::k7_1:
            return 8;
        case ChannelLayout::k5_1_4:
        case ChannelLayout::k7_1_2:
            return 10;
        case ChannelLayout::k7_1_4:
            return 12;
        case ChannelLayout::kUnset:
            return 0;
    }
    return 0;
}

std::vector<ChannelLayout> ChannelLayoutsForChannelCount(int channel_count) {
    static const std::vector<ChannelLayout> supported = {
        ChannelLayout::kMono, ChannelLayout::kStereo, ChannelLayout::k3_1_2,
        ChannelLayout::k5_1, ChannelLayout::k5_1_2, ChannelLayout::k5_1_4,
        ChannelLayout::k7_1, ChannelLayout::k7_1_2, ChannelLayout::k7_1_4};
    std::vector<ChannelLayout> result;
    std::copy_if(supported.begin(), supported.end(), std::back_inserter(result),
                 [channel_count](ChannelLayout layout) {
                     return ChannelCount(layout) == channel_count;
                 });
    return result;
}

ChannelLayout DetectChannelLayout(int channel_count,
                                  const std::vector<SpeakerPosition>& positions,
                                  bool positions_complete) {
    if (!positions_complete || static_cast<int>(positions.size()) != channel_count)
        return ChannelLayout::kUnset;
    ChannelLayout match = ChannelLayout::kUnset;
    for (const ChannelLayout candidate : ChannelLayoutsForChannelCount(channel_count)) {
        if (!MatchesLayout(candidate, positions))
            continue;
        if (match != ChannelLayout::kUnset)
            return ChannelLayout::kUnset;
        match = candidate;
    }
    return match;
}

bool BuildChannelOrderForIamf(ChannelLayout layout,
                              std::vector<int>* source_indices_for_output,
                              std::string* error) {
    if (source_indices_for_output == nullptr || layout == ChannelLayout::kUnset) {
        SetError(error, "a supported channel layout and mapping result are required");
        return false;
    }
    const int channel_count = ChannelCount(layout);
    if (channel_count <= 0) {
        SetError(error, "the selected channel layout is not supported");
        return false;
    }
    const std::vector<Position> input_positions = InputPositions(layout);
    const std::vector<Position> target_positions = TargetPositions(layout);
    if (static_cast<int>(input_positions.size()) != channel_count ||
        target_positions.size() != input_positions.size()) {
        SetError(error, "the selected layout does not have a complete input-to-IAMF channel order");
        return false;
    }
    source_indices_for_output->clear();
    source_indices_for_output->reserve(target_positions.size());
    for (const Position target : target_positions) {
        const int index = FindTargetPosition(layout, input_positions, target);
        if (index < 0) {
            SetError(error, "the selected input order cannot be mapped to the IAMF layout");
            return false;
        }
        source_indices_for_output->push_back(index);
    }
    return true;
}

const char* ToString(ChannelLayout layout) {
    switch (layout) {
        case ChannelLayout::kMono:
            return "Mono";
        case ChannelLayout::kStereo:
            return "Stereo";
        case ChannelLayout::k3_1_2:
            return "3.1.2";
        case ChannelLayout::k5_1:
            return "5.1";
        case ChannelLayout::k5_1_2:
            return "5.1.2";
        case ChannelLayout::k5_1_4:
            return "5.1.4";
        case ChannelLayout::k7_1:
            return "7.1";
        case ChannelLayout::k7_1_2:
            return "7.1.2";
        case ChannelLayout::k7_1_4:
            return "7.1.4";
        case ChannelLayout::kUnset:
            return "Unselected";
    }
    return "Unselected";
}

const char* IamfLayoutName(ChannelLayout layout) {
    switch (layout) {
        case ChannelLayout::kMono:
            return "LOUDSPEAKER_LAYOUT_MONO";
        case ChannelLayout::kStereo:
            return "LOUDSPEAKER_LAYOUT_STEREO";
        case ChannelLayout::k3_1_2:
            return "LOUDSPEAKER_LAYOUT_3_1_2_CH";
        case ChannelLayout::k5_1:
            return "LOUDSPEAKER_LAYOUT_5_1_CH";
        case ChannelLayout::k5_1_2:
            return "LOUDSPEAKER_LAYOUT_5_1_2_CH";
        case ChannelLayout::k5_1_4:
            return "LOUDSPEAKER_LAYOUT_5_1_4_CH";
        case ChannelLayout::k7_1:
            return "LOUDSPEAKER_LAYOUT_7_1_CH";
        case ChannelLayout::k7_1_2:
            return "LOUDSPEAKER_LAYOUT_7_1_2_CH";
        case ChannelLayout::k7_1_4:
            return "LOUDSPEAKER_LAYOUT_7_1_4_CH";
        case ChannelLayout::kUnset:
            return "";
    }
    return "";
}

std::string InputChannelOrderDescription(ChannelLayout layout) {
    const std::vector<Position> input_positions = InputPositions(layout);
    std::string description;
    for (const Position position : input_positions) {
        if (!description.empty())
            description += " / ";
        description += InputPositionShortName(layout, position);
    }
    return description;
}

const char* ToString(InputKind kind) {
    switch (kind) {
        case InputKind::kUnknown:
            return "unknown";
        case InputKind::kVideoContainer:
            return "video container";
        case InputKind::kStandaloneIamf:
            return "standalone IAMF";
        case InputKind::kAudioCandidate:
            return "audio candidate";
        case InputKind::kUnsupported:
            return "unsupported";
    }
    return "unknown";
}

const char* ToString(AudioRole role) {
    switch (role) {
        case AudioRole::kUnset:
            return "unassigned";
        case AudioRole::kFirstOrderAmbisonics:
            return "first-order ambisonics";
        case AudioRole::kSecondOrderAmbisonics:
            return "second-order ambisonics";
        case AudioRole::kThirdOrderAmbisonics:
            return "third-order ambisonics";
        case AudioRole::kHeadLockedStereo:
            return "head-locked stereo";
    }
    return "unassigned";
}

const char* ToString(ProjectMode mode) {
    switch (mode) {
        case ProjectMode::kUnset:
            return "unconfigured";
        case ProjectMode::kMuxExistingIamf:
            return "mux existing IAMF";
        case ProjectMode::kCreateIamfFromAudio:
            return "create IAMF from audio";
    }
    return "unconfigured";
}

void MuxProject::RecalculateMode() {
    mode = standalone_iamf ? ProjectMode::kMuxExistingIamf
                           : (!audio_inputs.empty() ? ProjectMode::kCreateIamfFromAudio : ProjectMode::kUnset);
}

bool MuxProject::AddInput(const MediaProbeInfo& input, std::string* error) {
    if (input.path.empty() || input.kind == InputKind::kUnknown || input.kind == InputKind::kUnsupported) {
        SetError(error, "input is not supported by the current project");
        return false;
    }
    if (input.kind == InputKind::kVideoContainer) {
        if (video) {
            SetError(error, "a video input is already configured");
            return false;
        }
        video = input;
        return true;
    }
    if (input.kind == InputKind::kStandaloneIamf) {
        if (!audio_inputs.empty()) {
            SetError(error, "an existing IAMF input cannot be mixed with source audio inputs");
            return false;
        }
        if (standalone_iamf) {
            SetError(error, "an IAMF input is already configured");
            return false;
        }
        standalone_iamf = input;
        RecalculateMode();
        return true;
    }
    if (standalone_iamf) {
        SetError(error, "source audio cannot be added while muxing an existing IAMF file");
        return false;
    }
    if (audio_inputs.size() >= 2) {
        SetError(error, "the first release accepts at most two source audio inputs");
        return false;
    }
    const auto duplicate = std::find_if(audio_inputs.begin(), audio_inputs.end(), [&input](const ProjectAudioInput& candidate) {
        return SamePath(candidate.media.path, input.path);
    });
    if (duplicate != audio_inputs.end()) {
        SetError(error, "this audio input is already configured");
        return false;
    }
    audio_inputs.push_back({input, AudioRole::kUnset, false});
    RecalculateMode();
    return true;
}

bool MuxProject::RemoveInput(const std::filesystem::path& path, std::string* error) {
    if (video && SamePath(video->path, path)) {
        video.reset();
        return true;
    }
    if (standalone_iamf && SamePath(standalone_iamf->path, path)) {
        standalone_iamf.reset();
        RecalculateMode();
        return true;
    }
    const auto previous_size = audio_inputs.size();
    audio_inputs.erase(std::remove_if(audio_inputs.begin(), audio_inputs.end(), [&path](const ProjectAudioInput& input) {
                           return SamePath(input.media.path, path);
                       }),
                       audio_inputs.end());
    if (audio_inputs.size() == previous_size) {
        SetError(error, "input path is not part of this project");
        return false;
    }
    RecalculateMode();
    return true;
}

bool MuxProject::AssignAudioRole(const std::filesystem::path& path, AudioRole role, bool confirmed, std::string* error) {
    if (role == AudioRole::kUnset) {
        SetError(error, "an audio role must be selected");
        return false;
    }
    const auto target = std::find_if(audio_inputs.begin(), audio_inputs.end(), [&path](const ProjectAudioInput& input) {
        return SamePath(input.media.path, path);
    });
    if (target == audio_inputs.end()) {
        SetError(error, "audio input path is not part of this project");
        return false;
    }
    const auto duplicate = std::find_if(audio_inputs.begin(), audio_inputs.end(), [&target, role](const ProjectAudioInput& input) {
        return &input != &*target && input.role == role;
    });
    if (duplicate != audio_inputs.end()) {
        SetError(error, "each audio role can be assigned to only one input");
        return false;
    }
    target->role = role;
    target->role_confirmed = confirmed;
    return true;
}

bool MuxProject::AssignChannelLayout(const std::filesystem::path& path, ChannelLayout layout,
                                     bool confirmed, std::string* error) {
    if (layout == ChannelLayout::kUnset || ChannelCount(layout) <= 0) {
        SetError(error, "a supported channel layout must be selected");
        return false;
    }
    const auto target = std::find_if(audio_inputs.begin(), audio_inputs.end(), [&path](const ProjectAudioInput& input) {
        return SamePath(input.media.path, path);
    });
    if (target == audio_inputs.end()) {
        SetError(error, "audio input path is not part of this project");
        return false;
    }
    target->channel_layout = layout;
    target->channel_layout_confirmed = confirmed;
    return true;
}

bool MuxProject::ValidateForExistingIamfMux(std::string* error) const {
    if (mode != ProjectMode::kMuxExistingIamf || !standalone_iamf) {
        SetError(error, "a standalone IAMF input is required for mux mode");
        return false;
    }
    if (!audio_inputs.empty()) {
        SetError(error, "mux mode cannot contain source audio inputs");
        return false;
    }
    return true;
}

bool MuxProject::ValidateForAudioEncoding(std::string* error) const {
    if (mode != ProjectMode::kCreateIamfFromAudio || standalone_iamf) {
        SetError(error, "source audio inputs are required for audio encoding mode");
        return false;
    }
    if (audio_inputs.empty() || audio_inputs.size() > 2) {
        SetError(error, "audio encoding requires one ambisonics input and an optional head-locked stereo input");
        return false;
    }
    bool has_ambisonics = false;
    bool has_head_locked_stereo = false;
    std::size_t ambisonics_count = 0;
    std::size_t head_locked_stereo_count = 0;
    for (const auto& input : audio_inputs) {
        if (!input.role_confirmed) {
            SetError(error, "assign a valid role to every audio input");
            return false;
        }
        if (!IsAmbisonicsRole(input.role) && input.role != AudioRole::kHeadLockedStereo) {
            SetError(error, "every source audio input must be Ambisonics or Head-Locked Stereo");
            return false;
        }
        if (IsAmbisonicsRole(input.role)) {
            has_ambisonics = true;
            ++ambisonics_count;
        }
        if (input.role == AudioRole::kHeadLockedStereo) {
            has_head_locked_stereo = true;
            ++head_locked_stereo_count;
        }
    }
    if (!has_ambisonics) {
        SetError(error, "audio encoding requires one Ambisonics input");
        return false;
    }
    if (ambisonics_count != 1 || head_locked_stereo_count > 1) {
        SetError(error, "audio encoding requires exactly one Ambisonics input and at most one Head-Locked Stereo input");
        return false;
    }
    if (has_head_locked_stereo) {
        for (const auto& input : audio_inputs) {
            const auto audio_stream = std::find_if(input.media.streams.begin(), input.media.streams.end(),
                                                   [](const MediaStreamInfo& stream) {
                                                       return stream.kind == MediaStreamKind::kAudio;
                                                   });
            if (input.role == AudioRole::kHeadLockedStereo &&
                (audio_stream == input.media.streams.end() || audio_stream->channel_count != 2)) {
                SetError(error, "Head-Locked Stereo input must contain exactly two channels");
                return false;
            }
        }
    }
    return true;
}

bool MuxProject::ValidateForChannelBasedEncoding(std::string* error) const {
    if (mode != ProjectMode::kCreateIamfFromAudio || standalone_iamf) {
        SetError(error, "source audio is required for Channel-based encoding mode");
        return false;
    }
    if (audio_inputs.size() != 1) {
        SetError(error, "Channel-based encoding requires exactly one audio input");
        return false;
    }
    const ProjectAudioInput& input = audio_inputs.front();
    if (input.media.kind != InputKind::kAudioCandidate || !input.media.HasAudio()) {
        SetError(error, "Channel-based mode requires a regular audio input");
        return false;
    }
    if (input.channel_layout == ChannelLayout::kUnset || ChannelCount(input.channel_layout) <= 0) {
        SetError(error, "select a supported Channel-based layout");
        return false;
    }
    if (!input.channel_layout_confirmed) {
        SetError(error, "confirm the selected input channel layout");
        return false;
    }
    const auto audio_stream = std::find_if(input.media.streams.begin(), input.media.streams.end(),
                                           [](const MediaStreamInfo& stream) {
                                               return stream.kind == MediaStreamKind::kAudio;
                                           });
    if (audio_stream == input.media.streams.end() ||
        audio_stream->channel_count != ChannelCount(input.channel_layout)) {
        SetError(error, "the selected channel layout does not match the source channel count");
        return false;
    }
    std::vector<int> channel_order;
    if (!BuildChannelOrderForIamf(input.channel_layout, &channel_order, error))
        return false;
    return true;
}

}  // namespace immersive_audio_muxer::app
