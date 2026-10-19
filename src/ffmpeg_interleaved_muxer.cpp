#include "ffmpeg_interleaved_muxer.h"

#include "avformat_input.h"
#include "ffmpeg_iamf_track_adapter.h"
#include "runtime_logging.h"
#include "standalone_iamf_reader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/iamf.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/timecode.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
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
    IAMF_MUX_LOG_ERROR("[FFmpegMuxer] {}", message);
}

bool IsCancelled(const FfmpegInterleavedMuxOptions& options) {
    return options.cancellation_requested && options.cancellation_requested();
}

int InterruptCallback(void* opaque) {
    const auto* callback = static_cast<const std::function<bool()>*>(opaque);
    return callback != nullptr && *callback && (*callback)() ? 1 : 0;
}

struct FormatContextDeleter {
    void operator()(AVFormatContext* context) const {
        avformat_input::Close(&context);
    }
};

struct OutputContextDeleter {
    void operator()(AVFormatContext* context) const {
        if (context == nullptr)
            return;
        if (context->pb != nullptr && context->oformat != nullptr &&
            (context->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&context->pb);
        }
        avformat_free_context(context);
    }
};

struct SyntheticContextDeleter {
    void operator()(AVFormatContext* context) const {
        if (context != nullptr)
            avformat_free_context(context);
    }
};

using InputContext = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using OutputContext = std::unique_ptr<AVFormatContext, OutputContextDeleter>;
using SyntheticInputContext = std::unique_ptr<AVFormatContext, SyntheticContextDeleter>;

SyntheticInputContext CreateStandaloneIamfInput(const StandaloneIamfReader& iamf, std::string* error) {
    const IamfIndexSummary& summary = iamf.summary();
    if (summary.sample_rate == 0 || summary.sample_rate > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        summary.frame_size > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        SetError(error, "standalone IAMF sample rate or frame size is outside FFmpeg limits");
        return {};
    }
    AVFormatContext* raw = avformat_alloc_context();
    if (raw == nullptr) {
        SetError(error, "cannot allocate standalone IAMF packet context");
        return {};
    }
    SyntheticInputContext input(raw);
    AVStream* stream = avformat_new_stream(raw, nullptr);
    if (stream == nullptr) {
        SetError(error, "cannot allocate standalone IAMF packet stream");
        return {};
    }
    stream->time_base = AVRational{1, static_cast<int>(summary.sample_rate)};
    stream->start_time = 0;
    stream->duration = summary.total_samples <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                           ? static_cast<int64_t>(summary.total_samples)
                           : AV_NOPTS_VALUE;
    stream->nb_frames = static_cast<int64_t>(iamf.temporal_units().size());
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->sample_rate = static_cast<int>(summary.sample_rate);
    stream->codecpar->frame_size = static_cast<int>(summary.frame_size);
    return input;
}

uint64_t EstimateStreamPacketCount(const AVStream* stream) {
    if (stream == nullptr)
        return 0;
    if (stream->nb_frames > 0)
        return static_cast<uint64_t>(stream->nb_frames);
    if (stream->duration <= 0 || stream->time_base.num <= 0 || stream->time_base.den <= 0 ||
        stream->codecpar == nullptr)
        return 0;

    const long double duration_seconds =
        static_cast<long double>(stream->duration) * stream->time_base.num / stream->time_base.den;
    long double packet_count = 0;
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        AVRational frame_rate = stream->avg_frame_rate;
        if (frame_rate.num <= 0 || frame_rate.den <= 0)
            frame_rate = stream->r_frame_rate;
        if (frame_rate.num > 0 && frame_rate.den > 0)
            packet_count = duration_seconds * frame_rate.num / frame_rate.den;
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
               stream->codecpar->sample_rate > 0 && stream->codecpar->frame_size > 0) {
        packet_count = duration_seconds * stream->codecpar->sample_rate / stream->codecpar->frame_size;
    }
    if (packet_count <= 0)
        return 0;
    if (packet_count >= static_cast<long double>(std::numeric_limits<uint64_t>::max()))
        return std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(std::ceil(packet_count));
}

uint64_t EstimateContextPacketCount(const AVFormatContext* context) {
    uint64_t count = 0;
    if (context == nullptr)
        return count;
    for (unsigned int index = 0; index < context->nb_streams; ++index) {
        const uint64_t stream_count = EstimateStreamPacketCount(context->streams[index]);
        if (stream_count > std::numeric_limits<uint64_t>::max() - count)
            return std::numeric_limits<uint64_t>::max();
        count += stream_count;
    }
    return count;
}

void LogOutputStreamParameters(const AVFormatContext* output,
                               uint64_t estimated_packet_count,
                               uint64_t moov_reservation) {
    if (output == nullptr)
        return;
    const char* format_name = output->oformat == nullptr || output->oformat->name == nullptr
                                  ? "unknown"
                                  : output->oformat->name;
    IAMF_MUX_LOG_INFO(
        "[FFmpegMuxer] preparing MP4 header: format={} output={} streams={} estimated_packets={} moov_size_reservation_bytes={}",
        format_name, output->url == nullptr ? "" : output->url,
        output->nb_streams, estimated_packet_count, moov_reservation);
    for (unsigned int index = 0; index < output->nb_streams; ++index) {
        const AVStream* stream = output->streams[index];
        if (stream == nullptr || stream->codecpar == nullptr) {
            IAMF_MUX_LOG_WARN("[FFmpegMuxer] output stream index={} has no codec parameters", index);
            continue;
        }
        const AVCodecParameters* parameters = stream->codecpar;
        const char* codec_name = avcodec_get_name(parameters->codec_id);
        IAMF_MUX_LOG_INFO(
            "[FFmpegMuxer] output stream index={} id={} type={} codec={} codec_id={} codec_tag=0x{:08x} time_base={}/{} bit_rate={} extradata_bytes={} disposition=0x{:x}",
            index, stream->id,
            av_get_media_type_string(parameters->codec_type) == nullptr
                ? "unknown"
                : av_get_media_type_string(parameters->codec_type),
            codec_name, static_cast<int>(parameters->codec_id), parameters->codec_tag,
            stream->time_base.num, stream->time_base.den, parameters->bit_rate,
            parameters->extradata_size, stream->disposition);
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            const char* pixel_format = av_get_pix_fmt_name(
                static_cast<AVPixelFormat>(parameters->format));
            IAMF_MUX_LOG_INFO(
                "[FFmpegMuxer] output video stream index={} size={}x{} format={} frame_rate={}/{}",
                index, parameters->width, parameters->height,
                pixel_format == nullptr
                    ? "unknown"
                    : pixel_format,
                stream->avg_frame_rate.num, stream->avg_frame_rate.den);
        } else if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            std::array<char, 128> channel_layout{};
            if (av_channel_layout_describe(&parameters->ch_layout, channel_layout.data(),
                                           channel_layout.size()) < 0)
                channel_layout[0] = '\0';
            IAMF_MUX_LOG_INFO(
                "[FFmpegMuxer] output audio stream index={} sample_rate={} channels={} channel_layout={} frame_size={} block_align={} bits_per_coded_sample={}",
                index, parameters->sample_rate, parameters->ch_layout.nb_channels,
                channel_layout[0] == '\0' ? "unknown" : channel_layout.data(),
                parameters->frame_size, parameters->block_align,
                parameters->bits_per_coded_sample);
        }
    }
}

bool WriteMp4HeaderWithFrontMoovReservation(AVFormatContext* output, uint64_t estimated_packet_count,
                                           std::string* error) {
    if (output == nullptr) {
        SetError(error, "cannot write MP4 header without an output context");
        return false;
    }
    constexpr uint64_t kMinimumMoovReservation = 1024ULL * 1024ULL;
    constexpr uint64_t kMoovBaseAllowance = 256ULL * 1024ULL;
    constexpr uint64_t kBytesPerEstimatedPacket = 32;
    constexpr uint64_t kMaximumReservation = static_cast<uint64_t>(std::numeric_limits<int>::max());
    if (estimated_packet_count > (kMaximumReservation - kMoovBaseAllowance) / kBytesPerEstimatedPacket) {
        SetError(error, "estimated MP4 index exceeds the supported front-moov reservation size");
        return false;
    }
    uint64_t reservation = std::max(kMinimumMoovReservation,
                                    kMoovBaseAllowance + estimated_packet_count * kBytesPerEstimatedPacket);
    reservation = (reservation + 7) & ~uint64_t{7};
    if (reservation > kMaximumReservation) {
        SetError(error, "estimated MP4 index exceeds the supported front-moov reservation size");
        return false;
    }

    const std::string reservation_text = std::to_string(reservation);
    AVDictionary* muxer_options = nullptr;
    const int option_status = av_dict_set(&muxer_options, "moov_size", reservation_text.c_str(), 0);
    if (option_status < 0) {
        SetError(error, "cannot reserve front-of-file MP4 index space: " + AvError(option_status));
        av_dict_free(&muxer_options);
        return false;
    }
    LogOutputStreamParameters(output, estimated_packet_count, reservation);
    const int status = avformat_write_header(output, &muxer_options);
    const bool reservation_was_ignored = av_dict_get(muxer_options, "moov_size", nullptr, 0) != nullptr;
    av_dict_free(&muxer_options);
    if (status < 0) {
        IAMF_MUX_LOG_ERROR(
            "[FFmpegMuxer] avformat_write_header failed: ffmpeg_status={} ffmpeg_error={} estimated_packets={} moov_size_reservation_bytes={} reservation_option_ignored={}",
            status, AvError(status), estimated_packet_count, reservation,
            reservation_was_ignored);
        SetError(error, "FFmpeg cannot write fast-start MP4 header: " + AvError(status));
        return false;
    }
    if (reservation_was_ignored) {
        IAMF_MUX_LOG_ERROR(
            "[FFmpegMuxer] MP4 muxer ignored moov_size reservation: estimated_packets={} moov_size_reservation_bytes={}",
            estimated_packet_count, reservation);
        SetError(error, "the FFmpeg MP4 muxer did not accept the front-moov reservation option");
        return false;
    }
    return true;
}

struct InterleavedIamfTrack {
    const StandaloneIamfReader* reader = nullptr;
    uint32_t placeholder_track_id = 0;
    int output_stream_index = -1;
    SyntheticInputContext input;
};

InputContext OpenInput(const std::string& path, const std::function<bool()>& cancellation_requested,
                       std::string* error) {
    if (path.empty()) {
        SetError(error, "FFmpeg interleaved mux input path is empty");
        return {};
    }
    AVFormatContext* raw = nullptr;
    const int status = avformat_input::Open(&raw, path, cancellation_requested);
    if (status < 0) {
        SetError(error, "cannot open or read streams from input " + path + ": " + AvError(status));
        return {};
    }
    return InputContext(raw);
}

bool CopyChapter(const AVChapter* source, AVFormatContext* output, std::string* error) {
    if (source == nullptr || output == nullptr)
        return false;
    AVChapter* chapter = static_cast<AVChapter*>(av_mallocz(sizeof(*chapter)));
    if (chapter == nullptr) {
        SetError(error, "cannot allocate output chapter");
        return false;
    }
    chapter->id = source->id;
    chapter->time_base = source->time_base;
    chapter->start = source->start;
    chapter->end = source->end;
    if (av_dict_copy(&chapter->metadata, source->metadata, 0) < 0) {
        av_dict_free(&chapter->metadata);
        av_free(chapter);
        SetError(error, "cannot copy chapter metadata");
        return false;
    }
    AVChapter** chapters = static_cast<AVChapter**>(
        av_realloc_array(output->chapters, output->nb_chapters + 1, sizeof(*chapters)));
    if (chapters == nullptr) {
        av_dict_free(&chapter->metadata);
        av_free(chapter);
        SetError(error, "cannot grow output chapter table");
        return false;
    }
    output->chapters = chapters;
    output->chapters[output->nb_chapters++] = chapter;
    return true;
}

bool CopyFormatInfo(const AVFormatContext* source, const AVFormatContext* iamf, AVFormatContext* output,
                    std::string* error) {
    if (source == nullptr || output == nullptr)
        return false;
    if (av_dict_copy(&output->metadata, source->metadata, 0) < 0) {
        SetError(error, "cannot copy source format metadata");
        return false;
    }
    if (iamf != nullptr && av_dict_copy(&output->metadata, iamf->metadata, AV_DICT_DONT_OVERWRITE) < 0) {
        SetError(error, "cannot merge IAMF format metadata");
        return false;
    }
    for (unsigned int index = 0; index < source->nb_chapters; ++index) {
        if (!CopyChapter(source->chapters[index], output, error))
            return false;
    }
    return true;
}

bool IsStaticAttachedPicture(const AVStream* stream) {
    return stream != nullptr && (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0 &&
           (stream->disposition & AV_DISPOSITION_TIMED_THUMBNAILS) == 0;
}

bool IsMovTimecodeStream(const AVStream* stream) {
    return stream != nullptr && stream->codecpar != nullptr && stream->codecpar->codec_type == AVMEDIA_TYPE_DATA &&
           stream->codecpar->codec_tag == MKTAG('t', 'm', 'c', 'd');
}

bool ReadAttachedPictureDimensions(const AVStream* source, int* width, int* height) {
    if (source == nullptr || source->codecpar == nullptr || source->attached_pic.data == nullptr ||
        source->attached_pic.size <= 0 || width == nullptr || height == nullptr)
        return false;
    const uint8_t* const data = source->attached_pic.data;
    const size_t size = static_cast<size_t>(source->attached_pic.size);
    if (source->codecpar->codec_id == AV_CODEC_ID_PNG && size >= 24 &&
        std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0 && std::memcmp(data + 12, "IHDR", 4) == 0) {
        const uint32_t parsed_width = AV_RB32(data + 16);
        const uint32_t parsed_height = AV_RB32(data + 20);
        if (parsed_width != 0 && parsed_height != 0 &&
            parsed_width <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
            parsed_height <= static_cast<uint32_t>(std::numeric_limits<int>::max())) {
            *width = static_cast<int>(parsed_width);
            *height = static_cast<int>(parsed_height);
            return true;
        }
        return false;
    }
    if (source->codecpar->codec_id != AV_CODEC_ID_MJPEG || size < 4 || data[0] != 0xff || data[1] != 0xd8)
        return false;

    size_t offset = 2;
    while (offset + 1 < size) {
        if (data[offset] != 0xff) {
            ++offset;
            continue;
        }
        while (offset < size && data[offset] == 0xff)
            ++offset;
        if (offset >= size)
            return false;
        const uint8_t marker = data[offset++];
        if (marker == 0xd9 || marker == 0xda)
            return false;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
            continue;
        if (offset + 2 > size)
            return false;
        const uint16_t segment_size = static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
                                                            data[offset + 1]);
        if (segment_size < 2 || segment_size > size - offset)
            return false;
        const bool is_start_of_frame = (marker >= 0xc0 && marker <= 0xc3) ||
                                       (marker >= 0xc5 && marker <= 0xc7) ||
                                       (marker >= 0xc9 && marker <= 0xcb) ||
                                       (marker >= 0xcd && marker <= 0xcf);
        if (is_start_of_frame && segment_size >= 7) {
            const uint16_t parsed_height = static_cast<uint16_t>((static_cast<uint16_t>(data[offset + 3]) << 8) |
                                                                 data[offset + 4]);
            const uint16_t parsed_width = static_cast<uint16_t>((static_cast<uint16_t>(data[offset + 5]) << 8) |
                                                                data[offset + 6]);
            if (parsed_width != 0 && parsed_height != 0) {
                *width = parsed_width;
                *height = parsed_height;
                return true;
            }
            return false;
        }
        offset += segment_size;
    }
    return false;
}

bool ShouldCopySourceStream(const AVStream* stream, bool keep_source_audio,
                            bool route_source_iamf_tracks_through_probe) {
    if (stream == nullptr || stream->codecpar == nullptr)
        return false;
    switch (stream->codecpar->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            // Preserve all video tracks in Preserve Source mode. This includes
            // timed thumbnails and static attached pictures.
            return true;
        case AVMEDIA_TYPE_AUDIO:
            if (route_source_iamf_tracks_through_probe &&
                stream->codecpar->codec_tag == MKTAG('i', 'a', 'm', 'f'))
                return false;
            return keep_source_audio;
        case AVMEDIA_TYPE_ATTACHMENT:
            // MP4 has no generic attachment track. Static cover art is carried
            // by a video AVStream with attached_pic disposition and is kept.
            return false;
        case AVMEDIA_TYPE_DATA:
            // Packetized data, including QuickTime tmcd, is scheduled with the
            // same timestamp merge as video, audio, and subtitles.
            return true;
        default:
            return true;
    }
}

bool CopyStreamParameters(const AVStream* source, AVStream* output, std::string* error) {
    if (source == nullptr || source->codecpar == nullptr || output == nullptr || output->codecpar == nullptr) {
        SetError(error, "invalid source or output stream while copying parameters");
        return false;
    }
    int status = avcodec_parameters_copy(output->codecpar, source->codecpar);
    if (status < 0) {
        SetError(error, "cannot copy codec parameters for stream " + std::to_string(source->index) + ": " +
                           AvError(status));
        return false;
    }
    // MOV demuxers can expose tmcd as an untyped data stream (codec_id NONE).
    // The MP4 muxer needs BIN_DATA so it can validate the patched tmcd tag and
    // write a proper QuickTime timecode sample entry.
    if (IsMovTimecodeStream(source))
        output->codecpar->codec_id = AV_CODEC_ID_BIN_DATA;
    if (IsStaticAttachedPicture(source) &&
        (output->codecpar->width <= 0 || output->codecpar->height <= 0)) {
        int width = 0;
        int height = 0;
        if (ReadAttachedPictureDimensions(source, &width, &height)) {
            output->codecpar->width = width;
            output->codecpar->height = height;
        }
    }
    // Preserve a source sample-entry tag when it is present.  In particular,
    // hvc1 versus hev1 is a meaningful compatibility choice for HEVC players,
    // and the source MP4 has already proved that the tag matches its codec.
    // Keep data tags untouched; custom MP4 tag support lets raw QuickTime
    // timecode pass through the normal packet scheduler.
    output->time_base = source->time_base;
    output->start_time = source->start_time;
    output->duration = source->duration;
    output->nb_frames = source->nb_frames;
    output->disposition = source->disposition;
    output->discard = source->discard;
    output->sample_aspect_ratio = source->sample_aspect_ratio;
    output->avg_frame_rate = source->avg_frame_rate;
    output->r_frame_rate = source->r_frame_rate;
    output->pts_wrap_bits = source->pts_wrap_bits;
    // Assign output track IDs in output order. Source IDs can be sparse or
    // collide with added IAMF tracks, and movenc may renumber them when it
    // builds the MP4 track table.
    output->id = output->index + 1;
    if (av_dict_copy(&output->metadata, source->metadata, 0) < 0) {
        SetError(error, "cannot copy metadata for stream " + std::to_string(source->index));
        return false;
    }
    if (IsStaticAttachedPicture(source)) {
        status = av_packet_ref(&output->attached_pic, &source->attached_pic);
        if (status < 0) {
            SetError(error, "cannot copy attached picture for stream " + std::to_string(source->index) + ": " +
                               AvError(status));
            return false;
        }
    }
    return true;
}

struct StreamMap {
    std::vector<int> source_to_output;
    std::vector<int> iamf_to_output;
};

bool BuildSourceStreams(const AVFormatContext* source, AVFormatContext* output,
                        const FfmpegInterleavedMuxOptions& options, StreamMap* map, std::string* error) {
    if (source == nullptr || output == nullptr || map == nullptr)
        return false;
    if (options.requested_video_stream_index < 0 ||
        options.requested_video_stream_index >= static_cast<int>(source->nb_streams) ||
        source->streams[options.requested_video_stream_index] == nullptr ||
        source->streams[options.requested_video_stream_index]->codecpar == nullptr ||
        source->streams[options.requested_video_stream_index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        SetError(error, "requested video stream index is not a video stream");
        return false;
    }
    map->source_to_output.assign(source->nb_streams, -1);
    for (unsigned int index = 0; index < source->nb_streams; ++index) {
        const AVStream* input_stream = source->streams[index];
        if (!ShouldCopySourceStream(input_stream, options.keep_source_audio,
                                    options.route_source_iamf_tracks_through_probe))
            continue;
        AVStream* output_stream = avformat_new_stream(output, nullptr);
        if (output_stream == nullptr) {
            SetError(error, "cannot allocate output stream for source stream " + std::to_string(index));
            return false;
        }
        if (!CopyStreamParameters(input_stream, output_stream, error))
            return false;
        map->source_to_output[index] = output_stream->index;
    }
    return true;
}

bool ResolveOutputVideoTrackId(const AVFormatContext* output, const StreamMap& map,
                               int source_stream_index, uint32_t* track_id,
                               std::string* error) {
    if (output == nullptr || track_id == nullptr || source_stream_index < 0 ||
        source_stream_index >= static_cast<int>(map.source_to_output.size())) {
        SetError(error, "cannot resolve selected output video track");
        return false;
    }
    const int output_index = map.source_to_output[source_stream_index];
    if (output_index < 0 || output_index >= static_cast<int>(output->nb_streams) ||
        output->streams[output_index] == nullptr || output->streams[output_index]->id <= 0) {
        SetError(error, "selected video stream was not copied to the MP4 output");
        return false;
    }
    *track_id = static_cast<uint32_t>(output->streams[output_index]->id);
    return true;
}

bool CopyPrograms(const AVFormatContext* source, AVFormatContext* output, const std::vector<int>& stream_map,
                  std::string* error) {
    if (source == nullptr || output == nullptr)
        return false;
    for (unsigned int index = 0; index < source->nb_programs; ++index) {
        const AVProgram* source_program = source->programs[index];
        if (source_program == nullptr)
            continue;
        std::vector<unsigned int> mapped_streams;
        mapped_streams.reserve(source_program->nb_stream_indexes);
        for (unsigned int stream_index = 0; stream_index < source_program->nb_stream_indexes; ++stream_index) {
            const unsigned int source_index = source_program->stream_index[stream_index];
            if (source_index < stream_map.size() && stream_map[source_index] >= 0)
                mapped_streams.push_back(static_cast<unsigned int>(stream_map[source_index]));
        }
        if (mapped_streams.empty())
            continue;
        AVProgram* output_program = av_new_program(output, source_program->id);
        if (output_program == nullptr) {
            SetError(error, "cannot allocate output program " + std::to_string(source_program->id));
            return false;
        }
        output_program->flags = source_program->flags;
        output_program->discard = source_program->discard;
        output_program->program_num = source_program->program_num;
        output_program->pmt_pid = source_program->pmt_pid;
        output_program->pcr_pid = source_program->pcr_pid;
        output_program->pmt_version = source_program->pmt_version;
        if (av_dict_copy(&output_program->metadata, source_program->metadata, 0) < 0) {
            SetError(error, "cannot copy metadata for output program " + std::to_string(source_program->id));
            return false;
        }
        for (unsigned int output_stream_index : mapped_streams) {
            if (av_program_add_stream_index2(output, output_program->id, output_stream_index) < 0) {
                SetError(error, "cannot add stream to output program " + std::to_string(source_program->id));
                return false;
            }
        }
    }
    return true;
}

bool ReplaceGroupParamsWithInput(AVStreamGroup* output_group, AVStreamGroup* input_group, std::string* error) {
    if (output_group == nullptr || input_group == nullptr || output_group->type != input_group->type) {
        SetError(error, "source and output stream group types do not match");
        return false;
    }
    switch (output_group->type) {
        case AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT:
            av_iamf_audio_element_free(&output_group->params.iamf_audio_element);
            output_group->params.iamf_audio_element = input_group->params.iamf_audio_element;
            input_group->params.iamf_audio_element = nullptr;
            break;
        case AV_STREAM_GROUP_PARAMS_IAMF_MIX_PRESENTATION:
            av_iamf_mix_presentation_free(&output_group->params.iamf_mix_presentation);
            output_group->params.iamf_mix_presentation = input_group->params.iamf_mix_presentation;
            input_group->params.iamf_mix_presentation = nullptr;
            break;
        case AV_STREAM_GROUP_PARAMS_TILE_GRID:
            av_free(output_group->params.tile_grid);
            output_group->params.tile_grid = input_group->params.tile_grid;
            input_group->params.tile_grid = nullptr;
            break;
        case AV_STREAM_GROUP_PARAMS_TREF:
            av_free(output_group->params.tref);
            output_group->params.tref = input_group->params.tref;
            input_group->params.tref = nullptr;
            break;
        case AV_STREAM_GROUP_PARAMS_LCEVC:
        case AV_STREAM_GROUP_PARAMS_DOLBY_VISION:
            av_free(output_group->params.layered_video);
            output_group->params.layered_video = input_group->params.layered_video;
            input_group->params.layered_video = nullptr;
            break;
        default:
            SetError(error, "unsupported FFmpeg stream group type");
            return false;
    }
    return true;
}

bool CopyStreamGroups(const AVFormatContext* input_const, AVFormatContext* output, const std::vector<int>& input_to_output,
                      bool iamf_only, std::string* error,
                      bool skip_unmapped_iamf_groups = false) {
    // The parameter objects are owned by the input context. We transfer them
    // to the output group and null the input pointer, so both contexts remain
    // independently destructible without a deep, ABI-sensitive clone.
    AVFormatContext* input = const_cast<AVFormatContext*>(input_const);
    for (unsigned int index = 0; index < input->nb_stream_groups; ++index) {
        AVStreamGroup* input_group = input->stream_groups[index];
        if (input_group == nullptr)
            continue;
        const bool is_iamf_group = input_group->type == AV_STREAM_GROUP_PARAMS_IAMF_AUDIO_ELEMENT ||
                                   input_group->type == AV_STREAM_GROUP_PARAMS_IAMF_MIX_PRESENTATION;
        if (iamf_only && !is_iamf_group) {
            SetError(error, "source contains a non-IAMF stream group that cannot be combined with an IAMF MP4 track");
            return false;
        }
        bool has_unmapped_stream = false;
        for (unsigned int stream_index = 0; stream_index < input_group->nb_streams; ++stream_index) {
            const AVStream* input_stream = input_group->streams[stream_index];
            if (input_stream == nullptr || input_stream->index < 0 ||
                static_cast<size_t>(input_stream->index) >= input_to_output.size() ||
                input_to_output[input_stream->index] < 0) {
                has_unmapped_stream = true;
                break;
            }
        }
        if (has_unmapped_stream) {
            if (is_iamf_group) {
                if (skip_unmapped_iamf_groups)
                    continue;
                SetError(error, "IAMF stream group references a stream that is not kept");
                return false;
            }
            // Keep FFmpeg's normal -map behavior for relation groups: a group
            // whose member was intentionally excluded is omitted as a whole.
            continue;
        }
        AVStreamGroup* output_group = avformat_stream_group_create(output, input_group->type, nullptr);
        if (output_group == nullptr) {
            SetError(error, "cannot create output stream group " + std::to_string(index));
            return false;
        }
        output_group->id = input_group->id;
        output_group->disposition = input_group->disposition;
        if (av_dict_copy(&output_group->metadata, input_group->metadata, 0) < 0) {
            SetError(error, "cannot copy stream group metadata " + std::to_string(index));
            return false;
        }
        if (!ReplaceGroupParamsWithInput(output_group, input_group, error))
            return false;
        for (unsigned int stream_index = 0; stream_index < input_group->nb_streams; ++stream_index) {
            const AVStream* input_stream = input_group->streams[stream_index];
            if (avformat_stream_group_add_stream(output_group, output->streams[input_to_output[input_stream->index]]) < 0) {
                SetError(error, "cannot add stream to output stream group " + std::to_string(index));
                return false;
            }
        }
    }
    return true;
}

struct PendingTimecodeFlags {
    int output_stream_index = -1;
    uint32_t flags = 0;
};

struct SyntheticTimecodeTrack {
    SyntheticInputContext input;
    int output_stream_index = -1;
    std::vector<uint8_t> packet;
    int64_t pts = 0;
    int64_t duration = 0;
    uint32_t flags = 0;
};

uint32_t ReadTimecodeFlags(const AVStream* stream) {
    if (stream == nullptr || stream->codecpar == nullptr || stream->codecpar->extradata == nullptr ||
        stream->codecpar->extradata_size < 8)
        return 0;
    return AV_RB32(stream->codecpar->extradata + 4);
}

SyntheticInputContext CreateTimecodeInput(AVRational time_base, std::string* error) {
    AVFormatContext* raw = avformat_alloc_context();
    if (raw == nullptr) {
        SetError(error, "cannot allocate packet context for the timecode track");
        return {};
    }
    SyntheticInputContext input(raw);
    AVStream* stream = avformat_new_stream(raw, nullptr);
    if (stream == nullptr) {
        SetError(error, "cannot allocate packet stream for the timecode track");
        return {};
    }
    stream->time_base = time_base;
    stream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    stream->codecpar->codec_id = AV_CODEC_ID_BIN_DATA;
    stream->codecpar->codec_tag = MKTAG('t', 'm', 'c', 'd');
    return input;
}

bool AddTimecodeMetadataTracks(const AVFormatContext* source, AVFormatContext* output,
                               const std::vector<int>& source_to_output,
                               std::vector<SyntheticTimecodeTrack>* synthetic_tracks,
                               std::vector<PendingTimecodeFlags>* timecode_flags,
                               std::string* error) {
    if (source == nullptr || output == nullptr || synthetic_tracks == nullptr || timecode_flags == nullptr)
        return false;

    bool has_source_tmcd_stream = false;
    for (unsigned int index = 0; index < source->nb_streams; ++index) {
        const AVStream* source_stream = source->streams[index];
        if (!IsMovTimecodeStream(source_stream))
            continue;
        if (index >= source_to_output.size() || source_to_output[index] < 0)
            continue;
        // A zero-duration tmcd stream has no media sample for movenc to
        // describe. Keep it out of flag patching and allow real video
        // metadata to create a usable timecode track when present.
        if (source_stream->duration == 0)
            continue;
        has_source_tmcd_stream = true;
        timecode_flags->push_back({source_to_output[index], ReadTimecodeFlags(source_stream)});
    }
    if (has_source_tmcd_stream)
        return true;

    const AVDictionaryEntry* global_timecode = av_dict_get(source->metadata, "timecode", nullptr, 0);
    for (unsigned int index = 0; index < source->nb_streams; ++index) {
        const AVStream* video = source->streams[index];
        if (video == nullptr || video->codecpar == nullptr || video->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
            IsStaticAttachedPicture(video) ||
            index >= source_to_output.size() || source_to_output[index] < 0)
            continue;
        const AVDictionaryEntry* timecode = global_timecode;
        if (timecode == nullptr)
            timecode = av_dict_get(video->metadata, "timecode", nullptr, 0);
        if (timecode == nullptr || timecode->value == nullptr || timecode->value[0] == '\0')
            continue;

        AVRational frame_rate = video->avg_frame_rate;
        if (frame_rate.num <= 0 || frame_rate.den <= 0)
            frame_rate = video->r_frame_rate;
        if (frame_rate.num <= 0 || frame_rate.den <= 0) {
            SetError(error, "video timecode is present but its frame rate is unavailable");
            return false;
        }
        AVTimecode parsed_timecode{};
        const int parse_status = av_timecode_init_from_string(&parsed_timecode, frame_rate,
                                                               timecode->value,
                                                               const_cast<AVFormatContext*>(source));
        if (parse_status < 0) {
            SetError(error, "cannot parse source video timecode: " + std::string(timecode->value));
            return false;
        }

        AVStream* output_video = output->streams[source_to_output[index]];
        AVStream* output_timecode = avformat_new_stream(output, nullptr);
        if (output_video == nullptr || output_timecode == nullptr) {
            SetError(error, "cannot allocate output QuickTime timecode track");
            return false;
        }
        uint32_t max_track_id = 0;
        for (unsigned int output_index = 0; output_index < output->nb_streams - 1; ++output_index) {
            const AVStream* existing = output->streams[output_index];
            if (existing != nullptr && existing->id > 0)
                max_track_id = std::max<uint32_t>(max_track_id, static_cast<uint32_t>(existing->id));
        }
        if (max_track_id >= 0x7ffffffeU) {
            SetError(error, "no track ID is available for the QuickTime timecode track");
            return false;
        }
        output_timecode->id = static_cast<int>(max_track_id + 1);
        output_timecode->time_base = av_inv_q(frame_rate);
        output_timecode->start_time = parsed_timecode.start;
        output_timecode->avg_frame_rate = frame_rate;
        output_timecode->r_frame_rate = frame_rate;
        output_timecode->nb_frames = 1;
        output_timecode->disposition = 0;
        output_timecode->codecpar->codec_type = AVMEDIA_TYPE_DATA;
        output_timecode->codecpar->codec_id = AV_CODEC_ID_BIN_DATA;
        output_timecode->codecpar->codec_tag = MKTAG('t', 'm', 'c', 'd');
        av_dict_set(&output_timecode->metadata, "handler_name", "Timecode placeholder", 0);
        if (video->duration > 0 && video->time_base.num > 0 && video->time_base.den > 0)
            output_timecode->duration = av_rescale_q(video->duration, video->time_base,
                                                      output_timecode->time_base);
        else if (video->nb_frames > 0)
            output_timecode->duration = video->nb_frames;

        AVStreamGroup* reference = avformat_stream_group_create(output, AV_STREAM_GROUP_PARAMS_TREF, nullptr);
        if (reference == nullptr || reference->params.tref == nullptr ||
            avformat_stream_group_add_stream(reference, output_video) < 0 ||
            avformat_stream_group_add_stream(reference, output_timecode) < 0) {
            SetError(error, "cannot link the QuickTime timecode track to its video track");
            return false;
        }
        reference->id = static_cast<unsigned int>(output_timecode->id);
        reference->params.tref->metadata_index = reference->nb_streams - 1;

        SyntheticTimecodeTrack track;
        track.input = CreateTimecodeInput(output_timecode->time_base, error);
        if (!track.input)
            return false;
        track.output_stream_index = output_timecode->index;
        track.pts = parsed_timecode.start;
        track.duration = output_timecode->duration;
        track.flags = (parsed_timecode.flags & AV_TIMECODE_FLAG_DROPFRAME) != 0 ? 1U : 0U;
        track.packet.resize(4);
        AV_WB32(track.packet.data(), static_cast<uint32_t>(parsed_timecode.start));
        synthetic_tracks->push_back(std::move(track));
        timecode_flags->push_back({output_timecode->index, synthetic_tracks->back().flags});
    }
    return true;
}

// A cursor is deliberately restricted to one input stream.  A single
// av_read_frame() cursor preserves the demuxer's physical order, which is not
// sufficient when an input MP4 has one track stored in a late mdat range.  We
// therefore open one lightweight demux cursor per mapped stream and merge the
// pending packets below by timestamp.  At most one packet per active stream is
// held by the scheduler; payloads are never decoded or accumulated wholesale.
struct PacketCursor {
    AVFormatContext* input = nullptr;
    int input_stream_index = -1;
    int output_stream_index = -1;
    bool eof = false;
    AVPacket* packet = nullptr;
    uint64_t sequence = 0;
    const StandaloneIamfReader* standalone_iamf = nullptr;
    size_t standalone_iamf_unit_index = 0;
    std::vector<uint8_t> synthetic_packet;
    int64_t synthetic_pts = AV_NOPTS_VALUE;
    int64_t synthetic_duration = 0;
    bool synthetic_packet_sent = false;
    const AVPacket* attached_picture = nullptr;
};

void ClearCursor(PacketCursor* cursor) {
    if (cursor == nullptr || cursor->packet == nullptr)
        return;
    av_packet_unref(cursor->packet);
}

bool ReadNextPacket(PacketCursor* cursor, std::string* error) {
    if (cursor == nullptr || cursor->input == nullptr || cursor->input_stream_index < 0 || cursor->packet == nullptr)
        return false;
    ClearCursor(cursor);
    if (cursor->standalone_iamf != nullptr) {
        const auto& units = cursor->standalone_iamf->temporal_units();
        if (cursor->standalone_iamf_unit_index >= units.size()) {
            cursor->eof = true;
            return true;
        }
        const IamfIndexEntry& sample = units[cursor->standalone_iamf_unit_index].index;
        std::vector<uint8_t> bytes;
        if (!cursor->standalone_iamf->ReadTemporalUnit(cursor->standalone_iamf_unit_index, &bytes, error))
            return false;
        if (bytes.empty() || bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            SetError(error, "standalone IAMF temporal unit has an invalid packet size");
            return false;
        }
        const int status = av_new_packet(cursor->packet, static_cast<int>(bytes.size()));
        if (status < 0) {
            SetError(error, "cannot allocate standalone IAMF packet: " + AvError(status));
            return false;
        }
        std::memcpy(cursor->packet->data, bytes.data(), bytes.size());
        cursor->packet->stream_index = cursor->input_stream_index;
        cursor->packet->pts = static_cast<int64_t>(sample.start_timestamp);
        cursor->packet->dts = static_cast<int64_t>(sample.start_timestamp);
        cursor->packet->duration = static_cast<int64_t>(sample.num_samples);
        cursor->packet->flags |= AV_PKT_FLAG_KEY;
        ++cursor->standalone_iamf_unit_index;
        ++cursor->sequence;
        return true;
    }
    if (cursor->attached_picture != nullptr) {
        if (cursor->synthetic_packet_sent) {
            cursor->eof = true;
            return true;
        }
        const int status = av_packet_ref(cursor->packet, cursor->attached_picture);
        if (status < 0) {
            SetError(error, "cannot reference attached picture packet: " + AvError(status));
            return false;
        }
        cursor->packet->stream_index = cursor->input_stream_index;
        cursor->packet->pts = cursor->packet->dts = 0;
        cursor->packet->duration = 0;
        cursor->packet->flags |= AV_PKT_FLAG_KEY;
        cursor->synthetic_packet_sent = true;
        ++cursor->sequence;
        return true;
    }
    if (!cursor->synthetic_packet.empty()) {
        if (cursor->synthetic_packet_sent) {
            cursor->eof = true;
            return true;
        }
        if (cursor->synthetic_packet.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            SetError(error, "synthetic timecode packet is too large");
            return false;
        }
        const int status = av_new_packet(cursor->packet, static_cast<int>(cursor->synthetic_packet.size()));
        if (status < 0) {
            SetError(error, "cannot allocate synthetic timecode packet: " + AvError(status));
            return false;
        }
        std::memcpy(cursor->packet->data, cursor->synthetic_packet.data(), cursor->synthetic_packet.size());
        cursor->packet->stream_index = cursor->input_stream_index;
        cursor->packet->pts = cursor->synthetic_pts;
        cursor->packet->dts = cursor->synthetic_pts;
        cursor->packet->duration = cursor->synthetic_duration;
        cursor->packet->flags |= AV_PKT_FLAG_KEY;
        cursor->synthetic_packet_sent = true;
        ++cursor->sequence;
        return true;
    }
    while (!cursor->eof) {
        const int status = av_read_frame(cursor->input, cursor->packet);
        if (status == AVERROR_EOF) {
            cursor->eof = true;
            return true;
        }
        if (status < 0) {
            SetError(error, "cannot read interleaved input packet: " + AvError(status));
            return false;
        }
        if (cursor->packet->stream_index != cursor->input_stream_index) {
            av_packet_unref(cursor->packet);
            continue;
        }
        const AVStream* source_stream = cursor->input->streams[cursor->input_stream_index];
        if (source_stream == nullptr || IsStaticAttachedPicture(source_stream)) {
            av_packet_unref(cursor->packet);
            continue;
        }
        if (cursor->packet->size < 0 || cursor->packet->duration < 0) {
            av_packet_unref(cursor->packet);
            SetError(error, "input packet has invalid size or duration");
            return false;
        }
        ++cursor->sequence;
        return true;
    }
    return true;
}

int64_t PacketOrderTimestamp(const PacketCursor& cursor) {
    if (cursor.packet == nullptr)
        return std::numeric_limits<int64_t>::max();
    int64_t timestamp = cursor.packet->dts;
    if (timestamp == AV_NOPTS_VALUE)
        timestamp = cursor.packet->pts;
    if (timestamp == AV_NOPTS_VALUE)
        return std::numeric_limits<int64_t>::max();
    const AVStream* stream = cursor.input->streams[cursor.input_stream_index];
    if (stream == nullptr)
        return std::numeric_limits<int64_t>::max();
    return av_rescale_q(timestamp, stream->time_base, AV_TIME_BASE_Q);
}

bool CursorComesFirst(const PacketCursor& left, const PacketCursor& right) {
    const int64_t left_timestamp = PacketOrderTimestamp(left);
    const int64_t right_timestamp = PacketOrderTimestamp(right);
    if (left_timestamp != right_timestamp)
        return left_timestamp < right_timestamp;
    if (left.sequence != right.sequence)
        return left.sequence < right.sequence;
    return left.output_stream_index < right.output_stream_index;
}

bool BuildPacketCursors(const std::string& path, const AVFormatContext* master_input,
                        const std::vector<int>& stream_map, AVFormatContext* output,
                        const std::function<bool()>& cancellation_requested,
                        std::vector<InputContext>* reader_inputs, std::vector<PacketCursor>* cursors,
                        std::vector<AVPacket*>* packets, std::string* error) {
    if (path.empty() || master_input == nullptr || output == nullptr || reader_inputs == nullptr || cursors == nullptr ||
        packets == nullptr)
        return false;
    if (stream_map.size() < master_input->nb_streams)
        return false;
    for (unsigned int index = 0; index < master_input->nb_streams; ++index) {
        const AVStream* input_stream = master_input->streams[index];
        const int output_index = stream_map[index];
        if (output_index < 0 || input_stream == nullptr || input_stream->codecpar == nullptr ||
            output_index >= static_cast<int>(output->nb_streams))
            continue;
        if (IsStaticAttachedPicture(input_stream)) {
            if (input_stream->attached_pic.size <= 0)
                continue;
            AVPacket* packet = av_packet_alloc();
            if (packet == nullptr) {
                SetError(error, "cannot allocate attached-picture interleave packet");
                return false;
            }
            PacketCursor cursor;
            cursor.input = const_cast<AVFormatContext*>(master_input);
            cursor.input_stream_index = static_cast<int>(index);
            cursor.output_stream_index = output_index;
            cursor.packet = packet;
            cursor.attached_picture = &input_stream->attached_pic;
            cursors->push_back(cursor);
            packets->push_back(packet);
            continue;
        }
        InputContext reader = OpenInput(path, cancellation_requested, error);
        if (!reader || index >= reader->nb_streams || reader->streams[index] == nullptr) {
            SetError(error, "cannot open per-stream interleave reader for stream " + std::to_string(index));
            return false;
        }
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            SetError(error, "cannot allocate per-stream interleave packet");
            return false;
        }
        reader_inputs->push_back(std::move(reader));
        cursors->push_back(PacketCursor{reader_inputs->back().get(), static_cast<int>(index), output_index, false,
                                         packet, 0});
        packets->push_back(packet);
    }
    return true;
}

bool BuildStandaloneIamfPacketCursor(const StandaloneIamfReader& iamf, AVFormatContext* input,
                                     int output_stream_index, std::vector<PacketCursor>* cursors,
                                     std::vector<AVPacket*>* packets, std::string* error) {
    if (input == nullptr || input->nb_streams != 1 || input->streams[0] == nullptr || output_stream_index < 0 ||
        cursors == nullptr || packets == nullptr || iamf.temporal_units().empty()) {
        SetError(error, "invalid standalone IAMF packet cursor request");
        return false;
    }
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        SetError(error, "cannot allocate standalone IAMF interleave packet");
        return false;
    }
    PacketCursor cursor;
    cursor.input = input;
    cursor.input_stream_index = 0;
    cursor.output_stream_index = output_stream_index;
    cursor.packet = packet;
    cursor.standalone_iamf = &iamf;
    cursors->push_back(cursor);
    packets->push_back(packet);
    return true;
}

bool BuildSyntheticTimecodePacketCursor(SyntheticTimecodeTrack* track,
                                        std::vector<PacketCursor>* cursors,
                                        std::vector<AVPacket*>* packets,
                                        std::string* error) {
    if (track == nullptr || !track->input || track->output_stream_index < 0 ||
        track->packet.empty() || cursors == nullptr || packets == nullptr) {
        SetError(error, "invalid synthetic timecode packet cursor request");
        return false;
    }
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        SetError(error, "cannot allocate synthetic timecode interleave packet");
        return false;
    }
    PacketCursor cursor;
    cursor.input = track->input.get();
    cursor.input_stream_index = 0;
    cursor.output_stream_index = track->output_stream_index;
    cursor.packet = packet;
    cursor.synthetic_packet = std::move(track->packet);
    cursor.synthetic_pts = track->pts;
    cursor.synthetic_duration = track->duration;
    cursors->push_back(std::move(cursor));
    packets->push_back(packet);
    return true;
}

void FreePacketCursors(std::vector<PacketCursor>* cursors, std::vector<AVPacket*>* packets) {
    if (cursors != nullptr) {
        for (PacketCursor& cursor : *cursors)
            ClearCursor(&cursor);
    }
    if (packets != nullptr) {
        for (AVPacket*& packet : *packets)
            av_packet_free(&packet);
        packets->clear();
    }
    if (cursors != nullptr)
        cursors->clear();
}

bool WritePackets(const FfmpegInterleavedMuxOptions& options, AVFormatContext* output,
                  std::vector<PacketCursor>* cursors, std::string* error) {
    if (output == nullptr || cursors == nullptr || cursors->empty())
        return false;
    for (PacketCursor& cursor : *cursors) {
        if (!ReadNextPacket(&cursor, error))
            return false;
    }
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        SetError(error, "cannot allocate output packet");
        return false;
    }
    bool success = true;
    while (true) {
        if (IsCancelled(options)) {
            SetError(error, "operation cancelled while interleaving MP4 packets");
            success = false;
            break;
        }
        PacketCursor* selected = nullptr;
        for (PacketCursor& cursor : *cursors) {
            if (cursor.eof)
                continue;
            if (selected == nullptr || CursorComesFirst(cursor, *selected))
                selected = &cursor;
        }
        if (selected == nullptr)
            break;
        const AVStream* input_stream = selected->input->streams[selected->input_stream_index];
        AVStream* output_stream = output->streams[selected->output_stream_index];
        if (input_stream == nullptr || output_stream == nullptr) {
            SetError(error, "packet cursor resolved an invalid output stream");
            success = false;
            break;
        }
        av_packet_move_ref(packet, selected->packet);
        packet->stream_index = selected->output_stream_index;
        av_packet_rescale_ts(packet, input_stream->time_base, output_stream->time_base);
        packet->pos = -1;
        // The cursors above already perform the cross-input DTS ordering.  Do
        // not feed that ordered sequence back into libavformat's second,
        // global interleave queue: movenc would be free to hold one of the
        // source tracks until the other input reaches EOF, recreating a
        // long tail of audio/data samples in the flat mdat.  av_write_frame
        // is the FFmpeg API intended for callers that perform their own
        // interleaving and passes the scheduler's physical order directly to
        // the MP4 muxer.
        const int status = av_write_frame(output, packet);
        av_packet_unref(packet);
        if (status < 0) {
            SetError(error, "FFmpeg cannot interleave packet for output stream " +
                               std::to_string(selected->output_stream_index) + ": " + AvError(status));
            success = false;
            break;
        }
        if (!ReadNextPacket(selected, error)) {
            success = false;
            break;
        }
    }
    av_packet_free(&packet);
    return success;
}

bool BuildOpusPlaceholderExtradata(AVCodecParameters* parameters, std::string* error) {
    if (parameters == nullptr) {
        SetError(error, "cannot allocate IAMF placeholder codec parameters");
        return false;
    }
    constexpr int kOpusHeadSize = 19;
    parameters->extradata = static_cast<uint8_t*>(av_mallocz(kOpusHeadSize + AV_INPUT_BUFFER_PADDING_SIZE));
    if (parameters->extradata == nullptr) {
        SetError(error, "cannot allocate IAMF placeholder OpusHead");
        return false;
    }
    std::memcpy(parameters->extradata, "OpusHead", 8);
    parameters->extradata[8] = 1;   // OpusHead version.
    parameters->extradata[9] = 2;   // Placeholder channel count; replaced with IAMF trak later.
    parameters->extradata[10] = 0;
    parameters->extradata[11] = 0;
    parameters->extradata[12] = 0x80;
    parameters->extradata[13] = 0xbb;
    parameters->extradata[14] = 0;
    parameters->extradata[15] = 0;
    parameters->extradata[16] = 0;
    parameters->extradata[17] = 0;
    parameters->extradata[18] = 0;
    parameters->extradata_size = kOpusHeadSize;
    return true;
}

bool AddStandaloneIamfPlaceholderStream(AVFormatContext* output, const StandaloneIamfReader& iamf,
                                        uint32_t* track_id, int* output_stream_index, std::string* error) {
    if (output == nullptr || track_id == nullptr || output_stream_index == nullptr) {
        SetError(error, "invalid IAMF placeholder output arguments");
        return false;
    }
    uint32_t max_track_id = 0;
    for (unsigned int index = 0; index < output->nb_streams; ++index) {
        const AVStream* stream = output->streams[index];
        if (stream != nullptr && stream->id > 0)
            max_track_id = std::max<uint32_t>(max_track_id, static_cast<uint32_t>(stream->id));
    }
    if (max_track_id >= 0x7ffffffeU)
        *track_id = 0x7fffffffU;
    else
        *track_id = std::max<uint32_t>(max_track_id + 1, 1U);
    AVStream* stream = avformat_new_stream(output, nullptr);
    if (stream == nullptr) {
        SetError(error, "cannot allocate IAMF placeholder stream");
        return false;
    }
    stream->id = static_cast<int>(*track_id);
    stream->time_base = AVRational{1, static_cast<int>(std::max<uint32_t>(iamf.summary().sample_rate, 1U))};
    stream->disposition = 0;
    av_dict_set(&stream->metadata, "handler_name",
                ("IAMF placeholder " + std::to_string(*track_id)).c_str(), 0);
    AVCodecParameters* parameters = stream->codecpar;
    parameters->codec_type = AVMEDIA_TYPE_AUDIO;
    parameters->codec_id = AV_CODEC_ID_OPUS;
    parameters->sample_rate = static_cast<int>(std::max<uint32_t>(iamf.summary().sample_rate, 1U));
    parameters->frame_size = static_cast<int>(std::max<uint32_t>(iamf.summary().frame_size, 1U));
    parameters->bit_rate = 0;
    parameters->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    if (!BuildOpusPlaceholderExtradata(parameters, error))
        return false;
    *output_stream_index = stream->index;
    return true;
}

bool WriteStandaloneIamfWithPlaceholder(const FfmpegInterleavedMuxOptions& options, AVFormatContext* source,
                                        const StandaloneIamfReader& iamf_reader,
                                        const VideoSpatialMetadata& output_spatial_metadata,
                                        std::string* error) {
    std::vector<InterleavedIamfTrack> iamf_tracks;
    AVFormatContext* output_raw = nullptr;
    int status = avformat_alloc_output_context2(&output_raw, nullptr, "mp4", options.output_path.c_str());
    if (status < 0 || output_raw == nullptr) {
        SetError(error, "cannot allocate MP4 output context: " + AvError(status < 0 ? status : AVERROR_UNKNOWN));
        if (output_raw != nullptr)
            avformat_free_context(output_raw);
        return false;
    }
    OutputContext output(output_raw);
    output->interrupt_callback.callback = InterruptCallback;
    output->interrupt_callback.opaque = const_cast<std::function<bool()>*>(&options.cancellation_requested);
    output->max_interleave_delta = options.max_interleave_delta;
    output->flags |= AVFMT_FLAG_AUTO_BSF;
    std::vector<SyntheticTimecodeTrack> timecode_tracks;
    std::vector<PendingTimecodeFlags> pending_timecode_flags;
    if (!CopyFormatInfo(source, nullptr, output.get(), error))
        return false;

    StreamMap map;
    if (!BuildSourceStreams(source, output.get(), options, &map, error))
        return false;
    uint32_t selected_video_track_id = 0;
    if (!ResolveOutputVideoTrackId(output.get(), map, options.requested_video_stream_index,
                                   &selected_video_track_id, error))
        return false;
    if (!CopyPrograms(source, output.get(), map.source_to_output, error))
        return false;
    const auto add_iamf_track = [&](const StandaloneIamfReader* reader) {
        if (reader == nullptr || reader->temporal_units().empty()) {
            SetError(error, "validated source IAMF track has no indexed temporal units");
            return false;
        }
        InterleavedIamfTrack track;
        track.reader = reader;
        track.input = CreateStandaloneIamfInput(*reader, error);
        if (!track.input ||
            !AddStandaloneIamfPlaceholderStream(output.get(), *reader,
                                                &track.placeholder_track_id,
                                                &track.output_stream_index, error))
            return false;
        iamf_tracks.push_back(std::move(track));
        return true;
    };
    if (!add_iamf_track(&iamf_reader))
        return false;
    for (const StandaloneIamfReader* source_iamf : options.source_iamf_tracks) {
        if (!add_iamf_track(source_iamf))
            return false;
    }
    if (source->nb_stream_groups != 0 &&
        !CopyStreamGroups(source, output.get(), map.source_to_output, false, error,
                          !options.keep_source_audio ||
                              options.route_source_iamf_tracks_through_probe))
        return false;
    if (!AddTimecodeMetadataTracks(source, output.get(), map.source_to_output,
                                   &timecode_tracks, &pending_timecode_flags, error))
        return false;

    if (IsCancelled(options)) {
        SetError(error, "operation cancelled before writing MP4 header");
        return false;
    }
    if ((output->oformat->flags & AVFMT_NOFILE) == 0) {
        status = avio_open(&output->pb, options.output_path.c_str(), AVIO_FLAG_WRITE);
        if (status < 0) {
            SetError(error, "cannot open MP4 output: " + AvError(status));
            return false;
        }
    }
    uint64_t estimated_packet_count = EstimateContextPacketCount(source);
    for (const InterleavedIamfTrack& track : iamf_tracks) {
        const uint64_t track_samples = track.reader->temporal_units().size();
        if (track_samples > std::numeric_limits<uint64_t>::max() - estimated_packet_count) {
            SetError(error, "estimated MP4 packet count overflows while reserving the movie index");
            return false;
        }
        estimated_packet_count += track_samples;
    }
    if (!WriteMp4HeaderWithFrontMoovReservation(output.get(), estimated_packet_count, error))
        return false;
    std::vector<InputContext> reader_inputs;
    std::vector<PacketCursor> cursors;
    std::vector<AVPacket*> packets;
    bool cursors_ready = BuildPacketCursors(options.source_path, source, map.source_to_output, output.get(),
                                            options.cancellation_requested, &reader_inputs, &cursors,
                                            &packets, error);
    for (const InterleavedIamfTrack& track : iamf_tracks) {
        if (!cursors_ready)
            break;
        cursors_ready = BuildStandaloneIamfPacketCursor(*track.reader, track.input.get(),
                                                         track.output_stream_index, &cursors,
                                                         &packets, error);
    }
    for (SyntheticTimecodeTrack& track : timecode_tracks) {
        if (!cursors_ready)
            break;
        cursors_ready = BuildSyntheticTimecodePacketCursor(&track, &cursors, &packets, error);
    }
    if (!cursors_ready) {
        FreePacketCursors(&cursors, &packets);
        return false;
    }
    const bool packet_success = WritePackets(options, output.get(), &cursors, error);
    FreePacketCursors(&cursors, &packets);
    if (!packet_success)
        return false;
    if (IsCancelled(options)) {
        SetError(error, "operation cancelled before finalizing MP4");
        return false;
    }
    status = av_write_trailer(output.get());
    if (status < 0) {
        SetError(error, "FFmpeg cannot finalize interleaved MP4: " + AvError(status));
        return false;
    }
    if (output->pb != nullptr && (output->oformat->flags & AVFMT_NOFILE) == 0)
        avio_flush(output->pb);
    std::vector<Mp4TimecodeTrackFlags> timecode_flags;
    timecode_flags.reserve(pending_timecode_flags.size());
    for (const PendingTimecodeFlags& pending : pending_timecode_flags) {
        if (pending.output_stream_index < 0 ||
            pending.output_stream_index >= static_cast<int>(output->nb_streams) ||
            output->streams[pending.output_stream_index] == nullptr ||
            output->streams[pending.output_stream_index]->id <= 0) {
            SetError(error, "cannot resolve output track ID for a timecode stream");
            return false;
        }
        timecode_flags.push_back({static_cast<uint32_t>(output->streams[pending.output_stream_index]->id),
                                  pending.flags});
    }
    output.reset();
    std::vector<FfmpegIamfPlaceholderTrack> placeholders;
    placeholders.reserve(iamf_tracks.size());
    for (const InterleavedIamfTrack& track : iamf_tracks)
        placeholders.push_back({track.placeholder_track_id, track.reader});
    if (!ReplaceFfmpegPlaceholderTracksWithIamf(options.output_path, placeholders, error,
                                                options.cancellation_requested) ||
        !PatchMp4VideoSpatialMetadata(options.output_path, selected_video_track_id,
                                      output_spatial_metadata, error))
        return false;
    return PatchMp4TimecodeTrackFlags(options.output_path, timecode_flags, error);
}

}  // namespace

bool FfmpegInterleavedMuxer::HasMp4Muxer() {
    return av_guess_format("mp4", nullptr, nullptr) != nullptr;
}

bool FfmpegInterleavedMuxer::Write(const FfmpegInterleavedMuxOptions& options, std::string* error) {
    if (options.source_path.empty() || options.iamf_path.empty() || options.output_path.empty()) {
        SetError(error, "source, IAMF, and output paths are required for interleaved muxing");
        return false;
    }
    if (options.max_interleave_delta < 0) {
        SetError(error, "max_interleave_delta must not be negative");
        return false;
    }
    if (IsCancelled(options)) {
        SetError(error, "operation cancelled before interleaved muxing");
        return false;
    }
    if (!HasMp4Muxer()) {
        SetError(error,
                 "FFmpeg was built without the MP4 muxer required for unified interleaved output");
        return false;
    }

    VideoSpatialMetadata output_spatial_metadata;
    if (!BuildVideoSpatialMetadata(options.video_mapping, options.source_spatial_metadata,
                                   &output_spatial_metadata, error))
        return false;

    InputContext source = OpenInput(options.source_path, options.cancellation_requested, error);
    if (!source)
        return false;
    StandaloneIamfReader standalone_iamf;
    std::string standalone_iamf_error;
    if (standalone_iamf.Open(options.iamf_path, &standalone_iamf_error)) {
        if (!standalone_iamf.ValidateObjectAudioAndParameters(&standalone_iamf_error)) {
            SetError(error, standalone_iamf_error);
            return false;
        }
        if (source->nb_streams == 0) {
            SetError(error, "interleaved mux inputs contain no streams");
            return false;
        }
        return WriteStandaloneIamfWithPlaceholder(options, source.get(), standalone_iamf,
                                                  output_spatial_metadata, error);
    }
    InputContext iamf = OpenInput(options.iamf_path, options.cancellation_requested, error);
    if (!iamf)
        return false;
    if (source->nb_streams == 0 || iamf->nb_streams == 0) {
        SetError(error, "interleaved mux inputs contain no streams");
        return false;
    }

    AVFormatContext* output_raw = nullptr;
    int status = avformat_alloc_output_context2(&output_raw, nullptr, "mp4", options.output_path.c_str());
    if (status < 0 || output_raw == nullptr) {
        SetError(error, "cannot allocate MP4 output context: " + AvError(status < 0 ? status : AVERROR_UNKNOWN));
        if (output_raw != nullptr)
            avformat_free_context(output_raw);
        return false;
    }
    OutputContext output(output_raw);
    output->interrupt_callback.callback = InterruptCallback;
    output->interrupt_callback.opaque = const_cast<std::function<bool()>*>(&options.cancellation_requested);
    output->max_interleave_delta = options.max_interleave_delta;
    output->flags |= AVFMT_FLAG_AUTO_BSF;
    std::vector<SyntheticTimecodeTrack> timecode_tracks;
    std::vector<PendingTimecodeFlags> pending_timecode_flags;

    if (!CopyFormatInfo(source.get(), iamf.get(), output.get(), error))
        return false;

    StreamMap map;
    if (!BuildSourceStreams(source.get(), output.get(), options, &map, error))
        return false;
    uint32_t selected_video_track_id = 0;
    if (!ResolveOutputVideoTrackId(output.get(), map, options.requested_video_stream_index,
                                   &selected_video_track_id, error))
        return false;
    if (!CopyPrograms(source.get(), output.get(), map.source_to_output, error))
        return false;
    map.iamf_to_output.assign(iamf->nb_streams, -1);
    for (unsigned int index = 0; index < iamf->nb_streams; ++index) {
        const AVStream* input_stream = iamf->streams[index];
        if (input_stream == nullptr || input_stream->codecpar == nullptr) {
            SetError(error, "IAMF input contains an invalid stream " + std::to_string(index));
            return false;
        }
        AVStream* output_stream = avformat_new_stream(output.get(), nullptr);
        if (output_stream == nullptr) {
            SetError(error, "cannot allocate output stream for IAMF stream " + std::to_string(index));
            return false;
        }
        if (!CopyStreamParameters(input_stream, output_stream, error))
            return false;
        map.iamf_to_output[index] = output_stream->index;
    }

    if (!CopyStreamGroups(iamf.get(), output.get(), map.iamf_to_output, true, error))
        return false;
    if (source->nb_stream_groups != 0 && !CopyStreamGroups(source.get(), output.get(), map.source_to_output, true, error))
        return false;
    if (!AddTimecodeMetadataTracks(source.get(), output.get(), map.source_to_output,
                                   &timecode_tracks, &pending_timecode_flags, error))
        return false;

    if (IsCancelled(options)) {
        SetError(error, "operation cancelled before writing MP4 header");
        return false;
    }
    if ((output->oformat->flags & AVFMT_NOFILE) == 0) {
        status = avio_open(&output->pb, options.output_path.c_str(), AVIO_FLAG_WRITE);
        if (status < 0) {
            SetError(error, "cannot open MP4 output: " + AvError(status));
            return false;
        }
    }
    const uint64_t source_packet_count = EstimateContextPacketCount(source.get());
    const uint64_t iamf_packet_count = EstimateContextPacketCount(iamf.get());
    if (iamf_packet_count > std::numeric_limits<uint64_t>::max() - source_packet_count) {
        SetError(error, "estimated MP4 packet count overflows while reserving the movie index");
        return false;
    }
    if (!WriteMp4HeaderWithFrontMoovReservation(output.get(), source_packet_count + iamf_packet_count, error))
        return false;
    std::vector<InputContext> reader_inputs;
    std::vector<PacketCursor> cursors;
    std::vector<AVPacket*> packets;
    if (!BuildPacketCursors(options.source_path, source.get(), map.source_to_output, output.get(),
                            options.cancellation_requested, &reader_inputs, &cursors, &packets, error) ||
        !BuildPacketCursors(options.iamf_path, iamf.get(), map.iamf_to_output, output.get(),
                            options.cancellation_requested, &reader_inputs, &cursors, &packets, error)) {
        FreePacketCursors(&cursors, &packets);
        return false;
    }
    for (SyntheticTimecodeTrack& track : timecode_tracks) {
        if (!BuildSyntheticTimecodePacketCursor(&track, &cursors, &packets, error)) {
            FreePacketCursors(&cursors, &packets);
            return false;
        }
    }
    const bool packet_success = WritePackets(options, output.get(), &cursors, error);
    FreePacketCursors(&cursors, &packets);
    if (!packet_success)
        return false;
    if (IsCancelled(options)) {
        SetError(error, "operation cancelled before finalizing MP4");
        return false;
    }
    status = av_write_trailer(output.get());
    if (status < 0) {
        SetError(error, "FFmpeg cannot finalize interleaved MP4: " + AvError(status));
        return false;
    }
    if (output->pb != nullptr && (output->oformat->flags & AVFMT_NOFILE) == 0) {
        avio_flush(output->pb);
    }
    std::vector<Mp4TimecodeTrackFlags> timecode_flags;
    timecode_flags.reserve(pending_timecode_flags.size());
    for (const PendingTimecodeFlags& pending : pending_timecode_flags) {
        if (pending.output_stream_index < 0 ||
            pending.output_stream_index >= static_cast<int>(output->nb_streams) ||
            output->streams[pending.output_stream_index] == nullptr ||
            output->streams[pending.output_stream_index]->id <= 0) {
            SetError(error, "cannot resolve output track ID for a timecode stream");
            return false;
        }
        timecode_flags.push_back({static_cast<uint32_t>(output->streams[pending.output_stream_index]->id),
                                  pending.flags});
    }
    output.reset();
    if (!PatchMp4VideoSpatialMetadata(options.output_path, selected_video_track_id,
                                      output_spatial_metadata, error))
        return false;
    return PatchMp4TimecodeTrackFlags(options.output_path, timecode_flags, error);
}

}  // namespace immersive_audio_muxer
