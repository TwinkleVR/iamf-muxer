#include "mux_job.h"

#include "iamf_box_writer.h"
#include "path_utils.h"
#include "runtime_logging.h"
#include "iamf_mp4_file_writer.h"
#include "atomic_output_transaction.h"
#include "container_normalization_service.h"
#include "ffmpeg_iamf_track_adapter.h"
#include "ffmpeg_interleaved_muxer.h"
#include "standalone_iamf_reader.h"
#include "video_input.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace immersive_audio_muxer {
namespace {

const char* StageName(MuxJobStage stage) {
    switch (stage) {
        case MuxJobStage::kPreparingEncoder:
            return "preparing-encoder";
        case MuxJobStage::kOpeningVideo:
            return "opening-video";
        case MuxJobStage::kReadingIamf:
            return "reading-iamf";
        case MuxJobStage::kValidatingIamf:
            return "validating-iamf";
        case MuxJobStage::kWritingOutput:
            return "writing-output";
        case MuxJobStage::kCompleted:
            return "completed";
        case MuxJobStage::kCancelled:
            return "cancelled";
        case MuxJobStage::kNone:
            return "none";
    }
    return "unknown";
}

void Emit(const MuxJobRequest& request, MuxJobStage stage, const std::string& message) {
    IAMF_MUX_LOG_INFO("[MuxJob][{}] {}", StageName(stage), message);
    if (request.event_callback)
        request.event_callback({stage, message});
}

bool IsCancelled(const MuxJobRequest& request) {
    return request.cancellation_requested && request.cancellation_requested();
}

bool Fail(MuxJobResult* result, MuxJobStage stage, const std::string& message, std::string* error) {
    if (result)
        result->failed_stage = stage;
    if (error)
        *error = message;
    if (stage == MuxJobStage::kCancelled)
        IAMF_MUX_LOG_WARN("[MuxJob][{}] {}", StageName(stage), message);
    else
        IAMF_MUX_LOG_ERROR("[MuxJob][{}] {}", StageName(stage), message);
    return false;
}

class MuxRunLogScope final {
public:
    MuxRunLogScope(const MuxJobResult* result, const std::string* error)
        : result_(result), error_(error), started_(std::chrono::steady_clock::now()) {}

    ~MuxRunLogScope() noexcept {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_).count();
        const MuxJobStage stage = result_ == nullptr ? MuxJobStage::kNone : result_->failed_stage;
        const char* message = error_ == nullptr || error_->empty()
                                  ? "no error text was provided"
                                  : error_->c_str();
        try {
            if (completed_)
                IAMF_MUX_LOG_INFO("[MuxJob] run {} after {} ms", outcome_, elapsed);
            else if (stage == MuxJobStage::kCancelled)
                IAMF_MUX_LOG_WARN("[MuxJob] run cancelled at stage={} after {} ms: {}",
                                  StageName(stage), elapsed, message);
            else
                IAMF_MUX_LOG_ERROR("[MuxJob] run failed at stage={} after {} ms: {}",
                                   StageName(stage), elapsed, message);
        } catch (...) {
        }
    }

    void MarkCompleted(std::string outcome) { completed_ = true; outcome_ = std::move(outcome); }

private:
    const MuxJobResult* result_;
    const std::string* error_;
    std::chrono::steady_clock::time_point started_;
    std::string outcome_;
    bool completed_ = false;
};

std::string NormalizedPathKey(const std::filesystem::path& path) {
    std::string result = path_utils::ToUtf8(std::filesystem::absolute(path).lexically_normal());
#if defined(_WIN32)
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
#endif
    return result;
}

bool IsSamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    return !left.empty() && !right.empty() && NormalizedPathKey(left) == NormalizedPathKey(right);
}

class TemporaryPathGuard final {
public:
    TemporaryPathGuard() = default;
    ~TemporaryPathGuard() {
        Cleanup();
    }

    void Set(std::filesystem::path path) { path_ = std::move(path); }
    void Cleanup() noexcept {
        if (path_.empty())
            return;
        std::error_code error;
        std::filesystem::remove(path_, error);
        path_.clear();
    }
    TemporaryPathGuard(const TemporaryPathGuard&) = delete;
    TemporaryPathGuard& operator=(const TemporaryPathGuard&) = delete;

private:
    std::filesystem::path path_;
};

std::filesystem::path NormalizedSourcePath(const std::unique_ptr<AtomicOutputTransaction>& transaction,
                                           std::string* error) {
    static std::atomic_uint64_t sequence{0};
    if (transaction) {
        const auto directory = transaction->temporary_path().parent_path();
        return directory / (".normalized-source-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed) + 1) +
                            ".mp4");
    }
    std::error_code filesystem_error;
    const auto directory = std::filesystem::temp_directory_path(filesystem_error);
    if (filesystem_error) {
        if (error)
            *error = "cannot locate temporary directory for container normalization: " + filesystem_error.message();
        return {};
    }
    return directory / ("iamf_muxer-normalized-" +
                        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed) + 1) + ".mp4");
}

}  // namespace

bool MuxJob::Run(const MuxJobRequest& request, MuxJobResult* result, std::string* error) {
    if (result)
        *result = {};
    if (error)
        error->clear();
    MuxRunLogScope run_log(result, error);
    const char* mode = request.encoder_options.has_value()
                           ? "audio-encode-and-mux"
                           : (request.video_path.empty() ? "iamf-only" : "iamf-with-video");
    IAMF_MUX_LOG_INFO(
        "[MuxJob] starting: mode={} iamf={} video={} output={} requested_video_stream={} keep_source_audio={} video_mapping={} overwrite_existing={} dry_run={}",
        mode, path_utils::ToUtf8(request.iamf_path), path_utils::ToUtf8(request.video_path),
        path_utils::ToUtf8(request.output_path), request.video_stream_index,
        request.keep_mp4_audio, VideoMappingPresetLabel(request.video_mapping.preset),
        request.overwrite_existing_files, request.dry_run);
    if (request.iamf_path.empty() && !request.encoder_options) {
        return Fail(result, MuxJobStage::kPreparingEncoder, "provide an IAMF input or official encoder options", error);
    }
    if (!request.dry_run && request.output_path.empty()) {
        return Fail(result, MuxJobStage::kWritingOutput, "an output path is required unless --dry-run is used", error);
    }
    if (!request.output_path.empty() && (IsSamePath(request.output_path, request.iamf_path) ||
                                         IsSamePath(request.output_path, request.video_path))) {
        return Fail(result, MuxJobStage::kWritingOutput, "output path must not overwrite an input file", error);
    }
    std::unique_ptr<AtomicOutputTransaction> output_transaction;
    if (!request.dry_run) {
        output_transaction = std::make_unique<AtomicOutputTransaction>();
        std::string transaction_error;
        if (!AtomicOutputTransaction::Create(request.output_path, request.overwrite_existing_files,
                                             output_transaction.get(), &transaction_error))
            return Fail(result, MuxJobStage::kWritingOutput, transaction_error, error);
    }
    TemporaryPathGuard normalized_source_cleanup;
    if (IsCancelled(request)) {
        Emit(request, MuxJobStage::kCancelled, "operation cancelled before start");
        return Fail(result, MuxJobStage::kCancelled, "operation cancelled", error);
    }

    std::filesystem::path iamf_path = request.iamf_path;
    if (iamf_path.empty()) {
        Emit(request, MuxJobStage::kPreparingEncoder, "starting official IAMF encoder");
        InProcessIamfEncoderOptions encoder_options = *request.encoder_options;
        const auto inherited_cancellation = encoder_options.cancellation_requested;
        encoder_options.cancellation_requested = [&request, inherited_cancellation]() {
            return (inherited_cancellation && inherited_cancellation()) || IsCancelled(request);
        };
        std::filesystem::path generated_iamf;
        std::string encoder_output;
        std::string encoder_error;
        if (!InProcessIamfEncoder::Encode(encoder_options, &generated_iamf, &encoder_error, &encoder_output)) {
            if (!encoder_output.empty())
                Emit(request, MuxJobStage::kPreparingEncoder, encoder_output);
            const MuxJobStage stage = IsCancelled(request) ? MuxJobStage::kCancelled : MuxJobStage::kPreparingEncoder;
            if (stage == MuxJobStage::kCancelled)
                Emit(request, stage, "operation cancelled while running official IAMF encoder");
            return Fail(result, stage, encoder_error, error);
        }
        if (!encoder_output.empty())
            Emit(request, MuxJobStage::kPreparingEncoder, encoder_output);
        iamf_path = generated_iamf;
        Emit(request, MuxJobStage::kPreparingEncoder, "official IAMF encoder completed: " + path_utils::ToUtf8(iamf_path));
    }

    VideoInput video;
    if (!request.video_path.empty()) {
        if (IsCancelled(request)) {
            Emit(request, MuxJobStage::kCancelled, "operation cancelled before video scan");
            return Fail(result, MuxJobStage::kCancelled, "operation cancelled", error);
        }
        Emit(request, MuxJobStage::kOpeningVideo, "reading input video streams");
        if (!FfmpegInterleavedMuxer::HasMp4Muxer())
            return Fail(result, MuxJobStage::kOpeningVideo,
                        "the configured FFmpeg build has no MP4 muxer; unified interleaved muxing is required",
                        error);
        std::string video_error;
        // Read only container metadata first. This lets an unusual MKV/WebM
        // reach the normalization path even when its original packet timeline
        // is not acceptable to the independent MP4 writer.
        if (!video.OpenMetadataOnly(path_utils::ToUtf8(request.video_path), request.video_stream_index, &video_error,
                                    request.cancellation_requested, request.keep_mp4_audio))
            return Fail(result, MuxJobStage::kOpeningVideo, video_error, error);
        if (!video.IsMp4Like()) {
            const std::filesystem::path normalized_path = NormalizedSourcePath(output_transaction, &video_error);
            if (normalized_path.empty())
                return Fail(result, MuxJobStage::kOpeningVideo, video_error, error);
            Emit(request, MuxJobStage::kOpeningVideo, "normalizing non-MP4 input with FFmpeg stream copy");
            int normalized_video_stream_index = -1;
            if (!ContainerNormalizationService::NormalizeToMp4(
                    path_utils::ToUtf8(request.video_path), path_utils::ToUtf8(normalized_path), video.track().stream_index,
                    request.keep_mp4_audio, &normalized_video_stream_index, &video_error,
                    request.cancellation_requested))
                return Fail(result, MuxJobStage::kOpeningVideo, video_error, error);
            normalized_source_cleanup.Set(normalized_path);
            if (!video.OpenMetadataOnly(path_utils::ToUtf8(normalized_path), normalized_video_stream_index, &video_error,
                                         request.cancellation_requested, request.keep_mp4_audio))
                return Fail(result, MuxJobStage::kOpeningVideo, video_error, error);
        }
        // All video inputs reach this point as MP4. A video mux must use the
        // scheduler so every track is interleaved and the front-moov reservation
        // remains active, regardless of spatial-mapping selection.
        if (!video.IsMp4Like())
            return Fail(result, MuxJobStage::kOpeningVideo,
                        "video input could not be normalized to MP4; unified interleaved muxing requires MP4",
                        error);
    }

    if (IsCancelled(request)) {
        Emit(request, MuxJobStage::kCancelled, "operation cancelled before IAMF scan");
        return Fail(result, MuxJobStage::kCancelled, "operation cancelled", error);
    }
    Emit(request, MuxJobStage::kReadingIamf, "reading standalone IAMF temporal units");
    StandaloneIamfReader reader;
    std::string reader_error;
    if (!reader.Open(path_utils::ToUtf8(iamf_path), &reader_error))
        return Fail(result, MuxJobStage::kReadingIamf, reader_error, error);

    Emit(request, MuxJobStage::kValidatingIamf, "validating IAMF descriptors and configuration box");
    if (!reader.ValidateObjectAudioAndParameters(&reader_error))
        return Fail(result, MuxJobStage::kValidatingIamf, reader_error, error);
    std::vector<uint8_t> configuration_box;
    if (!IamfBoxWriter::BuildConfigurationBox(reader.descriptor_bytes(), &configuration_box, &reader_error))
        return Fail(result, MuxJobStage::kValidatingIamf, reader_error, error);

    if (result) {
        result->iamf_path = iamf_path;
        result->iamf_summary = reader.summary();
        result->configuration_box_size = configuration_box.size();
    }
    const IamfIndexSummary& iamf_summary = reader.summary();
    IAMF_MUX_LOG_INFO(
        "[MuxJob] IAMF validated: path={} descriptor_bytes={} temporal_unit_bytes={} temporal_units={} total_samples={} sample_rate={} frame_size={} codec_configs={} audio_elements={} mix_presentations={} profile={} annotation={} configuration_box_bytes={}",
        path_utils::ToUtf8(iamf_path), iamf_summary.descriptor_bytes,
        iamf_summary.temporal_unit_bytes, iamf_summary.temporal_unit_count,
        iamf_summary.total_samples, iamf_summary.sample_rate, iamf_summary.frame_size,
        iamf_summary.codec_config_count, iamf_summary.audio_element_count,
        iamf_summary.mix_presentation_count, iamf_summary.primary_profile,
        iamf_summary.mix_presentation_annotation, configuration_box.size());
    if (request.dry_run) {
        Emit(request, MuxJobStage::kCompleted, "IAMF validation completed without writing output");
        run_log.MarkCompleted("dry-run validation succeeded");
        return true;
    }
    if (IsCancelled(request)) {
        Emit(request, MuxJobStage::kCancelled, "operation cancelled before MP4 writing");
        return Fail(result, MuxJobStage::kCancelled, "operation cancelled", error);
    }

    Emit(request, MuxJobStage::kWritingOutput, "writing MP4 output");
    std::string writer_error;
    const bool iamf_only = request.video_path.empty();
    const std::filesystem::path staging_path = output_transaction->temporary_path();
    std::vector<std::unique_ptr<StandaloneIamfReader>> preserved_source_iamf_readers;
    std::vector<const StandaloneIamfReader*> preserved_source_iamf_tracks;
    if (!iamf_only && request.keep_mp4_audio) {
        for (const std::string& source_iamf_path : video.validated_iamf_audio_paths()) {
            auto source_iamf = std::make_unique<StandaloneIamfReader>();
            if (!source_iamf->Open(source_iamf_path, &writer_error) ||
                !source_iamf->ValidateObjectAudioAndParameters(&writer_error)) {
                return Fail(result, MuxJobStage::kValidatingIamf,
                            "iamf-tools accepted an embedded IAMF track, but it could not be indexed for passthrough: " +
                                writer_error,
                            error);
            }
            preserved_source_iamf_tracks.push_back(source_iamf.get());
            preserved_source_iamf_readers.push_back(std::move(source_iamf));
        }
    }
    bool write_ok = false;
    if (iamf_only) {
        write_ok = IamfMp4FileWriter::WriteIamfOnly(path_utils::ToUtf8(staging_path), reader, &writer_error,
                                                    request.cancellation_requested);
    } else {
        FfmpegInterleavedMuxOptions interleaved_options;
        interleaved_options.source_path = path_utils::ToUtf8(video.track().source_path);
        interleaved_options.iamf_path = path_utils::ToUtf8(iamf_path);
        interleaved_options.output_path = path_utils::ToUtf8(staging_path);
        interleaved_options.requested_video_stream_index = video.track().stream_index;
        interleaved_options.keep_source_audio = request.keep_mp4_audio;
        interleaved_options.video_mapping = request.video_mapping;
        interleaved_options.source_spatial_metadata = video.track().spatial_metadata;
        interleaved_options.route_source_iamf_tracks_through_probe = request.keep_mp4_audio;
        interleaved_options.source_iamf_tracks = preserved_source_iamf_tracks;
        interleaved_options.cancellation_requested = request.cancellation_requested;
        write_ok = FfmpegInterleavedMuxer::Write(interleaved_options, &writer_error);
    }
    if (!write_ok) {
        output_transaction->Abort();
        return Fail(result, MuxJobStage::kWritingOutput, writer_error, error);
    }
    if (IsCancelled(request)) {
        output_transaction->Abort();
        Emit(request, MuxJobStage::kCancelled, "operation cancelled after MP4 writing");
        return Fail(result, MuxJobStage::kCancelled, "operation cancelled", error);
    }

    normalized_source_cleanup.Cleanup();
    std::string commit_error;
    if (!output_transaction->Commit(&commit_error)) {
        output_transaction->Abort();
        return Fail(result, MuxJobStage::kWritingOutput, commit_error, error);
    }
    if (!commit_error.empty())
        Emit(request, MuxJobStage::kWritingOutput, "warning: " + commit_error);
    if (result) {
        result->wrote_output = true;
        result->iamf_only_output = iamf_only;
    }
    Emit(request, MuxJobStage::kCompleted, "MP4 output written: " + path_utils::ToUtf8(request.output_path));
    std::error_code file_size_error;
    const uintmax_t output_size = std::filesystem::file_size(request.output_path, file_size_error);
    IAMF_MUX_LOG_INFO(
        "[MuxJob] output committed: path={} file_size_bytes={} iamf_temporal_units={} iamf_only={}",
        path_utils::ToUtf8(request.output_path), file_size_error ? 0 : output_size,
        iamf_summary.temporal_unit_count, iamf_only);
    run_log.MarkCompleted("output committed");
    return true;
}

}  // namespace immersive_audio_muxer
