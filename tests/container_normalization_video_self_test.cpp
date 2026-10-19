#include "container_normalization_service.h"
#include "mux_job.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/intreadwrite.h>
}

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

struct VideoStats {
    AVCodecID codec_id = AV_CODEC_ID_NONE;
    uint64_t packet_count = 0;
    uint64_t missing_dts_count = 0;
    uint64_t non_monotonic_dts_count = 0;
    std::vector<int64_t> presentation_times_us;
};

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
                ("iamf_muxer_video_normalization_test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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

bool InspectVideo(const std::filesystem::path& path, int requested_stream_index, bool require_valid_dts,
                  VideoStats* stats, int* resolved_stream_index, std::string* error) {
    if (stats == nullptr || resolved_stream_index == nullptr) {
        if (error != nullptr)
            *error = "video inspection output is null";
        return false;
    }
    *stats = {};
    *resolved_stream_index = -1;

    AVFormatContext* input = nullptr;
    int status = avformat_open_input(&input, path.u8string().c_str(), nullptr, nullptr);
    if (status < 0) {
        if (error != nullptr)
            *error = "cannot open video inspection input: " + AvError(status);
        avformat_close_input(&input);
        return false;
    }
    status = avformat_find_stream_info(input, nullptr);
    if (status < 0) {
        if (error != nullptr)
            *error = "cannot inspect video input streams: " + AvError(status);
        avformat_close_input(&input);
        return false;
    }

    int video_index = requested_stream_index;
    if (video_index < 0)
        video_index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_index < 0 || video_index >= static_cast<int>(input->nb_streams) ||
        input->streams[video_index] == nullptr || input->streams[video_index]->codecpar == nullptr ||
        input->streams[video_index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        if (error != nullptr)
            *error = "cannot resolve a video stream for packet inspection";
        avformat_close_input(&input);
        return false;
    }
    *resolved_stream_index = video_index;
    stats->codec_id = input->streams[video_index]->codecpar->codec_id;

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        if (error != nullptr)
            *error = "cannot allocate packet for video inspection";
        avformat_close_input(&input);
        return false;
    }
    int64_t last_dts = AV_NOPTS_VALUE;
    while ((status = av_read_frame(input, packet)) >= 0) {
        if (packet->stream_index == video_index) {
            ++stats->packet_count;
            const AVStream* video_stream = input->streams[video_index];
            stats->presentation_times_us.push_back(
                packet->pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE
                                              : av_rescale_q(packet->pts, video_stream->time_base, AV_TIME_BASE_Q));
            if (packet->dts == AV_NOPTS_VALUE) {
                ++stats->missing_dts_count;
                if (require_valid_dts) {
                    if (error != nullptr)
                        *error = "normalized video packet has no DTS";
                    av_packet_unref(packet);
                    av_packet_free(&packet);
                    avformat_close_input(&input);
                    return false;
                }
            } else {
                if (last_dts != AV_NOPTS_VALUE && packet->dts <= last_dts) {
                    ++stats->non_monotonic_dts_count;
                    if (require_valid_dts) {
                        if (error != nullptr)
                            *error = "video DTS is not strictly increasing while reading packet " +
                                     std::to_string(stats->packet_count) + ": previous=" + std::to_string(last_dts) +
                                     ", current=" + std::to_string(packet->dts);
                        av_packet_unref(packet);
                        av_packet_free(&packet);
                        avformat_close_input(&input);
                        return false;
                    }
                }
                last_dts = packet->dts;
            }
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&input);
    if (status != AVERROR_EOF) {
        if (error != nullptr)
            *error = "cannot read all video packets: " + AvError(status);
        return false;
    }
    return true;
}

bool ParseExpected(const char* text, uint64_t* value) {
    if (text == nullptr || value == nullptr)
        return false;
    try {
        const unsigned long long parsed = std::stoull(text);
        if (parsed > std::numeric_limits<uint64_t>::max())
            return false;
        *value = static_cast<uint64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool HasIamfAudioTrack(const std::filesystem::path& path, std::string* error) {
    AVFormatContext* input = nullptr;
    int status = avformat_open_input(&input, path.u8string().c_str(), nullptr, nullptr);
    if (status < 0) {
        if (error != nullptr)
            *error = "cannot open muxed output to verify IAMF audio: " + AvError(status);
        avformat_close_input(&input);
        return false;
    }
    status = avformat_find_stream_info(input, nullptr);
    if (status < 0) {
        if (error != nullptr)
            *error = "cannot inspect muxed output audio: " + AvError(status);
        avformat_close_input(&input);
        return false;
    }
    bool found_iamf_audio = false;
    std::string audio_streams;
    for (unsigned int index = 0; index < input->nb_streams; ++index) {
        const AVStream* stream = input->streams[index];
        if (stream == nullptr || stream->codecpar == nullptr ||
            stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (!audio_streams.empty())
            audio_streams += ", ";
        audio_streams += std::string(avcodec_get_name(stream->codecpar->codec_id)) + "/" +
                         std::to_string(stream->codecpar->codec_tag);
        // This FFmpeg build may expose IAMF as Opus substreams rather than one
        // stream carrying the `iamf` sample-entry tag.
        if (stream->codecpar->codec_tag == MKTAG('i', 'a', 'm', 'f') ||
            stream->codecpar->codec_id == AV_CODEC_ID_OPUS)
            found_iamf_audio = true;
    }
    avformat_close_input(&input);
    if (!found_iamf_audio && error != nullptr)
        *error = "muxed output contains no identifiable IAMF audio track; audio streams=[" + audio_streams + "]";
    return found_iamf_audio;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4 && argc != 5) {
        std::cerr << "usage: container_normalization_video_self_test <input.mkv> "
                     "[expected_packets expected_missing_dts [standalone.iamf]]\n";
        return 2;
    }

    uint64_t expected_packets = 0;
    uint64_t expected_missing_dts = 0;
    const bool has_expectations = argc >= 4;
    if (has_expectations && (!ParseExpected(argv[2], &expected_packets) ||
                             !ParseExpected(argv[3], &expected_missing_dts))) {
        std::cerr << "expected packet and missing-DTS counts must be non-negative integers\n";
        return 2;
    }

    const std::filesystem::path source = std::filesystem::u8path(argv[1]);
    TestDirectory test_directory;
    if (test_directory.path().empty()) {
        std::cerr << "cannot create video normalization test directory\n";
        return 1;
    }

    VideoStats source_stats;
    int source_video_index = -1;
    std::string error;
    if (!InspectVideo(source, -1, false, &source_stats, &source_video_index, &error)) {
        std::cerr << "cannot inspect source video packets: " << error << '\n';
        return 1;
    }
    if (has_expectations && (source_stats.packet_count != expected_packets ||
                             source_stats.missing_dts_count != expected_missing_dts)) {
        std::cerr << "source video packet stats differ from expectations: packets=" << source_stats.packet_count
                  << ", missing_dts=" << source_stats.missing_dts_count << '\n';
        return 1;
    }

    const auto normalized_path = test_directory.path() / "normalized.mp4";
    int normalized_video_index = -1;
    if (!immersive_audio_muxer::ContainerNormalizationService::NormalizeToMp4(
            source.u8string(), normalized_path.u8string(), source_video_index, false, &normalized_video_index, &error)) {
        std::cerr << "video normalization failed: " << error << '\n';
        return 1;
    }

    VideoStats normalized_stats;
    int inspected_normalized_video_index = -1;
    if (!InspectVideo(normalized_path, normalized_video_index, true, &normalized_stats,
                      &inspected_normalized_video_index, &error)) {
        std::cerr << "cannot validate normalized video packets: " << error << '\n';
        return 1;
    }
    if (normalized_stats.codec_id != source_stats.codec_id) {
        std::cerr << "video codec changed during stream copy: source=" << avcodec_get_name(source_stats.codec_id)
                  << ", normalized=" << avcodec_get_name(normalized_stats.codec_id) << '\n';
        return 1;
    }
    if (normalized_stats.packet_count != source_stats.packet_count) {
        std::cerr << "video packet count changed during normalization: source=" << source_stats.packet_count
                  << ", normalized=" << normalized_stats.packet_count << '\n';
        return 1;
    }
    if (source_stats.presentation_times_us.size() != normalized_stats.presentation_times_us.size() ||
        source_stats.presentation_times_us.empty() || source_stats.presentation_times_us.front() == AV_NOPTS_VALUE ||
        normalized_stats.presentation_times_us.front() == AV_NOPTS_VALUE) {
        std::cerr << "video presentation timestamps could not be compared across normalization\n";
        return 1;
    }
    const int64_t presentation_offset_us = normalized_stats.presentation_times_us.front() -
                                           source_stats.presentation_times_us.front();
    for (size_t index = 0; index < source_stats.presentation_times_us.size(); ++index) {
        const int64_t source_pts = source_stats.presentation_times_us[index];
        const int64_t normalized_pts = normalized_stats.presentation_times_us[index];
        if (source_pts == AV_NOPTS_VALUE || normalized_pts == AV_NOPTS_VALUE ||
            std::llabs((normalized_pts - source_pts) - presentation_offset_us) > 1500) {
            std::cerr << "video presentation timeline changed during normalization at packet " << (index + 1)
                      << ": source_pts_us=" << source_pts << ", normalized_pts_us=" << normalized_pts
                      << ", expected_constant_offset_us=" << presentation_offset_us << '\n';
            return 1;
        }
    }

    if (argc == 5) {
        immersive_audio_muxer::MuxJobRequest request;
        request.iamf_path = std::filesystem::u8path(argv[4]);
        request.video_path = source;
        request.output_path = test_directory.path() / "muxed.mp4";
        request.video_stream_index = source_video_index;
        request.overwrite_existing_files = false;
        immersive_audio_muxer::MuxJobResult mux_result;
        if (!immersive_audio_muxer::MuxJob::Run(request, &mux_result, &error)) {
            std::cerr << "full mux failed: " << error << '\n';
            return 1;
        }
        if (!mux_result.wrote_output) {
            std::cerr << "full mux reported success without writing the output file\n";
            return 1;
        }
        VideoStats muxed_stats;
        int muxed_video_index = -1;
        if (!InspectVideo(request.output_path, -1, true, &muxed_stats, &muxed_video_index, &error)) {
            std::cerr << "cannot validate muxed video packets: " << error << '\n';
            return 1;
        }
        if (muxed_stats.codec_id != source_stats.codec_id ||
            muxed_stats.packet_count != source_stats.packet_count ||
            muxed_stats.presentation_times_us.size() != source_stats.presentation_times_us.size()) {
            std::cerr << "full mux changed the video stream: codec=" << avcodec_get_name(muxed_stats.codec_id)
                      << ", packets=" << muxed_stats.packet_count << '\n';
            return 1;
        }
        const int64_t muxed_pts_offset_us = muxed_stats.presentation_times_us.front() -
                                            source_stats.presentation_times_us.front();
        for (size_t index = 0; index < source_stats.presentation_times_us.size(); ++index) {
            const int64_t source_pts = source_stats.presentation_times_us[index];
            const int64_t muxed_pts = muxed_stats.presentation_times_us[index];
            if (source_pts == AV_NOPTS_VALUE || muxed_pts == AV_NOPTS_VALUE ||
                std::llabs((muxed_pts - source_pts) - muxed_pts_offset_us) > 1500) {
                std::cerr << "full mux changed the video presentation timeline at packet " << (index + 1) << '\n';
                return 1;
            }
        }
        if (!HasIamfAudioTrack(request.output_path, &error)) {
            std::cerr << error << '\n';
            return 1;
        }
        std::cout << "full mux passed: video_packets=" << muxed_stats.packet_count
                  << ", iamf_audio=present, pts_offset_us=" << muxed_pts_offset_us << '\n';
    }

    std::cout << "video stream-copy normalization passed: codec=" << avcodec_get_name(source_stats.codec_id)
              << ", packets=" << source_stats.packet_count << " -> " << normalized_stats.packet_count
              << ", source_missing_dts=" << source_stats.missing_dts_count
              << ", source_non_monotonic_dts=" << source_stats.non_monotonic_dts_count
              << ", pts_offset_us=" << presentation_offset_us
              << ", normalized_dts=present_and_strictly_increasing\n";
    return 0;
}
