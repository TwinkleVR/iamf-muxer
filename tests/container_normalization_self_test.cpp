#include "container_normalization_service.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string AvError(int status) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(status, buffer, sizeof(buffer));
    return buffer;
}

class TestDirectory final {
public:
    TestDirectory() {
        std::error_code error;
        path_ = std::filesystem::temp_directory_path(error) /
                ("iamf_muxer_normalization_test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!error)
            std::filesystem::create_directories(path_, error);
        if (error)
            path_.clear();
    }

    ~TestDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

bool ReadStreams(const std::filesystem::path& path, std::vector<AVMediaType>* types, std::vector<AVCodecID>* codecs, std::string* error) {
    if (types == nullptr || codecs == nullptr) {
        if (error != nullptr)
            *error = "stream result vectors are null";
        return false;
    }
    types->clear();
    codecs->clear();
    const std::string input_path = path.u8string();
    AVFormatContext* input = nullptr;
    int status = avformat_open_input(&input, input_path.c_str(), nullptr, nullptr);
    if (status < 0) {
        if (error != nullptr)
            *error = "cannot open media input: " + AvError(status);
        avformat_close_input(&input);
        return false;
    }
    status = avformat_find_stream_info(input, nullptr);
    if (status < 0) {
        if (error != nullptr)
            *error = "cannot inspect media streams: " + AvError(status);
        avformat_close_input(&input);
        return false;
    }
    for (unsigned int index = 0; index < input->nb_streams; ++index) {
        const AVStream* stream = input->streams[index];
        if (stream == nullptr || stream->codecpar == nullptr) {
            if (error != nullptr)
                *error = "media input contains an invalid stream";
            avformat_close_input(&input);
            return false;
        }
        types->push_back(stream->codecpar->codec_type);
        codecs->push_back(stream->codecpar->codec_id);
    }
    avformat_close_input(&input);
    return true;
}

bool VerifySubtitleText(const std::filesystem::path& path, const std::string& expected_text, std::string* error) {
    const std::string input_path = path.u8string();
    AVFormatContext* input = nullptr;
    int status = avformat_open_input(&input, input_path.c_str(), nullptr, nullptr);
    if (status < 0) {
        if (error != nullptr)
            *error = "cannot open normalized MP4 for subtitle verification: " + AvError(status);
        avformat_close_input(&input);
        return false;
    }
    status = avformat_find_stream_info(input, nullptr);
    if (status < 0) {
        if (error != nullptr)
            *error = "cannot inspect normalized MP4 subtitle stream: " + AvError(status);
        avformat_close_input(&input);
        return false;
    }
    int subtitle_stream_index = -1;
    for (unsigned int index = 0; index < input->nb_streams; ++index) {
        const AVStream* stream = input->streams[index];
        if (stream != nullptr && stream->codecpar != nullptr && stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
            stream->codecpar->codec_id == AV_CODEC_ID_MOV_TEXT) {
            subtitle_stream_index = static_cast<int>(index);
            break;
        }
    }
    if (subtitle_stream_index < 0) {
        if (error != nullptr)
            *error = "normalized MP4 has no MOV_TEXT subtitle stream";
        avformat_close_input(&input);
        return false;
    }

    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_MOV_TEXT);
    AVCodecContext* decoder_context = decoder == nullptr ? nullptr : avcodec_alloc_context3(decoder);
    if (decoder_context == nullptr) {
        if (error != nullptr)
            *error = "cannot allocate MOV_TEXT subtitle decoder";
        avcodec_free_context(&decoder_context);
        avformat_close_input(&input);
        return false;
    }
    status = avcodec_parameters_to_context(decoder_context, input->streams[subtitle_stream_index]->codecpar);
    if (status >= 0)
        status = avcodec_open2(decoder_context, decoder, nullptr);
    if (status < 0) {
        if (error != nullptr)
            *error = "cannot open MOV_TEXT subtitle decoder: " + AvError(status);
        avcodec_free_context(&decoder_context);
        avformat_close_input(&input);
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        if (error != nullptr)
            *error = "cannot allocate packet for subtitle verification";
        avcodec_free_context(&decoder_context);
        avformat_close_input(&input);
        return false;
    }
    bool found_expected_text = false;
    bool found_timed_packet = false;
    while ((status = av_read_frame(input, packet)) >= 0) {
        if (packet->stream_index == subtitle_stream_index) {
            found_timed_packet = found_timed_packet || (packet->pts != AV_NOPTS_VALUE && packet->duration > 0);
            AVSubtitle subtitle{};
            int got_subtitle = 0;
            status = avcodec_decode_subtitle2(decoder_context, &subtitle, &got_subtitle, packet);
            if (status < 0) {
                if (error != nullptr)
                    *error = "cannot decode normalized MOV_TEXT packet: " + AvError(status);
                av_packet_unref(packet);
                av_packet_free(&packet);
                avcodec_free_context(&decoder_context);
                avformat_close_input(&input);
                return false;
            }
            if (got_subtitle) {
                for (unsigned int rect_index = 0; rect_index < subtitle.num_rects; ++rect_index) {
                    const AVSubtitleRect* rect = subtitle.rects[rect_index];
                    if (rect != nullptr && ((rect->text != nullptr && expected_text == rect->text) ||
                                            (rect->ass != nullptr && std::string(rect->ass).find(expected_text) != std::string::npos)))
                        found_expected_text = true;
                }
                avsubtitle_free(&subtitle);
            }
        }
        av_packet_unref(packet);
    }
    const bool reached_eof = status == AVERROR_EOF;
    av_packet_free(&packet);
    avcodec_free_context(&decoder_context);
    avformat_close_input(&input);
    if (!reached_eof || !found_timed_packet || !found_expected_text) {
        if (error != nullptr)
            *error = "normalized subtitle payload or timing is invalid: eof=" + std::to_string(reached_eof) +
                     ", timed_packet=" + std::to_string(found_timed_packet) + ", expected_text=" + std::to_string(found_expected_text);
        return false;
    }
    return true;
}

bool VerifyNormalization(const std::filesystem::path& source, const std::filesystem::path& output, int source_video_index,
                         bool keep_source_audio, int expected_video_index, std::string* error) {
    int normalized_video_index = -1;
    if (!immersive_audio_muxer::ContainerNormalizationService::NormalizeToMp4(source.u8string(), output.u8string(), source_video_index,
                                                                              keep_source_audio, &normalized_video_index, error))
        return false;
    if (normalized_video_index != expected_video_index) {
        if (error != nullptr)
            *error = "normalized selected-video index is " + std::to_string(normalized_video_index) + ", expected " +
                     std::to_string(expected_video_index);
        return false;
    }

    std::vector<AVMediaType> types;
    std::vector<AVCodecID> codecs;
    if (!ReadStreams(output, &types, &codecs, error))
        return false;
    int audio_count = 0;
    int video_count = 0;
    int subtitle_count = 0;
    bool has_expected_video = false;
    bool has_expected_audio = false;
    bool has_expected_subtitle = false;
    for (size_t index = 0; index < types.size(); ++index) {
        if (types[index] == AVMEDIA_TYPE_VIDEO) {
            ++video_count;
            has_expected_video = has_expected_video || codecs[index] == AV_CODEC_ID_MPEG4;
        } else if (types[index] == AVMEDIA_TYPE_AUDIO) {
            ++audio_count;
            has_expected_audio = has_expected_audio || codecs[index] == AV_CODEC_ID_AC3;
        } else if (types[index] == AVMEDIA_TYPE_SUBTITLE) {
            ++subtitle_count;
            has_expected_subtitle = has_expected_subtitle || codecs[index] == AV_CODEC_ID_MOV_TEXT;
        }
    }
    if (video_count != 1 || !has_expected_video || subtitle_count != 1 || audio_count != (keep_source_audio ? 1 : 0) ||
        has_expected_audio != keep_source_audio) {
        if (error != nullptr)
            *error = "normalized stream selection is incorrect: video=" + std::to_string(video_count) +
                     ", audio=" + std::to_string(audio_count) + ", subtitles=" + std::to_string(subtitle_count);
        return false;
    }
    if (!has_expected_subtitle) {
        if (error != nullptr)
            *error = "normalized SubRip subtitle was not converted to MOV_TEXT";
        return false;
    }
    return VerifySubtitleText(output, "Test subtitle", error);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: container_normalization_self_test <subrip-source.mkv>\n";
        return 2;
    }
    TestDirectory test_directory;
    if (test_directory.path().empty()) {
        std::cerr << "cannot create normalization test directory\n";
        return 1;
    }
    const std::filesystem::path source = std::filesystem::u8path(argv[1]);
    const auto dropped_audio_output = test_directory.path() / "drop-audio.mp4";
    const auto kept_audio_output = test_directory.path() / "keep-audio.mp4";
    std::string error;
    std::vector<AVMediaType> source_types;
    std::vector<AVCodecID> source_codecs;
    if (!ReadStreams(source, &source_types, &source_codecs, &error)) {
        std::cerr << "cannot inspect SubRip test fixture: " << error << '\n';
        return 1;
    }
    int source_video_index = -1;
    bool audio_precedes_video = false;
    bool has_subrip = false;
    for (size_t index = 0; index < source_types.size(); ++index) {
        if (source_types[index] == AVMEDIA_TYPE_AUDIO && source_video_index < 0)
            audio_precedes_video = true;
        if (source_types[index] == AVMEDIA_TYPE_VIDEO && source_video_index < 0)
            source_video_index = static_cast<int>(index);
        if (source_types[index] == AVMEDIA_TYPE_SUBTITLE && source_codecs[index] == AV_CODEC_ID_SUBRIP)
            has_subrip = true;
    }
    if (source_video_index <= 0 || !audio_precedes_video || !has_subrip) {
        std::cerr << "test fixture must put audio before video and contain a SubRip subtitle stream\n";
        return 1;
    }

    if (!VerifyNormalization(source, dropped_audio_output, source_video_index, false, 0, &error)) {
        std::cerr << "normalization with source audio disabled failed: " << error << '\n';
        return 1;
    }
    if (!VerifyNormalization(source, kept_audio_output, source_video_index, true, source_video_index, &error)) {
        std::cerr << "normalization with source audio enabled failed: " << error << '\n';
        return 1;
    }
    std::cout << "MKV normalization preserves SubRip subtitles as MOV_TEXT and honors source-audio selection.\n";
    return 0;
}
