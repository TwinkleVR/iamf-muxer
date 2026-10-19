#include "ffmpeg_interleaved_muxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace immersive_audio_muxer;

struct FormatCloser {
    void operator()(AVFormatContext* context) const {
        if (context != nullptr)
            avformat_close_input(&context);
    }
};

struct PacketCloser {
    void operator()(AVPacket* packet) const {
        av_packet_free(&packet);
    }
};

using InputFormat = std::unique_ptr<AVFormatContext, FormatCloser>;
using Packet = std::unique_ptr<AVPacket, PacketCloser>;

struct AudioTrack {
    uint32_t codec_tag = 0;
    int sample_rate = 0;
    int channels = 0;
    std::vector<std::vector<uint8_t>> packets;
};

struct MediaSummary {
    int video_streams = 0;
    int audio_streams = 0;
    bool has_iamf = false;
    std::map<AVCodecID, AudioTrack> source_audio_tracks;
    std::vector<std::string> stream_descriptions;
};

std::string AvError(int status) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(status, buffer, sizeof(buffer));
    return buffer;
}

std::string FourCC(uint32_t tag) {
    std::string value(4, '\0');
    for (int index = 0; index < 4; ++index)
        value[index] = static_cast<char>((tag >> (index * 8)) & 0xff);
    return value;
}

bool IsSourceCodec(AVCodecID codec_id) {
    return codec_id == AV_CODEC_ID_FLAC || codec_id == AV_CODEC_ID_PCM_S24LE ||
           codec_id == AV_CODEC_ID_PCM_F32LE;
}

bool ReadMedia(const std::string& path, MediaSummary* summary, std::string* error) {
    if (summary == nullptr) {
        if (error != nullptr)
            *error = "media summary output is null";
        return false;
    }
    *summary = {};
    AVFormatContext* raw_context = nullptr;
    int status = avformat_open_input(&raw_context, path.c_str(), nullptr, nullptr);
    if (status < 0) {
        avformat_close_input(&raw_context);
        if (error != nullptr)
            *error = "cannot open " + path + ": " + AvError(status);
        return false;
    }
    InputFormat context(raw_context);
    status = avformat_find_stream_info(context.get(), nullptr);
    if (status < 0) {
        if (error != nullptr)
            *error = "cannot read stream information from " + path + ": " + AvError(status);
        return false;
    }

    for (unsigned int index = 0; index < context->nb_streams; ++index) {
        const AVStream* stream = context->streams[index];
        if (stream == nullptr || stream->codecpar == nullptr)
            continue;
        summary->stream_descriptions.push_back(
            std::to_string(index) + ":" + avcodec_get_name(stream->codecpar->codec_id) + "/" +
            FourCC(stream->codecpar->codec_tag));
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            ++summary->video_streams;
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        ++summary->audio_streams;
        const std::string codec_tag = FourCC(stream->codecpar->codec_tag);
        // The linked FFmpeg demuxer may expose IAMF as Opus substreams rather
        // than one stream carrying the `iamf` sample-entry tag.
        if (codec_tag == "iamf" || !IsSourceCodec(stream->codecpar->codec_id)) {
            summary->has_iamf = true;
        }
        if (!IsSourceCodec(stream->codecpar->codec_id))
            continue;
        AudioTrack track;
        track.codec_tag = stream->codecpar->codec_tag;
        track.sample_rate = stream->codecpar->sample_rate;
        track.channels = stream->codecpar->ch_layout.nb_channels;
        summary->source_audio_tracks.emplace(stream->codecpar->codec_id, std::move(track));
    }

    Packet packet(av_packet_alloc());
    if (!packet) {
        if (error != nullptr)
            *error = "cannot allocate an FFmpeg packet";
        return false;
    }
    for (;;) {
        status = av_read_frame(context.get(), packet.get());
        if (status == AVERROR_EOF)
            break;
        if (status < 0) {
            if (error != nullptr)
                *error = "cannot read packets from " + path + ": " + AvError(status);
            return false;
        }
        if (packet->stream_index >= 0 &&
            packet->stream_index < static_cast<int>(context->nb_streams)) {
            const AVStream* stream = context->streams[packet->stream_index];
            if (stream != nullptr && stream->codecpar != nullptr) {
                auto track = summary->source_audio_tracks.find(stream->codecpar->codec_id);
                if (track != summary->source_audio_tracks.end() && packet->size > 0) {
                    track->second.packets.emplace_back(packet->data,
                                                       packet->data + packet->size);
                }
            }
        }
        av_packet_unref(packet.get());
    }
    return true;
}

bool RunMux(const std::string& source_path, const std::string& iamf_path,
            const std::string& output_path, bool keep_source_audio,
            std::string* error) {
    std::error_code filesystem_error;
    std::filesystem::remove(std::filesystem::u8path(output_path), filesystem_error);
    if (filesystem_error) {
        if (error != nullptr)
            *error = "cannot remove previous test output: " + filesystem_error.message();
        return false;
    }
    FfmpegInterleavedMuxOptions options;
    options.source_path = source_path;
    options.iamf_path = iamf_path;
    options.output_path = output_path;
    options.requested_video_stream_index = 0;
    options.keep_source_audio = keep_source_audio;
    return FfmpegInterleavedMuxer::Write(options, error);
}

bool VerifyKeptAudio(const MediaSummary& source, const MediaSummary& output,
                     std::string* error) {
    if (source.video_streams != 1 || source.audio_streams != 3 ||
        source.source_audio_tracks.size() != 3) {
        if (error != nullptr)
            *error = "the regression input must contain one video stream, FLAC, PCM S24LE, and PCM F32LE; found video=" +
                     std::to_string(source.video_streams) + ", audio=" +
                     std::to_string(source.audio_streams) + ", supported audio=" +
                     std::to_string(source.source_audio_tracks.size());
        return false;
    }
    if (output.video_streams != 1 || output.audio_streams < 4 || !output.has_iamf ||
        output.source_audio_tracks.size() != 3) {
        if (error != nullptr)
            *error = "Keep Source Audio output must contain video, IAMF, FLAC, PCM S24LE, and PCM F32LE tracks; found video=" +
                     std::to_string(output.video_streams) + ", audio=" +
                     std::to_string(output.audio_streams) + ", IAMF=" +
                     (output.has_iamf ? "yes" : "no") + ", source audio=" +
                     std::to_string(output.source_audio_tracks.size()) + ", streams=[" +
                     [&output]() {
                         std::string value;
                         for (const std::string& stream : output.stream_descriptions)
                             value += (value.empty() ? "" : ", ") + stream;
                         return value;
                     }() + "]";
        return false;
    }

    for (const auto& source_entry : source.source_audio_tracks) {
        const auto output_entry = output.source_audio_tracks.find(source_entry.first);
        if (output_entry == output.source_audio_tracks.end()) {
            if (error != nullptr)
                *error = std::string("output is missing source codec ") +
                         avcodec_get_name(source_entry.first);
            return false;
        }
        const std::string expected_tag = source_entry.first == AV_CODEC_ID_FLAC
                                             ? "fLaC"
                                             : source_entry.first == AV_CODEC_ID_PCM_F32LE ? "fpcm" : "ipcm";
        if (FourCC(source_entry.second.codec_tag) != expected_tag ||
            FourCC(output_entry->second.codec_tag) != expected_tag) {
            if (error != nullptr)
                *error = std::string("unexpected sample entry for ") +
                         avcodec_get_name(source_entry.first);
            return false;
        }
        if (source_entry.second.sample_rate != output_entry->second.sample_rate ||
            source_entry.second.channels != output_entry->second.channels ||
            source_entry.second.packets.empty() ||
            source_entry.second.packets != output_entry->second.packets) {
            if (error != nullptr)
                *error = "source audio packet payload or stream parameters changed for " +
                         std::string(avcodec_get_name(source_entry.first));
            return false;
        }
    }
    return true;
}

bool VerifyDroppedAudio(const MediaSummary& output, std::string* error) {
    if (output.video_streams != 1 || output.audio_streams < 1 || !output.has_iamf ||
        !output.source_audio_tracks.empty()) {
        if (error != nullptr)
            *error = "disabled Keep Source Audio output must contain only video and IAMF tracks";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: keep_source_audio_codec_self_test <source.mp4> <iamf.iamf> "
                     "<keep-output.mp4> <drop-output.mp4>\n";
        return 2;
    }
    const std::string source_path = argv[1];
    const std::string iamf_path = argv[2];
    const std::string keep_output_path = argv[3];
    const std::string drop_output_path = argv[4];
    std::string error;
    MediaSummary source;
    if (!ReadMedia(source_path, &source, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    if (!RunMux(source_path, iamf_path, keep_output_path, true, &error)) {
        std::cerr << "Keep Source Audio mux failed: " << error << '\n';
        return 1;
    }
    MediaSummary kept;
    if (!ReadMedia(keep_output_path, &kept, &error) || !VerifyKeptAudio(source, kept, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    if (!RunMux(source_path, iamf_path, drop_output_path, false, &error)) {
        std::cerr << "audio-excluded mux failed: " << error << '\n';
        return 1;
    }
    MediaSummary dropped;
    if (!ReadMedia(drop_output_path, &dropped, &error) || !VerifyDroppedAudio(dropped, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "Keep Source Audio preserved FLAC/fLaC, PCM S24LE/ipcm, and PCM F32LE/fpcm packets unchanged.\n";
    return 0;
}
