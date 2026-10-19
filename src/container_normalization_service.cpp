#include "container_normalization_service.h"

#include "avformat_input.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace immersive_audio_muxer {
namespace {

std::string AvError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

void SetError(std::string* error, const std::string& message) {
    if (error)
        *error = message;
}

int InterruptCallback(void* opaque) {
    const auto* callback = static_cast<const std::function<bool()>*>(opaque);
    return callback != nullptr && *callback && (*callback)() ? 1 : 0;
}

bool IsCancelled(const std::function<bool()>& callback) {
    return callback && callback();
}

bool SamePath(const std::string& left, const std::string& right) {
    std::error_code left_error;
    std::error_code right_error;
    const std::filesystem::path left_path = std::filesystem::absolute(std::filesystem::u8path(left), left_error);
    const std::filesystem::path right_path = std::filesystem::absolute(std::filesystem::u8path(right), right_error);
    if (left_error || right_error)
        return left == right;
    std::string left_key = left_path.lexically_normal().u8string();
    std::string right_key = right_path.lexically_normal().u8string();
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

bool CopyChapter(const AVChapter* source, AVFormatContext* output, std::string* error) {
    if (source == nullptr || output == nullptr)
        return false;
    AVChapter* chapter = static_cast<AVChapter*>(av_mallocz(sizeof(AVChapter)));
    if (chapter == nullptr) {
        SetError(error, "cannot allocate normalized MP4 chapter");
        return false;
    }
    chapter->id = source->id;
    chapter->time_base = source->time_base;
    chapter->start = source->start;
    chapter->end = source->end;
    if (av_dict_copy(&chapter->metadata, source->metadata, 0) < 0) {
        av_dict_free(&chapter->metadata);
        av_free(chapter);
        SetError(error, "cannot copy normalized MP4 chapter metadata");
        return false;
    }
    AVChapter** chapters = static_cast<AVChapter**>(av_realloc_array(
        output->chapters, output->nb_chapters + 1, sizeof(*output->chapters)));
    if (chapters == nullptr) {
        av_dict_free(&chapter->metadata);
        av_free(chapter);
        SetError(error, "cannot grow normalized MP4 chapter table");
        return false;
    }
    output->chapters = chapters;
    output->chapters[output->nb_chapters++] = chapter;
    return true;
}

void RemoveOutputFile(const std::string& output_path, bool output_created) {
    if (!output_created)
        return;
    std::error_code error;
    std::filesystem::remove(std::filesystem::u8path(output_path), error);
}

bool IsOptionalStream(const AVStream* stream, bool keep_source_audio) {
    if (stream == nullptr || stream->codecpar == nullptr)
        return false;
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT)
        return true;
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !keep_source_audio)
        return true;
    return false;
}

bool IsSubRipSubtitle(const AVStream* stream) {
    return stream != nullptr && stream->codecpar != nullptr && stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
           stream->codecpar->codec_id == AV_CODEC_ID_SUBRIP;
}

bool IsUnsupportedSubtitle(const AVStream* stream, const AVOutputFormat* output_format) {
    if (stream == nullptr || stream->codecpar == nullptr || output_format == nullptr ||
        stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
        return false;
    return avformat_query_codec(output_format, stream->codecpar->codec_id, FF_COMPLIANCE_NORMAL) != 1;
}

std::string StreamDescription(unsigned int index, const AVCodecParameters* parameters) {
    return "stream " + std::to_string(index) + " (" +
           (parameters == nullptr ? std::string("unknown") : avcodec_get_name(parameters->codec_id)) + ")";
}

struct BitstreamFilterDeleter {
    void operator()(AVBSFContext* context) const { av_bsf_free(&context); }
};

using BitstreamFilter = std::unique_ptr<AVBSFContext, BitstreamFilterDeleter>;

struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

using OwnedPacket = std::unique_ptr<AVPacket, PacketDeleter>;

struct VideoDtsRepairState {
    bool enabled = false;
    bool has_valid_dts = false;
    bool rebuild_dts_at_constant_frame_rate = false;
    bool cfr_anchor_written = false;
    int64_t last_written_dts = AV_NOPTS_VALUE;
    int64_t dts_shift = 0;
    int64_t cfr_anchor_dts = AV_NOPTS_VALUE;
    int64_t cfr_next_frame_index = 1;
    int64_t cfr_pts_origin = AV_NOPTS_VALUE;
    AVRational cfr_frame_duration{0, 1};
    size_t buffered_bytes = 0;
    std::vector<OwnedPacket> leading_packets_without_dts;
};

constexpr size_t kMaximumLeadingVideoPacketsWithoutDts = 4096;
constexpr size_t kMaximumLeadingVideoPacketBytesWithoutDts = 128 * 1024 * 1024;

bool IsTimestampOnFrameGrid(int64_t timestamp, int64_t origin, AVRational stream_time_base,
                            AVRational frame_duration) {
    if (timestamp == AV_NOPTS_VALUE || origin == AV_NOPTS_VALUE)
        return false;
    const int64_t delta = timestamp - origin;
    const int64_t frame_index = av_rescale_q(delta, stream_time_base, frame_duration);
    const int64_t expected_delta = av_rescale_q(frame_index, frame_duration, stream_time_base);
    return std::fabs(static_cast<long double>(delta) - static_cast<long double>(expected_delta)) <= 1.0L;
}

bool CreateSubRipFilter(const AVStream* source_stream, unsigned int stream_index, BitstreamFilter* filter, std::string* error) {
    if (source_stream == nullptr || source_stream->codecpar == nullptr || filter == nullptr) {
        SetError(error, "cannot prepare SubRip conversion for an invalid stream");
        return false;
    }
    const AVBitStreamFilter* text_filter = av_bsf_get_by_name("text2movsub");
    if (text_filter == nullptr) {
        SetError(error, "FFmpeg does not provide the text2movsub filter for subtitle stream " + std::to_string(stream_index));
        return false;
    }
    AVBSFContext* context = nullptr;
    int status = av_bsf_alloc(text_filter, &context);
    if (status < 0 || context == nullptr) {
        SetError(error, "cannot allocate SubRip conversion for " + StreamDescription(stream_index, source_stream->codecpar) + ": " +
                            AvError(status < 0 ? status : AVERROR(ENOMEM)));
        av_bsf_free(&context);
        return false;
    }
    BitstreamFilter prepared_filter(context);
    status = avcodec_parameters_copy(prepared_filter->par_in, source_stream->codecpar);
    if (status < 0) {
        SetError(error, "cannot configure SubRip conversion for " + StreamDescription(stream_index, source_stream->codecpar) + ": " +
                            AvError(status));
        return false;
    }
    prepared_filter->time_base_in = source_stream->time_base;
    status = av_bsf_init(prepared_filter.get());
    if (status < 0) {
        SetError(error, "cannot initialize SubRip conversion for " + StreamDescription(stream_index, source_stream->codecpar) + ": " +
                            AvError(status));
        return false;
    }
    prepared_filter->par_out->codec_id = AV_CODEC_ID_MOV_TEXT;
    prepared_filter->par_out->codec_tag = 0;
    *filter = std::move(prepared_filter);
    return true;
}

}  // namespace

bool ContainerNormalizationService::NormalizeToMp4(const std::string& input_path, const std::string& output_path,
                                                   int requested_video_stream_index, bool keep_source_audio,
                                                   int* normalized_video_stream_index, std::string* error,
                                                   const std::function<bool()>& cancellation_requested) {
    if (normalized_video_stream_index == nullptr) {
        SetError(error, "normalized video stream index output is required");
        return false;
    }
    *normalized_video_stream_index = -1;
    if (input_path.empty() || output_path.empty() || SamePath(input_path, output_path)) {
        SetError(error, "container normalization requires distinct input and output paths");
        return false;
    }
    std::error_code filesystem_error;
    const auto input_filesystem_path = std::filesystem::u8path(input_path);
    const auto output_filesystem_path = std::filesystem::u8path(output_path);
    if (!std::filesystem::is_regular_file(input_filesystem_path, filesystem_error) || filesystem_error) {
        SetError(error, "normalization input is not a regular file: " + input_path);
        return false;
    }
    filesystem_error.clear();
    if (std::filesystem::exists(output_filesystem_path, filesystem_error) || filesystem_error) {
        SetError(error, "normalization output already exists: " + output_path);
        return false;
    }

    AVFormatContext* input = nullptr;
    int status = avformat_input::Open(&input, input_path, cancellation_requested);
    if (status < 0) {
        SetError(error, "cannot open or read input streams for MP4 normalization: " + AvError(status));
        return false;
    }
    if (IsCancelled(cancellation_requested)) {
        avformat_input::Close(&input);
        SetError(error, "operation cancelled while probing input for MP4 normalization");
        return false;
    }
    if (input->nb_streams == 0) {
        avformat_input::Close(&input);
        SetError(error, "input has no streams that can be normalized to MP4");
        return false;
    }
    if (input->nb_stream_groups != 0) {
        avformat_input::Close(&input);
        SetError(error, "input contains stream groups that cannot be preserved by MP4 normalization");
        return false;
    }

    int selected_video_stream_index = requested_video_stream_index;
    if (selected_video_stream_index < 0)
        selected_video_stream_index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (selected_video_stream_index < 0 || selected_video_stream_index >= static_cast<int>(input->nb_streams) ||
        input->streams[selected_video_stream_index] == nullptr || input->streams[selected_video_stream_index]->codecpar == nullptr ||
        input->streams[selected_video_stream_index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        avformat_input::Close(&input);
        SetError(error, "requested video stream index is not a video stream during MP4 normalization");
        return false;
    }

    AVFormatContext* output = nullptr;
    status = avformat_alloc_output_context2(&output, nullptr, "mp4", output_path.c_str());
    if (status < 0 || output == nullptr) {
        if (output != nullptr)
            avformat_free_context(output);
        avformat_input::Close(&input);
        SetError(error, "cannot allocate MP4 normalization output: " + AvError(status < 0 ? status : AVERROR_UNKNOWN));
        return false;
    }
    output->interrupt_callback.callback = InterruptCallback;
    output->interrupt_callback.opaque = const_cast<std::function<bool()>*>(&cancellation_requested);
    std::vector<AVStream*> output_streams(input->nb_streams, nullptr);
    std::vector<BitstreamFilter> subtitle_filters(input->nb_streams);
    std::vector<VideoDtsRepairState> video_dts_states(input->nb_streams);
    for (unsigned int index = 0; index < input->nb_streams; ++index) {
        const AVStream* source_stream = input->streams[index];
        if (source_stream == nullptr || source_stream->codecpar == nullptr) {
            SetError(error, "input contains an invalid stream at index " + std::to_string(index));
            avformat_free_context(output);
            avformat_input::Close(&input);
            return false;
        }
        if (IsOptionalStream(source_stream, keep_source_audio))
            continue;

        const bool convert_subrip = IsSubRipSubtitle(source_stream);
        if (!convert_subrip && IsUnsupportedSubtitle(source_stream, output->oformat)) {
            SetError(error, "MP4 normalization cannot preserve " + StreamDescription(index, source_stream->codecpar) +
                                ": subtitle codec is unsupported by MP4 and has no conversion path");
            avformat_free_context(output);
            avformat_input::Close(&input);
            return false;
        }
        if (convert_subrip && !CreateSubRipFilter(source_stream, index, &subtitle_filters[index], error)) {
            avformat_free_context(output);
            avformat_input::Close(&input);
            return false;
        }

        const AVCodecID output_codec_id = convert_subrip ? AV_CODEC_ID_MOV_TEXT : source_stream->codecpar->codec_id;
        const int codec_support = avformat_query_codec(output->oformat, output_codec_id, FF_COMPLIANCE_NORMAL);
        if (codec_support == 0) {
            SetError(error, "MP4 normalization cannot preserve " + StreamDescription(index, source_stream->codecpar) +
                                ": codec is unsupported by MP4");
            avformat_free_context(output);
            avformat_input::Close(&input);
            return false;
        }
        AVStream* destination_stream = avformat_new_stream(output, nullptr);
        if (destination_stream == nullptr) {
            SetError(error, "cannot allocate normalized MP4 stream " + std::to_string(index));
            avformat_free_context(output);
            avformat_input::Close(&input);
            return false;
        }
        status = avcodec_parameters_copy(destination_stream->codecpar,
                                         convert_subrip ? subtitle_filters[index]->par_out : source_stream->codecpar);
        if (status < 0) {
            SetError(error, "cannot copy stream " + std::to_string(index) + " codec parameters: " + AvError(status));
            avformat_free_context(output);
            avformat_input::Close(&input);
            return false;
        }
        destination_stream->codecpar->codec_tag = 0;
        destination_stream->time_base = source_stream->time_base;
        destination_stream->start_time = source_stream->start_time;
        destination_stream->duration = source_stream->duration;
        destination_stream->nb_frames = source_stream->nb_frames;
        destination_stream->discard = source_stream->discard;
        destination_stream->avg_frame_rate = source_stream->avg_frame_rate;
        destination_stream->r_frame_rate = source_stream->r_frame_rate;
        destination_stream->pts_wrap_bits = source_stream->pts_wrap_bits;
        destination_stream->sample_aspect_ratio = source_stream->sample_aspect_ratio;
        destination_stream->disposition = source_stream->disposition;
        destination_stream->id = source_stream->id;
        status = av_dict_copy(&destination_stream->metadata, source_stream->metadata, 0);
        if (status < 0) {
            SetError(error, "cannot copy stream " + std::to_string(index) + " metadata: " + AvError(status));
            avformat_free_context(output);
            avformat_input::Close(&input);
            return false;
        }
        if ((source_stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
            status = av_packet_ref(&destination_stream->attached_pic, &source_stream->attached_pic);
            if (status < 0) {
                SetError(error, "cannot copy attached picture for stream " + std::to_string(index) + ": " + AvError(status));
                avformat_free_context(output);
                avformat_input::Close(&input);
                return false;
            }
        }
        output_streams[index] = destination_stream;
        video_dts_states[index].enabled = source_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                                          (source_stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0;
        if (static_cast<int>(index) == selected_video_stream_index)
            *normalized_video_stream_index = destination_stream->index;
    }
    if (*normalized_video_stream_index < 0) {
        SetError(error, "selected video stream was not copied during MP4 normalization");
        avformat_free_context(output);
        avformat_input::Close(&input);
        return false;
    }
    status = av_dict_copy(&output->metadata, input->metadata, 0);
    if (status < 0) {
        SetError(error, "cannot copy MP4 format metadata: " + AvError(status));
        avformat_free_context(output);
        avformat_input::Close(&input);
        return false;
    }
    for (unsigned int index = 0; index < input->nb_chapters; ++index) {
        if (!CopyChapter(input->chapters[index], output, error)) {
            avformat_free_context(output);
            avformat_input::Close(&input);
            return false;
        }
    }

    bool output_created = true;
    if (!(output->oformat->flags & AVFMT_NOFILE)) {
        status = avio_open(&output->pb, output_path.c_str(), AVIO_FLAG_WRITE);
        if (status < 0) {
            SetError(error, "cannot open normalized MP4 output: " + AvError(status));
            avformat_free_context(output);
            avformat_input::Close(&input);
            RemoveOutputFile(output_path, output_created);
            return false;
        }
    }
    status = avformat_write_header(output, nullptr);
    if (status < 0) {
        SetError(error, "FFmpeg cannot write normalized MP4 header: " + AvError(status));
        if (output->pb != nullptr && !(output->oformat->flags & AVFMT_NOFILE))
            avio_closep(&output->pb);
        avformat_free_context(output);
        avformat_input::Close(&input);
        RemoveOutputFile(output_path, output_created);
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    AVPacket* filtered_packet = av_packet_alloc();
    if (packet == nullptr || filtered_packet == nullptr) {
        SetError(error, "cannot allocate packet for MP4 normalization");
        av_packet_free(&packet);
        av_packet_free(&filtered_packet);
        if (output->pb != nullptr && !(output->oformat->flags & AVFMT_NOFILE))
            avio_closep(&output->pb);
        avformat_free_context(output);
        avformat_input::Close(&input);
        RemoveOutputFile(output_path, output_created);
        return false;
    }
    bool success = true;
    auto write_normalized_packet = [&](AVPacket* output_packet, unsigned int source_stream_index, AVRational packet_time_base) {
        AVStream* destination_stream = output_streams[source_stream_index];
        VideoDtsRepairState* video_state = source_stream_index < video_dts_states.size() &&
                                                    video_dts_states[source_stream_index].enabled
                                                ? &video_dts_states[source_stream_index]
                                                : nullptr;
        if (video_state != nullptr && video_state->dts_shift > 0 && output_packet->dts != AV_NOPTS_VALUE) {
            if (output_packet->dts < std::numeric_limits<int64_t>::min() + video_state->dts_shift) {
                SetError(error, "video DTS underflows while applying the inferred decode-order offset in stream " +
                                    std::to_string(source_stream_index));
                av_packet_unref(output_packet);
                return false;
            }
            output_packet->dts -= video_state->dts_shift;
        }
        av_packet_rescale_ts(output_packet, packet_time_base, destination_stream->time_base);
        output_packet->stream_index = destination_stream->index;
        output_packet->pos = -1;
        if (video_state != nullptr) {
            if (output_packet->dts == AV_NOPTS_VALUE) {
                SetError(error, "video packet still has no DTS after leading-timestamp repair in stream " +
                                    std::to_string(source_stream_index));
                av_packet_unref(output_packet);
                return false;
            }
            if (video_state->last_written_dts != AV_NOPTS_VALUE &&
                output_packet->dts <= video_state->last_written_dts) {
                SetError(error, "video DTS is not strictly increasing after MP4 time-base conversion in stream " +
                                    std::to_string(source_stream_index) + ": " +
                                    std::to_string(video_state->last_written_dts) + " >= " +
                                    std::to_string(output_packet->dts));
                av_packet_unref(output_packet);
                return false;
            }
            if (video_state->dts_shift > 0 && output_packet->pts != AV_NOPTS_VALUE &&
                output_packet->dts > output_packet->pts) {
                SetError(error, "video DTS remains later than PTS after leading-timestamp repair in stream " +
                                    std::to_string(source_stream_index));
                av_packet_unref(output_packet);
                return false;
            }
        }
        const int64_t output_dts = output_packet->dts;
        status = av_interleaved_write_frame(output, output_packet);
        av_packet_unref(output_packet);
        if (status < 0) {
            SetError(error, "FFmpeg cannot write normalized MP4 packet for stream " + std::to_string(source_stream_index) + ": " +
                                AvError(status));
            return false;
        }
        if (video_state != nullptr)
            video_state->last_written_dts = output_dts;
        return true;
    };
    auto flush_leading_video_packets = [&](unsigned int source_stream_index, const AVPacket* first_valid_packet) {
        VideoDtsRepairState& video_state = video_dts_states[source_stream_index];
        const AVStream* source_stream = input->streams[source_stream_index];
        if (first_valid_packet == nullptr || first_valid_packet->dts == AV_NOPTS_VALUE) {
            SetError(error, "cannot infer missing leading video DTS without a valid timestamp anchor in stream " +
                                std::to_string(source_stream_index));
            return false;
        }
        int64_t next_dts = first_valid_packet->dts;
        AVRational frame_rate = source_stream->avg_frame_rate;
        if (frame_rate.num <= 0 || frame_rate.den <= 0)
            frame_rate = source_stream->r_frame_rate;
        bool use_frame_rate = !video_state.leading_packets_without_dts.empty() &&
                              frame_rate.num > 0 && frame_rate.den > 0;
        const AVRational frame_duration = use_frame_rate ? av_inv_q(frame_rate) : AVRational{};
        const int64_t pts_origin = video_state.leading_packets_without_dts.empty()
                                       ? first_valid_packet->pts
                                       : video_state.leading_packets_without_dts.front()->pts;
        if (pts_origin == AV_NOPTS_VALUE)
            use_frame_rate = false;
        int64_t frame_steps = 0;
        for (auto iterator = video_state.leading_packets_without_dts.rbegin();
             use_frame_rate && iterator != video_state.leading_packets_without_dts.rend(); ++iterator) {
            const int64_t current_offset = av_rescale_q(frame_steps + 1, frame_duration, source_stream->time_base);
            const int64_t previous_offset = av_rescale_q(frame_steps, frame_duration, source_stream->time_base);
            const int64_t expected_duration = current_offset - previous_offset;
            const int64_t packet_duration = (*iterator)->duration;
            if (expected_duration <= 0 ||
                (packet_duration > 0 && (packet_duration - expected_duration < -1 ||
                                         packet_duration - expected_duration > 1)) ||
                !IsTimestampOnFrameGrid((*iterator)->pts, pts_origin, source_stream->time_base, frame_duration)) {
                use_frame_rate = false;
                break;
            }
            ++frame_steps;
        }
        if (use_frame_rate &&
            !IsTimestampOnFrameGrid(first_valid_packet->pts, pts_origin, source_stream->time_base, frame_duration))
            use_frame_rate = false;
        frame_steps = 0;
        for (auto iterator = video_state.leading_packets_without_dts.rbegin();
             iterator != video_state.leading_packets_without_dts.rend(); ++iterator) {
            int64_t packet_duration = (*iterator)->duration;
            int64_t reconstructed_dts = AV_NOPTS_VALUE;
            if (use_frame_rate) {
                ++frame_steps;
                const int64_t frame_offset = av_rescale_q(frame_steps, frame_duration, source_stream->time_base);
                if (first_valid_packet->dts < std::numeric_limits<int64_t>::min() + frame_offset) {
                    SetError(error, "missing leading video DTS underflows the supported timestamp range in stream " +
                                        std::to_string(source_stream_index));
                    return false;
                }
                reconstructed_dts = first_valid_packet->dts - frame_offset;
            } else {
                if (packet_duration <= 0) {
                    AVRational fallback_rate = frame_rate;
                    if (fallback_rate.num > 0 && fallback_rate.den > 0)
                        packet_duration = av_rescale_q(1, av_inv_q(fallback_rate), source_stream->time_base);
                }
                if (packet_duration <= 0) {
                    SetError(error, "cannot infer missing leading video DTS in stream " +
                                        std::to_string(source_stream_index) + ": packet duration is unavailable");
                    return false;
                }
                if (next_dts < std::numeric_limits<int64_t>::min() + packet_duration) {
                    SetError(error, "missing leading video DTS underflows the supported timestamp range in stream " +
                                        std::to_string(source_stream_index));
                    return false;
                }
                reconstructed_dts = next_dts - packet_duration;
                next_dts = reconstructed_dts;
            }
            if ((*iterator)->pts == AV_NOPTS_VALUE) {
                SetError(error, "cannot infer missing leading video DTS in stream " +
                                    std::to_string(source_stream_index) + ": presentation timestamp is unavailable");
                return false;
            }
            if (reconstructed_dts == AV_NOPTS_VALUE) {
                SetError(error, "cannot represent reconstructed leading video DTS in stream " +
                                    std::to_string(source_stream_index));
                return false;
            }
            (*iterator)->dts = reconstructed_dts;
            if ((*iterator)->dts > (*iterator)->pts)
                video_state.dts_shift = std::max(video_state.dts_shift, (*iterator)->dts - (*iterator)->pts);
        }
        if (first_valid_packet->pts != AV_NOPTS_VALUE && first_valid_packet->dts > first_valid_packet->pts)
            video_state.dts_shift = std::max(video_state.dts_shift, first_valid_packet->dts - first_valid_packet->pts);
        if (use_frame_rate) {
            video_state.rebuild_dts_at_constant_frame_rate = true;
            video_state.cfr_anchor_dts = first_valid_packet->dts;
            video_state.cfr_next_frame_index = 1;
            video_state.cfr_pts_origin = pts_origin;
            video_state.cfr_frame_duration = frame_duration;
        }
        for (const OwnedPacket& pending_packet : video_state.leading_packets_without_dts) {
            if (!write_normalized_packet(pending_packet.get(), source_stream_index, source_stream->time_base))
                return false;
        }
        video_state.leading_packets_without_dts.clear();
        video_state.buffered_bytes = 0;
        return true;
    };
    auto prepare_cfr_video_packet = [&](unsigned int source_stream_index, AVPacket* video_packet) {
        VideoDtsRepairState& video_state = video_dts_states[source_stream_index];
        const AVStream* source_stream = input->streams[source_stream_index];
        const int64_t previous_offset = av_rescale_q(video_state.cfr_next_frame_index - 1,
                                                      video_state.cfr_frame_duration, source_stream->time_base);
        const int64_t current_offset = av_rescale_q(video_state.cfr_next_frame_index,
                                                     video_state.cfr_frame_duration, source_stream->time_base);
        const int64_t expected_duration = current_offset - previous_offset;
        if (video_packet->duration > 0 &&
            (video_packet->duration - expected_duration < -1 || video_packet->duration - expected_duration > 1)) {
            SetError(error, "video packet duration leaves the inferred constant-frame-rate timeline in stream " +
                                std::to_string(source_stream_index));
            return false;
        }
        if (current_offset > 0 && video_state.cfr_anchor_dts > std::numeric_limits<int64_t>::max() - current_offset) {
            SetError(error, "reconstructed constant-frame-rate DTS overflows in stream " +
                                std::to_string(source_stream_index));
            return false;
        }
        video_packet->dts = video_state.cfr_anchor_dts + current_offset;
        ++video_state.cfr_next_frame_index;
        return true;
    };
    auto drain_subtitle_filter = [&](unsigned int source_stream_index) {
        AVBSFContext* filter = subtitle_filters[source_stream_index].get();
        while (true) {
            status = av_bsf_receive_packet(filter, filtered_packet);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
                return true;
            if (status < 0) {
                SetError(error, "FFmpeg cannot convert normalized subtitle stream " + std::to_string(source_stream_index) + ": " +
                                    AvError(status));
                return false;
            }
            if (!write_normalized_packet(filtered_packet, source_stream_index, filter->time_base_out))
                return false;
        }
    };
    while ((status = av_read_frame(input, packet)) >= 0) {
        if (IsCancelled(cancellation_requested)) {
            SetError(error, "operation cancelled while normalizing MP4");
            success = false;
            av_packet_unref(packet);
            break;
        }
        if (packet->stream_index < 0 || static_cast<unsigned int>(packet->stream_index) >= output_streams.size()) {
            SetError(error, "input packet references an invalid stream during MP4 normalization");
            success = false;
            av_packet_unref(packet);
            break;
        }
        if (output_streams[packet->stream_index] == nullptr) {
            av_packet_unref(packet);
            continue;
        }
        if (subtitle_filters[packet->stream_index]) {
            const unsigned int source_stream_index = static_cast<unsigned int>(packet->stream_index);
            status = av_bsf_send_packet(subtitle_filters[source_stream_index].get(), packet);
            if (status < 0) {
                SetError(error, "FFmpeg cannot submit subtitle stream " + std::to_string(source_stream_index) +
                                    " for MP4 conversion: " + AvError(status));
                av_packet_unref(packet);
                success = false;
                break;
            }
            if (!drain_subtitle_filter(source_stream_index)) {
                success = false;
                break;
            }
            continue;
        }
        const AVStream* source_stream = input->streams[packet->stream_index];
        const int packet_stream_index = packet->stream_index;
        VideoDtsRepairState* video_state = video_dts_states[packet_stream_index].enabled
                                                ? &video_dts_states[packet_stream_index]
                                                : nullptr;
        bool cfr_anchor_packet = false;
        if (video_state != nullptr && !video_state->has_valid_dts) {
            if (packet->dts == AV_NOPTS_VALUE) {
                const size_t packet_bytes = packet->size > 0 ? static_cast<size_t>(packet->size) : 0;
                if (video_state->leading_packets_without_dts.size() >= kMaximumLeadingVideoPacketsWithoutDts ||
                    packet_bytes > kMaximumLeadingVideoPacketBytesWithoutDts - video_state->buffered_bytes) {
                    SetError(error, "cannot safely buffer leading video packets without DTS in stream " +
                                        std::to_string(packet_stream_index) + ": no valid DTS anchor was found within the recovery limit");
                    success = false;
                    av_packet_unref(packet);
                    break;
                }
                AVPacket* pending_packet = av_packet_alloc();
                if (pending_packet == nullptr) {
                    SetError(error, "cannot allocate packet while repairing leading video DTS");
                    success = false;
                    av_packet_unref(packet);
                    break;
                }
                av_packet_move_ref(pending_packet, packet);
                video_state->leading_packets_without_dts.emplace_back(pending_packet);
                video_state->buffered_bytes += packet_bytes;
                continue;
            }
            video_state->has_valid_dts = true;
            if (!flush_leading_video_packets(static_cast<unsigned int>(packet_stream_index), packet)) {
                success = false;
                av_packet_unref(packet);
                break;
            }
            cfr_anchor_packet = video_state->rebuild_dts_at_constant_frame_rate;
        } else if (video_state != nullptr && video_state->rebuild_dts_at_constant_frame_rate &&
                   video_state->cfr_anchor_written) {
            if (!prepare_cfr_video_packet(static_cast<unsigned int>(packet_stream_index), packet)) {
                success = false;
                av_packet_unref(packet);
                break;
            }
        }
        if (!write_normalized_packet(packet, static_cast<unsigned int>(packet_stream_index), source_stream->time_base)) {
            success = false;
            break;
        }
        if (cfr_anchor_packet)
            video_state->cfr_anchor_written = true;
        av_packet_unref(packet);
    }
    if (success && status != AVERROR_EOF) {
        SetError(error, "cannot read input while normalizing MP4: " + AvError(status));
        success = false;
    }
    if (success) {
        for (unsigned int index = 0; index < video_dts_states.size(); ++index) {
            VideoDtsRepairState& video_state = video_dts_states[index];
            if (!video_state.enabled || video_state.leading_packets_without_dts.empty())
                continue;
            const AVStream* source_stream = input->streams[index];
            if (source_stream->codecpar->video_delay != 0) {
                SetError(error, "video stream " + std::to_string(index) +
                                    " has leading packets without DTS and no later DTS anchor; cannot infer decode order safely");
                success = false;
                break;
            }
            for (const OwnedPacket& pending_packet : video_state.leading_packets_without_dts) {
                if (pending_packet->pts == AV_NOPTS_VALUE) {
                    SetError(error, "video stream " + std::to_string(index) +
                                        " has neither DTS nor PTS for a leading packet");
                    success = false;
                    break;
                }
                pending_packet->dts = pending_packet->pts;
            }
            if (!success)
                break;
            video_state.has_valid_dts = true;
            for (const OwnedPacket& pending_packet : video_state.leading_packets_without_dts) {
                if (!write_normalized_packet(pending_packet.get(), index, source_stream->time_base)) {
                    success = false;
                    break;
                }
            }
            video_state.leading_packets_without_dts.clear();
            video_state.buffered_bytes = 0;
            if (!success)
                break;
        }
    }
    if (success) {
        for (unsigned int index = 0; index < subtitle_filters.size(); ++index) {
            AVBSFContext* filter = subtitle_filters[index].get();
            if (filter == nullptr)
                continue;
            status = av_bsf_send_packet(filter, nullptr);
            if (status < 0 && status != AVERROR_EOF) {
                SetError(error, "FFmpeg cannot flush subtitle conversion for stream " + std::to_string(index) + ": " + AvError(status));
                success = false;
                break;
            }
            if (!drain_subtitle_filter(index)) {
                success = false;
                break;
            }
        }
    }
    av_packet_free(&filtered_packet);
    av_packet_free(&packet);
    if (success) {
        status = av_write_trailer(output);
        if (status < 0) {
            SetError(error, "FFmpeg cannot finalize normalized MP4: " + AvError(status));
            success = false;
        }
    }
    if (output->pb != nullptr && !(output->oformat->flags & AVFMT_NOFILE))
        avio_closep(&output->pb);
    avformat_free_context(output);
    avformat_input::Close(&input);
    if (!success)
        RemoveOutputFile(output_path, output_created);
    return success;
}

}  // namespace immersive_audio_muxer
