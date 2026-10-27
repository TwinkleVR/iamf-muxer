#include "app/media_probe_service.h"

#include "avformat_input.h"
#include "path_utils.h"
#include "runtime_logging.h"
#include "standalone_iamf_reader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace immersive_audio_muxer::app {
namespace {

const char* StandaloneIamfCodecName(uint32_t codec_id) {
    switch (codec_id) {
        case 0x4f707573:
            return "opus";
        case 0x6970636d:
            return "lpcm";
        case 0x6d703461:
            return "aac-lc";
        case 0x664c6143:
            return "flac";
        default:
            return "unknown";
    }
}

std::string AvError(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error_code, buffer, sizeof(buffer));
    return buffer;
}

std::string LowerExtension(const std::filesystem::path& path) {
    std::string extension = path_utils::ToUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return extension;
}

AudioRole SuggestedRole(int channel_count) {
    switch (channel_count) {
        case 4:
            return AudioRole::kFirstOrderAmbisonics;
        case 9:
            return AudioRole::kSecondOrderAmbisonics;
        case 16:
            return AudioRole::kThirdOrderAmbisonics;
        case 2:
            return AudioRole::kHeadLockedStereo;
        default:
            return AudioRole::kUnset;
    }
}

std::string ProfileName(uint8_t profile) {
    switch (profile) {
        case 0:
            return "Simple Profile";
        case 1:
            return "Base Profile";
        case 2:
            return "Base-Enhanced Profile";
        case 3:
            return "Base-Advanced Profile";
        case 4:
            return "Advanced 1 Profile";
        case 5:
            return "Advanced 2 Profile";
        default:
            return "Profile " + std::to_string(profile);
    }
}

std::string ProfileSummary(uint8_t primary, uint8_t additional) {
    const std::string primary_name = ProfileName(primary);
    if (additional == primary)
        return primary_name;
    return primary_name + " + " + ProfileName(additional);
}

MediaStreamKind StreamKind(AVMediaType type) {
    switch (type) {
        case AVMEDIA_TYPE_VIDEO:
            return MediaStreamKind::kVideo;
        case AVMEDIA_TYPE_AUDIO:
            return MediaStreamKind::kAudio;
        case AVMEDIA_TYPE_DATA:
            return MediaStreamKind::kData;
        default:
            return MediaStreamKind::kOther;
    }
}

SpeakerPosition SpeakerPositionFromAvChannel(AVChannel channel) {
    switch (channel) {
        case AV_CHAN_FRONT_LEFT:
            return SpeakerPosition::kFrontLeft;
        case AV_CHAN_FRONT_RIGHT:
            return SpeakerPosition::kFrontRight;
        case AV_CHAN_FRONT_CENTER:
            return SpeakerPosition::kFrontCenter;
        case AV_CHAN_FRONT_LEFT_OF_CENTER:
            return SpeakerPosition::kFrontLeftOfCenter;
        case AV_CHAN_FRONT_RIGHT_OF_CENTER:
            return SpeakerPosition::kFrontRightOfCenter;
        case AV_CHAN_LOW_FREQUENCY:
            return SpeakerPosition::kLowFrequency;
        case AV_CHAN_SIDE_LEFT:
        case AV_CHAN_BACK_LEFT:
            return channel == AV_CHAN_SIDE_LEFT ? SpeakerPosition::kSideLeft : SpeakerPosition::kBackLeft;
        case AV_CHAN_SIDE_RIGHT:
        case AV_CHAN_BACK_RIGHT:
            return channel == AV_CHAN_SIDE_RIGHT ? SpeakerPosition::kSideRight : SpeakerPosition::kBackRight;
        case AV_CHAN_TOP_FRONT_LEFT:
            return SpeakerPosition::kTopFrontLeft;
        case AV_CHAN_TOP_FRONT_RIGHT:
            return SpeakerPosition::kTopFrontRight;
        case AV_CHAN_TOP_SIDE_LEFT:
            return SpeakerPosition::kTopMiddleLeft;
        case AV_CHAN_TOP_SIDE_RIGHT:
            return SpeakerPosition::kTopMiddleRight;
        case AV_CHAN_TOP_BACK_LEFT:
            return SpeakerPosition::kTopBackLeft;
        case AV_CHAN_TOP_BACK_RIGHT:
            return SpeakerPosition::kTopBackRight;
#if LIBAVUTIL_VERSION_MAJOR >= 59
        case AV_CHAN_SIDE_SURROUND_LEFT:
            return SpeakerPosition::kSideLeft;
        case AV_CHAN_SIDE_SURROUND_RIGHT:
            return SpeakerPosition::kSideRight;
#endif
        default:
            return SpeakerPosition::kUnknown;
    }
}

void ReadChannelLayout(const AVChannelLayout& layout, MediaStreamInfo* stream) {
    if (stream == nullptr)
        return;
    std::array<char, 128> description{};
    if (av_channel_layout_describe(&layout, description.data(), description.size()) >= 0)
        stream->channel_layout_name = description.data();
    stream->channel_positions.clear();
    stream->channel_positions_complete = layout.order != AV_CHANNEL_ORDER_UNSPEC && layout.nb_channels > 0;
    std::vector<SpeakerPosition> seen;
    for (int index = 0; index < layout.nb_channels; ++index) {
        const SpeakerPosition position = SpeakerPositionFromAvChannel(
            av_channel_layout_channel_from_index(&layout, static_cast<unsigned int>(index)));
        stream->channel_positions.push_back(position);
        if (position == SpeakerPosition::kUnknown ||
            std::find(seen.begin(), seen.end(), position) != seen.end())
            stream->channel_positions_complete = false;
        seen.push_back(position);
    }
}

bool ProbeStandaloneIamf(const std::filesystem::path& path, MediaProbeInfo* info, std::string* error) {
    StandaloneIamfReader reader;
    std::string reader_error;
    if (!reader.Open(path_utils::ToUtf8(path), &reader_error) || !reader.ValidateObjectAudioAndParameters(&reader_error)) {
        if (error)
            *error = "cannot parse standalone IAMF: " + reader_error;
        return false;
    }
    const IamfIndexSummary& summary = reader.summary();
    info->kind = InputKind::kStandaloneIamf;
    info->duration_ms = summary.sample_rate == 0 ? 0 : static_cast<int64_t>(summary.total_samples * 1000 / summary.sample_rate);
    info->iamf_profile = ProfileSummary(summary.primary_profile, summary.additional_profile);
    info->iamf_mix_presentation_annotation = summary.mix_presentation_annotation;
    info->iamf_mix_presentation_count = summary.mix_presentation_count;
    info->iamf_audio_element_count = summary.audio_element_count;
    MediaStreamInfo stream;
    stream.index = 0;
    stream.kind = MediaStreamKind::kAudio;
    stream.sample_rate = static_cast<int>(summary.sample_rate);
    stream.codec_name = StandaloneIamfCodecName(reader.codec_id());
    info->streams.push_back(std::move(stream));
    info->diagnostic = "validated standalone IAMF with " + std::to_string(summary.temporal_unit_count) + " temporal units";
    return true;
}

}  // namespace

bool MediaProbeService::Probe(const std::filesystem::path& path, MediaProbeInfo* info, std::string* error) {
    if (info == nullptr || path.empty()) {
        if (error)
            *error = "an input path is required";
        IAMF_MUX_LOG_WARN("[Probe] rejected request with empty path or result pointer");
        return false;
    }
    IAMF_MUX_LOG_INFO("[Probe] starting media inspection: path={}", path_utils::ToUtf8(path));
    *info = {};
    info->path = path;
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(path, filesystem_error)) {
        if (error)
            *error = "input path is not a regular file";
        IAMF_MUX_LOG_ERROR("[Probe] input is not a regular file: path={} filesystem_error={}",
                           path_utils::ToUtf8(path), filesystem_error.message());
        return false;
    }
    if (LowerExtension(path) == ".iamf") {
        if (!ProbeStandaloneIamf(path, info, error)) {
            info->kind = InputKind::kUnsupported;
            info->diagnostic = error ? *error : "standalone IAMF parsing failed";
            IAMF_MUX_LOG_ERROR("[Probe] standalone IAMF inspection failed: path={} error={}",
                               path_utils::ToUtf8(path), info->diagnostic);
            return false;
        }
        IAMF_MUX_LOG_INFO(
            "[Probe] standalone IAMF validated: path={} duration_ms={} profile={} audio_elements={} mixes={} annotation={} streams={} diagnostic={}",
            path_utils::ToUtf8(path), info->duration_ms, info->iamf_profile,
            info->iamf_audio_element_count, info->iamf_mix_presentation_count,
            info->iamf_mix_presentation_annotation, info->streams.size(), info->diagnostic);
        return true;
    }

    AVFormatContext* context = nullptr;
    const std::string input_path = path_utils::ToUtf8(path);
    bool audio_tracks_suppressed = false;
    const int status = avformat_input::Open(&context, input_path, {}, &audio_tracks_suppressed);
    if (status < 0) {
        IAMF_MUX_LOG_WARN("[Probe] FFmpeg open failed; trying standalone IAMF fallback: path={} ffmpeg_status={} error={}",
                          input_path, status, AvError(status));
        MediaProbeInfo iamf_info;
        std::string iamf_error;
        if (ProbeStandaloneIamf(path, &iamf_info, &iamf_error)) {
            iamf_info.path = path;
            *info = std::move(iamf_info);
            IAMF_MUX_LOG_INFO(
                "[Probe] FFmpeg fallback recognized standalone IAMF: path={} duration_ms={} profile={} temporal_units={} annotation={}",
                input_path, info->duration_ms, info->iamf_profile,
                info->iamf_audio_element_count, info->iamf_mix_presentation_annotation);
            return true;
        }
        if (error)
            *error = "cannot open media input: " + AvError(status);
        IAMF_MUX_LOG_ERROR("[Probe] media open failed: path={} ffmpeg_status={} error={}",
                           input_path, status, error == nullptr ? AvError(status) : *error);
        return false;
    }
    if (context->duration != AV_NOPTS_VALUE)
        info->duration_ms = std::max<int64_t>(0, context->duration / (AV_TIME_BASE / 1000));
    const char* container_name = context->iformat == nullptr || context->iformat->name == nullptr
                                     ? "unknown"
                                     : context->iformat->name;
    IAMF_MUX_LOG_INFO("[Probe] FFmpeg opened input: path={} container={} duration_ms={} stream_count={} suppressed_iamf_audio={}",
                      input_path, container_name, info->duration_ms,
                      context->nb_streams, audio_tracks_suppressed);

    bool has_video = false;
    bool has_audio = false;
    bool has_iamf_stream = false;
    for (unsigned int index = 0; index < context->nb_streams; ++index) {
        const AVStream* stream = context->streams[index];
        const AVCodecParameters* parameters = stream->codecpar;
        if (parameters == nullptr)
            continue;
        MediaStreamInfo item;
        item.index = static_cast<int>(index);
        item.codec_name = avcodec_get_name(parameters->codec_id);
        const bool is_iamf_stream = parameters->codec_tag == MKTAG('i', 'a', 'm', 'f') ||
                                    item.codec_name == "iamf";
        if (is_iamf_stream)
            item.codec_name = "iamf";
        const bool is_audio_stream = parameters->codec_type == AVMEDIA_TYPE_AUDIO || is_iamf_stream;
        item.kind = is_iamf_stream ? MediaStreamKind::kAudio : StreamKind(parameters->codec_type);
        item.bit_rate = parameters->bit_rate > 0 ? parameters->bit_rate : 0;
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            has_video = true;
            item.width = parameters->width;
            item.height = parameters->height;
            AVRational frame_rate = stream->avg_frame_rate;
            if (frame_rate.num <= 0 || frame_rate.den <= 0)
                frame_rate = stream->r_frame_rate;
            if (frame_rate.num > 0 && frame_rate.den > 0)
                item.frame_rate = av_q2d(frame_rate);
        } else if (is_audio_stream) {
            has_audio = true;
            item.sample_rate = parameters->sample_rate;
            item.bit_depth = std::max(parameters->bits_per_raw_sample, parameters->bits_per_coded_sample);
            if (item.bit_depth <= 0)
                item.bit_depth = av_get_bits_per_sample(parameters->codec_id);
            item.channel_count = parameters->ch_layout.nb_channels;
            ReadChannelLayout(parameters->ch_layout, &item);
            has_iamf_stream = has_iamf_stream || is_iamf_stream;
        }
        const char* stream_kind = parameters->codec_type == AVMEDIA_TYPE_VIDEO
                                      ? "video"
                                      : (is_audio_stream ? "audio" : "other");
        IAMF_MUX_LOG_INFO(
            "[Probe] stream index={} type={} codec={} codec_id={} codec_tag=0x{:08x} time_base={}/{} bit_rate={}",
            index, stream_kind, item.codec_name, static_cast<int>(parameters->codec_id),
            parameters->codec_tag, stream->time_base.num, stream->time_base.den, item.bit_rate);
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            IAMF_MUX_LOG_INFO("[Probe] video stream index={} size={}x{} frame_rate={:.6f}",
                              index, item.width, item.height, item.frame_rate);
        } else if (is_audio_stream) {
            std::array<char, 128> channel_layout_name{};
            if (av_channel_layout_describe(&parameters->ch_layout, channel_layout_name.data(),
                                           channel_layout_name.size()) < 0)
                channel_layout_name[0] = '\0';
            IAMF_MUX_LOG_INFO(
                "[Probe] audio stream index={} sample_rate={} channels={} bit_depth={} channel_layout={} iamf={}",
                index, item.sample_rate, item.channel_count, item.bit_depth,
                channel_layout_name[0] == '\0' ? "unknown" : channel_layout_name.data(), is_iamf_stream);
        }
        info->streams.push_back(std::move(item));
    }
    if (audio_tracks_suppressed) {
        // FFmpeg may only be able to open this video after the IAMF tracks are
        // hidden from its MP4 view. Keep their presence visible to the UI so
        // the user can request passthrough; iamf-tools validates them later.
        MediaStreamInfo iamf_stream;
        iamf_stream.index = -1;
        iamf_stream.kind = MediaStreamKind::kAudio;
        iamf_stream.codec_name = "iamf";
        info->streams.push_back(std::move(iamf_stream));
        has_audio = true;
        has_iamf_stream = true;
    }
    avformat_input::Close(&context);

    if (has_video) {
        info->kind = InputKind::kVideoContainer;
        info->diagnostic = "video container detected";
        IAMF_MUX_LOG_INFO("[Probe] completed: path={} kind={} duration_ms={} streams={} diagnostic={}",
                          input_path, ToString(info->kind), info->duration_ms,
                          info->streams.size(), info->diagnostic);
        return true;
    }
    if (has_audio && !has_iamf_stream) {
        info->kind = InputKind::kAudioCandidate;
        const auto first_audio = std::find_if(info->streams.begin(), info->streams.end(), [](const MediaStreamInfo& stream) {
            return stream.kind == MediaStreamKind::kAudio;
        });
        if (first_audio != info->streams.end()) {
            info->suggested_audio_role = SuggestedRole(first_audio->channel_count);
            info->channel_layout_candidates = ChannelLayoutsForChannelCount(first_audio->channel_count);
            info->suggested_channel_layout = DetectChannelLayout(
                first_audio->channel_count, first_audio->channel_positions,
                first_audio->channel_positions_complete);
            info->channel_layout_detection_exact =
                info->suggested_channel_layout != ChannelLayout::kUnset;
            if (!info->channel_layout_detection_exact && info->channel_layout_candidates.size() == 1)
                info->suggested_channel_layout = info->channel_layout_candidates.front();
        }
        info->diagnostic = "audio input detected; expected 4, 9, or 16 channels with ACN/SN3D ordering";
        IAMF_MUX_LOG_INFO(
            "[Probe] completed: path={} kind={} duration_ms={} streams={} suggested_role={} suggested_layout={} layout_exact={} diagnostic={}",
            input_path, ToString(info->kind), info->duration_ms, info->streams.size(),
            ToString(info->suggested_audio_role), ToString(info->suggested_channel_layout),
            info->channel_layout_detection_exact, info->diagnostic);
        return true;
    }
    if (!has_video && (has_iamf_stream || !has_audio)) {
        MediaProbeInfo iamf_info;
        std::string iamf_error;
        if (ProbeStandaloneIamf(path, &iamf_info, &iamf_error)) {
            iamf_info.path = path;
            *info = std::move(iamf_info);
            IAMF_MUX_LOG_INFO("[Probe] completed using standalone IAMF fallback: path={} duration_ms={} profile={} annotation={}",
                              input_path, info->duration_ms, info->iamf_profile,
                              info->iamf_mix_presentation_annotation);
            return true;
        }
    }
    info->kind = InputKind::kUnsupported;
    info->diagnostic = has_iamf_stream ? "MP4-wrapped IAMF is not a first-release source input" : "input has no supported video or audio stream";
    if (error)
        *error = info->diagnostic;
    IAMF_MUX_LOG_ERROR("[Probe] unsupported media: path={} container={} has_video={} has_audio={} has_iamf={} error={}",
                       input_path, container_name, has_video, has_audio, has_iamf_stream,
                       info->diagnostic);
    return false;
}

}  // namespace immersive_audio_muxer::app
