#include "gui_controller.h"

#include "app/audio_preparation_service.h"
#include "app/media_probe_service.h"
#include "app/metadata_builder.h"
#include "mux_job.h"
#include "runtime_logging.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QStringList>
#include <QUrl>

#if defined(Q_OS_WIN)
#include <shlobj.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace immersive_audio_muxer::gui {
namespace {

QString PathText(const std::optional<app::MediaProbeInfo>& info) {
    if (!info)
        return {};
    return QDir::fromNativeSeparators(QString::fromUtf8(info->path.u8string().c_str()));
}

QString PathText(const std::filesystem::path& path) {
    if (path.empty())
        return {};
    return QDir::fromNativeSeparators(QString::fromUtf8(path.u8string().c_str()));
}

QString NormalizedPathText(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    const auto normalized = (error ? path : absolute).lexically_normal();
    return QDir::fromNativeSeparators(QString::fromUtf8(normalized.u8string().c_str()));
}

class WorkspaceCleanupGuard final {
public:
    explicit WorkspaceCleanupGuard(std::filesystem::path path) : path_(std::move(path)) {}
    ~WorkspaceCleanupGuard() {
        if (path_.empty())
            return;
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

private:
    std::filesystem::path path_;
};

QString ExistingParentDirectory(const QFileInfo& target) {
    QDir directory(target.absolutePath());
    while (!directory.exists()) {
        const QString previous = directory.absolutePath();
        if (!directory.cdUp() || directory.absolutePath() == previous)
            break;
    }
    return directory.absolutePath();
}

#if defined(Q_OS_WIN)
bool SelectWindowsFile(const QString& path) {
    const QString native_path = QDir::toNativeSeparators(path);
    PIDLIST_ABSOLUTE item = ILCreateFromPathW(
        reinterpret_cast<LPCWSTR>(native_path.utf16()));
    if (item == nullptr)
        return false;

    PIDLIST_ABSOLUTE folder = ILClone(item);
    PCUITEMID_CHILD child = ILFindLastID(item);
    const bool has_parent = folder != nullptr && child != nullptr && ILRemoveLastID(folder);
    const bool selected = has_parent &&
                          SUCCEEDED(SHOpenFolderAndSelectItems(folder, 1, &child, 0));

    if (folder != nullptr)
        CoTaskMemFree(folder);
    CoTaskMemFree(item);
    return selected;
}
#endif

bool SamePath(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    const QString lhs_text = NormalizedPathText(lhs);
    const QString rhs_text = NormalizedPathText(rhs);
#if defined(_WIN32)
    return lhs_text.compare(rhs_text, Qt::CaseInsensitive) == 0;
#else
    return lhs_text == rhs_text;
#endif
}

QString FormatDuration(int64_t duration_ms);

QString MediaSummary(const std::optional<app::MediaProbeInfo>& info, bool channel_based = false) {
    if (!info)
        return {};
    const auto audio = std::find_if(info->streams.begin(), info->streams.end(),
                                    [](const app::MediaStreamInfo& s) {
                                        return s.kind == app::MediaStreamKind::kAudio;
                                    });
    const auto video = std::find_if(info->streams.begin(), info->streams.end(),
                                    [](const app::MediaStreamInfo& s) {
                                        return s.kind == app::MediaStreamKind::kVideo;
                                    });
    QString result = channel_based && info->kind == app::InputKind::kAudioCandidate
                         ? QStringLiteral("audio input detected")
                         : QString::fromUtf8(info->diagnostic.c_str());
    if (audio != info->streams.end()) {
        result += QStringLiteral(" | ") + QString::fromUtf8(audio->codec_name.c_str());
        result += QStringLiteral(" | ") + QString::number(audio->channel_count) + QStringLiteral(" ch");
        if (audio->sample_rate > 0)
            result += QStringLiteral(" | ") + QString::number(audio->sample_rate) + QStringLiteral(" Hz");
    } else if (video != info->streams.end()) {
        result += QStringLiteral(" | ") + QString::fromUtf8(video->codec_name.c_str());
        result += QStringLiteral(" | ") + QString::number(video->width) + QStringLiteral("x") +
                  QString::number(video->height);
    }
    const QString duration = FormatDuration(info->duration_ms);
    if (!duration.isEmpty())
        result += QStringLiteral(" | ") + duration;
    return result;
}

QString SlotError(const QString& slot) {
    return QStringLiteral("Unsupported input slot: ") + slot;
}

QString FormatDuration(int64_t duration_ms) {
    if (duration_ms <= 0)
        return {};
    const qlonglong total_seconds = static_cast<qlonglong>(duration_ms / 1000);
    const qlonglong hours = total_seconds / 3600;
    const int minutes = static_cast<int>((total_seconds / 60) % 60);
    const int seconds = static_cast<int>(total_seconds % 60);
    const int milliseconds = static_cast<int>(duration_ms % 1000);
    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'))
        .arg(milliseconds, 3, 10, QChar('0'));
}

QString ChannelLayoutTitle(app::ChannelLayout layout) {
    if (layout == app::ChannelLayout::kMono)
        return QStringLiteral("Mono");
    if (layout == app::ChannelLayout::kStereo)
        return QStringLiteral("Stereo");
    return QString::fromLatin1(app::ToString(layout));
}

app::ChannelLayout ParseChannelLayoutTitle(const QString& name) {
    for (const app::ChannelLayout layout : {
             app::ChannelLayout::kMono, app::ChannelLayout::kStereo, app::ChannelLayout::k3_1_2,
             app::ChannelLayout::k5_1, app::ChannelLayout::k5_1_2,
             app::ChannelLayout::k5_1_4, app::ChannelLayout::k7_1,
             app::ChannelLayout::k7_1_2, app::ChannelLayout::k7_1_4}) {
        if (ChannelLayoutTitle(layout).compare(name, Qt::CaseInsensitive) == 0)
            return layout;
    }
    return app::ChannelLayout::kUnset;
}

QString ChannelLayoutUnavailableMessage(const app::MediaStreamInfo* stream) {
    const QString generic_message =
        QStringLiteral("No supported layout matches this audio.");
    if (stream == nullptr)
        return generic_message;

    const QString layout_name = QString::fromUtf8(stream->channel_layout_name.c_str()).trimmed();
    static const QStringList supported_layout_names = {
        QStringLiteral("mono"),       QStringLiteral("stereo"),    QStringLiteral("3.1.2"),
        QStringLiteral("5.1"),        QStringLiteral("5.1(side)"), QStringLiteral("5.1(back)"),
        QStringLiteral("5.1.2"),      QStringLiteral("5.1.2(back)"),
        QStringLiteral("5.1.4"),      QStringLiteral("7.1"),      QStringLiteral("7.1.2"),
        QStringLiteral("7.1.4")};
    const bool looks_like_named_layout = !layout_name.isEmpty() &&
                                         layout_name.front().isDigit() &&
                                         layout_name.contains(QLatin1Char('.'));
    if (looks_like_named_layout &&
        !supported_layout_names.contains(layout_name, Qt::CaseInsensitive))
        return layout_name + QStringLiteral(" isn't supported yet.");
    return generic_message;
}

QString FriendlyCodec(const std::string& codec_name) {
    const QString codec = QString::fromUtf8(codec_name.c_str()).toLower();
    if (codec == QStringLiteral("h264"))
        return QStringLiteral("H.264 / AVC");
    if (codec == QStringLiteral("hevc") || codec == QStringLiteral("h265"))
        return QStringLiteral("H.265 / HEVC");
    if (codec == QStringLiteral("av1"))
        return QStringLiteral("AV1");
    if (codec == QStringLiteral("vp9"))
        return QStringLiteral("VP9");
    if (codec == QStringLiteral("opus"))
        return QStringLiteral("Opus");
    if (codec == QStringLiteral("aac") || codec == QStringLiteral("aac-lc"))
        return QStringLiteral("AAC-LC");
    if (codec == QStringLiteral("vorbis"))
        return QStringLiteral("Vorbis");
    if (codec == QStringLiteral("iamf"))
        return QStringLiteral("IAMF");
    return QString::fromUtf8(codec_name.c_str());
}

QString FormatSampleRate(int sample_rate) {
    if (sample_rate <= 0)
        return {};
    if (sample_rate % 1000 == 0)
        return QString::number(sample_rate / 1000) + QStringLiteral("kHz");
    return QString::number(sample_rate) + QStringLiteral(" Hz");
}

QString FormatBitDepth(int bit_depth) {
    if (bit_depth <= 0)
        return {};
    return QString::number(bit_depth) + QStringLiteral("-bit");
}

QString FormatBitrate(int64_t bit_rate) {
    if (bit_rate <= 0)
        return {};
    if (bit_rate >= 1000000)
        return QString::number(bit_rate / 1000000.0, 'f', bit_rate >= 10000000 ? 0 : 1) +
               QStringLiteral(" Mbps");
    if (bit_rate >= 1000)
        return QString::number(bit_rate / 1000.0, 'f', bit_rate >= 10000000 ? 0 : 1) +
               QStringLiteral(" kbps");
    return QString::number(bit_rate) + QStringLiteral(" bps");
}

QString FormatFrameRate(double frame_rate) {
    if (frame_rate <= 0.0)
        return {};
    QString value = QString::number(frame_rate, 'f', 3);
    while (value.endsWith(QLatin1Char('0')))
        value.chop(1);
    if (value.endsWith(QLatin1Char('.')))
        value.chop(1);
    return value + QStringLiteral(" fps");
}

QString AmbisonicsOrderTag(app::AudioRole role) {
    switch (role) {
        case app::AudioRole::kFirstOrderAmbisonics:
            return QStringLiteral("1st Order");
        case app::AudioRole::kSecondOrderAmbisonics:
            return QStringLiteral("2nd Order");
        case app::AudioRole::kThirdOrderAmbisonics:
            return QStringLiteral("3rd Order");
        default:
            return {};
    }
}

const app::MediaStreamInfo* FirstStream(const app::MediaProbeInfo& info,
                                        app::MediaStreamKind kind) {
    const auto it = std::find_if(info.streams.begin(), info.streams.end(),
                                 [kind](const app::MediaStreamInfo& stream) {
                                     return stream.kind == kind;
                                 });
    return it == info.streams.end() ? nullptr : &*it;
}

QStringList TagsForInfo(const std::optional<app::MediaProbeInfo>& optional_info,
                        bool video, app::AudioRole audio_role) {
    QStringList tags;
    if (!optional_info)
        return tags;

    const app::MediaProbeInfo& info = *optional_info;
    const app::MediaStreamInfo* stream =
        FirstStream(info, video ? app::MediaStreamKind::kVideo : app::MediaStreamKind::kAudio);
    const QString duration = FormatDuration(info.duration_ms);
    if (!duration.isEmpty())
        tags << duration;

    if (video) {
        if (stream != nullptr) {
            const QString codec = FriendlyCodec(stream->codec_name);
            if (!codec.isEmpty())
                tags << codec;
            if (stream->width > 0 && stream->height > 0)
                tags << QString::number(stream->width) + QStringLiteral("x") +
                            QString::number(stream->height);
            const QString frame_rate = FormatFrameRate(stream->frame_rate);
            if (!frame_rate.isEmpty())
                tags << frame_rate;
            const QString bit_rate = FormatBitrate(stream->bit_rate);
            if (!bit_rate.isEmpty())
                tags << bit_rate;
        }
        return tags;
    }

    if (info.kind == app::InputKind::kStandaloneIamf) {
        if (!info.iamf_profile.empty())
            tags << QString::fromUtf8(info.iamf_profile.c_str());
        if (stream != nullptr) {
            const QString codec = FriendlyCodec(stream->codec_name);
            if (!codec.isEmpty())
                tags << codec;
        }
        if (info.iamf_mix_presentation_count > 0)
            tags << QString::number(info.iamf_mix_presentation_count) +
                        QStringLiteral(" Mix Presentations");
        if (info.iamf_audio_element_count > 0)
            tags << QString::number(info.iamf_audio_element_count) +
                        QStringLiteral(" Audio Elements");
        return tags;
    }

    if (stream != nullptr) {
        const QString codec = FriendlyCodec(stream->codec_name);
        if (!codec.isEmpty())
            tags << codec;
    }

    const QString order = AmbisonicsOrderTag(audio_role);
    if (!order.isEmpty())
        tags << order;
    if (stream != nullptr && stream->channel_count > 0)
        tags << QString::number(stream->channel_count) + QStringLiteral("ch");
    if (stream != nullptr) {
        const QString sample_rate = FormatSampleRate(stream->sample_rate);
        if (!sample_rate.isEmpty())
            tags << sample_rate;
        const QString bit_depth = FormatBitDepth(stream->bit_depth);
        if (!bit_depth.isEmpty())
            tags << bit_depth;
    }
    return tags;
}
bool LooksLikeStandaloneIamf(const std::filesystem::path& path) {
    // Drag feedback must stay cheap: inspect only the first OBU and its IA code.
    std::ifstream file(path, std::ios::binary);
    std::array<unsigned char, 32> bytes{};
    file.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    const std::size_t bytes_read = static_cast<std::size_t>(file.gcount());
    if (bytes_read == 0 || (bytes[0] >> 3) != 31)
        return false;

    std::size_t payload_offset = 1;
    bool size_complete = false;
    for (int i = 0; i < 10 && payload_offset < bytes_read; ++i) {
        const unsigned char size_byte = bytes[payload_offset++];
        if ((size_byte & 0x80) == 0) {
            size_complete = true;
            break;
        }
    }
    if (!size_complete || payload_offset + 4 > bytes_read)
        return false;

    return bytes[payload_offset] == 'i' &&
           bytes[payload_offset + 1] == 'a' &&
           bytes[payload_offset + 2] == 'm' &&
           bytes[payload_offset + 3] == 'f';
}

}  // namespace

GuiController::GuiController(QObject* parent) : QObject(parent) {}

GuiController::~GuiController() {
    cancel_requested_.store(true);
    if (worker_.joinable())
        worker_.join();
}

QString GuiController::detectedInputType() const {
    if (!modeSelected())
        return QStringLiteral("Choose audio workflow");
    if (!spatial_audio_)
        return iamfMode() ? QStringLiteral("IAMF mode")
                          : (ambisonicsMode() ? QStringLiteral("Ambisonics mode")
                                              : QStringLiteral("Channel-based mode"));
    return iamfMode() ? QStringLiteral("IAMF")
                      : (ambisonicsMode() ? QStringLiteral("Ambisonics")
                                          : QStringLiteral("Channel-based"));
}

bool GuiController::iamfMode() const {
    return input_mode_ == 0;
}

bool GuiController::ambisonicsMode() const {
    return input_mode_ == 1;
}

bool GuiController::channelBasedMode() const {
    return input_mode_ == 2;
}

void GuiController::setInputMode(int mode) {
    if (job_running_)
        return;
    if (mode < -1 || mode > 2)
        return;
    if (input_mode_ == mode)
        return;

    input_mode_ = mode;
    mix_annotation_ = QStringLiteral("default mix presentation");
    emit projectOptionsChanged();
    spatial_audio_.reset();
    head_locked_stereo_.reset();
    channel_layout_ = app::ChannelLayout::kUnset;
    channel_layout_confirmed_ = false;
    RecalculateProject();
    if (mode == 0) {
        SetStatus(QStringLiteral("IAMF selected. Add a standalone IAMF file to continue."));
    } else if (mode == 1) {
        SetStatus(QStringLiteral("Ambisonics selected. Add Ambisonics audio to continue."));
    } else if (mode == 2) {
        SetStatus(QStringLiteral("Channel-based selected. Add a supported speaker-layout audio file to continue."));
    } else {
        SetStatus(QStringLiteral("Choose an audio workflow to begin."));
    }
    emit projectStateChanged();
}

bool GuiController::videoHasAudio() const {
    return video_ && video_->HasAudio();
}

QString GuiController::videoPath() const { return PathText(video_); }
QString GuiController::videoSummary() const { return MediaSummary(video_); }
QStringList GuiController::videoTags() const {
    return TagsForInfo(video_, true, app::AudioRole::kUnset);
}
QString GuiController::spatialAudioPath() const { return PathText(spatial_audio_); }
QString GuiController::spatialAudioSummary() const {
    return MediaSummary(spatial_audio_, channelBasedMode());
}
QStringList GuiController::spatialAudioTags() const {
    return TagsForInfo(spatial_audio_, false,
                       spatial_audio_ && !channelBasedMode() ? spatial_audio_->suggested_audio_role
                                                             : app::AudioRole::kUnset);
}
QStringList GuiController::channelLayoutOptions() const {
    QStringList options;
    if (!channelBasedMode() || !spatial_audio_)
        return options;
    const app::MediaStreamInfo* stream =
        FirstStream(*spatial_audio_, app::MediaStreamKind::kAudio);
    if (stream == nullptr)
        return options;

    std::vector<app::ChannelLayout> layouts = spatial_audio_->channel_layout_candidates;
    const auto suggested = std::find(layouts.begin(), layouts.end(),
                                     spatial_audio_->suggested_channel_layout);
    if (suggested != layouts.end())
        std::rotate(layouts.begin(), suggested, std::next(suggested));
    for (const app::ChannelLayout layout : layouts)
        options.push_back(ChannelLayoutTitle(layout));
    return options;
}
QString GuiController::channelLayoutName() const {
    return ChannelLayoutTitle(channel_layout_);
}
QString GuiController::channelOrderDescription() const {
    if (channel_layout_ == app::ChannelLayout::kUnset)
        return {};
    QString description = QString::fromStdString(app::InputChannelOrderDescription(channel_layout_));
    description.replace(QStringLiteral(" / "), QStringLiteral(" "));
    return description;
}
QString GuiController::channelLayoutStatus() const {
    if (!channelBasedMode() || !spatial_audio_)
        return QStringLiteral("Add an audio file to detect its channel layout.");

    const app::MediaStreamInfo* stream = FirstStream(*spatial_audio_, app::MediaStreamKind::kAudio);
    if (stream == nullptr)
        return QStringLiteral("The selected input has no audio stream.");
    if (channelLayoutOptions().isEmpty())
        return ChannelLayoutUnavailableMessage(stream);

    if (!channel_layout_confirmed_)
        return {};
    if (spatial_audio_->channel_layout_detection_exact &&
        channel_layout_ == spatial_audio_->suggested_channel_layout)
        return QStringLiteral("Layout detected from speaker labels.");
    return QStringLiteral("Input layout confirmed.");
}

bool GuiController::channelLayoutDetected() const {
    return channelBasedMode() && spatial_audio_ &&
           channel_layout_ != app::ChannelLayout::kUnset && channel_layout_confirmed_ &&
           spatial_audio_->channel_layout_detection_exact &&
           channel_layout_ == spatial_audio_->suggested_channel_layout;
}

void GuiController::selectChannelLayout(const QString& layout_name) {
    if (job_running_ || !channelBasedMode() || !spatial_audio_)
        return;
    const app::ChannelLayout selected = ParseChannelLayoutTitle(layout_name);
    const QString selected_name = ChannelLayoutTitle(selected);
    if (selected == app::ChannelLayout::kUnset ||
        app::ChannelCount(selected) != ChannelCount(*spatial_audio_) ||
        std::find(spatial_audio_->channel_layout_candidates.begin(),
                  spatial_audio_->channel_layout_candidates.end(), selected) ==
            spatial_audio_->channel_layout_candidates.end() ||
        !channelLayoutOptions().contains(selected_name))
        return;
    channel_layout_ = selected;
    channel_layout_confirmed_ = true;
    RecalculateProject();
    emit projectStateChanged();
}
QString GuiController::standaloneIamfCodec() const {
    if (!iamfMode() || !spatial_audio_)
        return QStringLiteral("—");

    const app::MediaStreamInfo* stream =
        FirstStream(*spatial_audio_, app::MediaStreamKind::kAudio);
    if (stream == nullptr || stream->codec_name.empty())
        return QStringLiteral("UNKNOWN");

    const QString codec = QString::fromUtf8(stream->codec_name.c_str()).toLower();
    if (codec == QStringLiteral("opus"))
        return QStringLiteral("OPUS");
    if (codec == QStringLiteral("lpcm"))
        return QStringLiteral("LPCM");
    if (codec == QStringLiteral("aac") || codec == QStringLiteral("aac-lc"))
        return QStringLiteral("AAC-LC");
    if (codec == QStringLiteral("flac"))
        return QStringLiteral("FLAC");
    return codec.toUpper();
}
QString GuiController::headLockedStereoPath() const { return PathText(head_locked_stereo_); }
QString GuiController::headLockedStereoSummary() const {
    return MediaSummary(head_locked_stereo_);
}
QStringList GuiController::headLockedStereoTags() const {
    return TagsForInfo(head_locked_stereo_, false, app::AudioRole::kHeadLockedStereo);
}

int GuiController::ChannelCount(const app::MediaProbeInfo& info) {
    const auto it = std::find_if(info.streams.begin(), info.streams.end(),
                                 [](const app::MediaStreamInfo& s) {
                                     return s.kind == app::MediaStreamKind::kAudio;
                                 });
    return it == info.streams.end() ? 0 : it->channel_count;
}

void GuiController::SetStatus(const QString& status) {
    if (status_text_ == status)
        return;
    status_text_ = status;
    emit statusTextChanged();
}

void GuiController::setMixAnnotation(const QString& value) {
    if (mix_annotation_ == value)
        return;
    // Keep an empty value while the user is editing; the metadata builder
    // supplies its encoding-time fallback only when a job is started.
    mix_annotation_ = value;
    emit projectOptionsChanged();
}

void GuiController::setGeneratedCodec(int value) {
    if (value < 0 || value > 3)
        value = 0;
    if (generated_codec_ == value)
        return;
    generated_codec_ = value;
    emit projectOptionsChanged();
}
void GuiController::setVideoMappingPreset(int value) {
    if (value < static_cast<int>(immersive_audio_muxer::VideoMappingPreset::kPreserveSource) ||
        value > static_cast<int>(immersive_audio_muxer::VideoMappingPreset::kVr360TopBottom)) {
        value = static_cast<int>(immersive_audio_muxer::VideoMappingPreset::kPreserveSource);
    }
    if (video_mapping_preset_ == value)
        return;
    video_mapping_preset_ = value;
    emit projectOptionsChanged();
    RecalculateProject();
}


void GuiController::setIncludeVideo(bool enabled) {
    if (include_video_ == enabled)
        return;
    include_video_ = enabled;
    emit projectOptionsChanged();
    RecalculateProject();
}

void GuiController::setKeepMp4Audio(bool enabled) {
    if (keep_mp4_audio_ == enabled)
        return;
    keep_mp4_audio_ = enabled;
    emit projectOptionsChanged();
    RecalculateProject();
}

void GuiController::setOverwriteExistingFiles(bool enabled) {
    if (overwrite_existing_files_ == enabled)
        return;
    overwrite_existing_files_ = enabled;
    emit projectOptionsChanged();
    RecalculateProject();
}

QString GuiController::outputPathPlaceholder() const {
    return PathText(SuggestedOutputPath());
}

std::filesystem::path GuiController::SuggestedOutputPath() const {
    const std::filesystem::path source_path =
        video_ ? video_->path
               : (spatial_audio_ ? spatial_audio_->path
                                 : (head_locked_stereo_ ? head_locked_stereo_->path
                                                         : std::filesystem::path{}));
    if (source_path.empty())
        return {};

    const std::string source_stem = source_path.stem().u8string();
    const bool already_has_iamf_suffix =
        QString::fromUtf8(source_stem.c_str()).endsWith(QStringLiteral("_iamf"),
                                                         Qt::CaseInsensitive);
    const std::string output_base_name =
        source_stem + (already_has_iamf_suffix ? "_1" : "_iamf");

    const auto collides_with_input = [this](const std::filesystem::path& path) {
        return (video_ && SamePath(path, video_->path)) ||
               (spatial_audio_ && SamePath(path, spatial_audio_->path)) ||
               (head_locked_stereo_ && SamePath(path, head_locked_stereo_->path));
    };

    for (int suffix_index = 0;; ++suffix_index) {
        std::string output_name = output_base_name;
        if (suffix_index > 0)
            output_name += "_" + std::to_string(suffix_index);
        const auto candidate = source_path.parent_path() /
                               std::filesystem::u8path(output_name + ".mp4");
        if (!collides_with_input(candidate))
            return candidate;
    }
}

std::filesystem::path GuiController::EffectiveOutputPath() const {
    const QString manual_path = output_path_.trimmed();
    if (!manual_path.isEmpty())
        return std::filesystem::u8path(manual_path.toUtf8().toStdString());
    return SuggestedOutputPath();
}

void GuiController::UpdateAutomaticOutputPath() {
    if (output_path_manual_)
        return;

    const QString automatic_path = PathText(SuggestedOutputPath());
    if (output_path_ == automatic_path)
        return;

    updating_automatic_output_path_ = true;
    output_path_ = automatic_path;
    emit outputPathChanged();
    updating_automatic_output_path_ = false;
}

void GuiController::setOutputPath(const QString& path) {
    const bool value_changed = output_path_ != path;
    const bool manual_value = !path.trimmed().isEmpty();
    const bool manual_state_changed =
        !updating_automatic_output_path_ &&
        output_path_manual_ != manual_value;
    if (!value_changed && !manual_state_changed)
        return;

    output_path_ = path;
    if (!updating_automatic_output_path_)
        output_path_manual_ = manual_value;
    if (value_changed)
        emit outputPathChanged();
    if (value_changed || manual_state_changed)
        RecalculateProject();
}

void GuiController::openOutputLocation() {
    const std::filesystem::path target_path = EffectiveOutputPath();
    if (target_path.empty()) {
        SetStatus(QStringLiteral("Choose an input or output path first."));
        return;
    }

    std::error_code filesystem_error;
    const std::filesystem::path absolute_path =
        std::filesystem::absolute(target_path, filesystem_error);
    const QString target_text = PathText(filesystem_error ? target_path : absolute_path);
    const QFileInfo target(target_text);
    const bool target_exists = target.exists() && target.isFile();
    const QString folder = ExistingParentDirectory(target);
    bool opened = false;

#if defined(Q_OS_WIN)
    if (target_exists) {
        opened = SelectWindowsFile(target.absoluteFilePath());
    } else {
        opened = QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    }
#elif defined(Q_OS_MACOS)
    if (target_exists)
        opened = QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), target.absoluteFilePath()});
    else
        opened = QProcess::startDetached(QStringLiteral("open"), {folder});
#else
    opened = QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
#endif

    if (!opened) {
        SetStatus(QStringLiteral("Could not open the output location."));
        return;
    }
    SetStatus(target_exists ? QStringLiteral("Opened the output file location.")
                           : QStringLiteral("Opened the output folder."));
}

bool GuiController::SetVideoInput(const app::MediaProbeInfo& info, QString* error,
                                  bool replace_existing) {
    if (!modeSelected()) {
        if (error)
            *error = QStringLiteral("Select IAMF or Ambisonics before adding media inputs.");
        return false;
    }
    if (info.kind != app::InputKind::kVideoContainer || !info.HasVideo()) {
        if (error)
            *error = QStringLiteral("Video Input must contain a video stream.");
        return false;
    }
    if (video_ && !replace_existing) {
        if (error)
            *error = QStringLiteral("A video input is already configured. Clear it first.");
        return false;
    }
    video_ = info;
    return true;
}

bool GuiController::SetSpatialAudioInput(const app::MediaProbeInfo& info, QString* error,
                                         bool replace_existing) {
    if (!modeSelected()) {
        if (error)
            *error = QStringLiteral("Select an audio workflow before adding audio.");
        return false;
    }
    if (iamfMode()) {
        if (info.kind != app::InputKind::kStandaloneIamf) {
            if (error)
                *error = QStringLiteral("IAMF mode accepts only a standalone IAMF file in Spatial Audio Input.");
            return false;
        }
        if (spatial_audio_ && !replace_existing) {
            if (error)
                *error = QStringLiteral("A Spatial Audio input is already configured. Clear it first.");
            return false;
        }
        spatial_audio_ = info;
        const QString annotation = QString::fromUtf8(
            info.iamf_mix_presentation_annotation.c_str());
        const QString next_annotation = annotation.trimmed().isEmpty()
                                            ? QStringLiteral("default mix presentation")
                                            : annotation;
        if (mix_annotation_ != next_annotation) {
            mix_annotation_ = next_annotation;
            emit projectOptionsChanged();
        }
        return true;
    }

    if (channelBasedMode()) {
        if (info.kind != app::InputKind::kAudioCandidate ||
            info.channel_layout_candidates.empty()) {
            if (error)
                *error = QStringLiteral("Channel-based mode supports Mono, Stereo, 3.1.2, 5.1, 5.1.2, 5.1.4, 7.1, 7.1.2, or 7.1.4 audio.");
            return false;
        }
        if (spatial_audio_ && !replace_existing) {
            if (error)
                *error = QStringLiteral("A Channel-based input is already configured. Clear it first.");
            return false;
        }
        spatial_audio_ = info;
        channel_layout_ = info.channel_layout_detection_exact
                              ? info.suggested_channel_layout
                              : app::ChannelLayout::kUnset;
        channel_layout_confirmed_ = info.channel_layout_detection_exact;
        return true;
    }

    if (info.kind != app::InputKind::kAudioCandidate ||
        !app::IsAmbisonicsRole(info.suggested_audio_role)) {
        if (error)
            *error = QStringLiteral("Ambisonics mode accepts only 4, 9, or 16 channel Ambisonics audio.");
        return false;
    }
    if (spatial_audio_ && !replace_existing) {
        if (error)
            *error = QStringLiteral("A Spatial Audio input is already configured. Clear it first.");
        return false;
    }
    spatial_audio_ = info;
    return true;
}

bool GuiController::SetHeadLockedStereoInput(const app::MediaProbeInfo& info, QString* error,
                                             bool replace_existing) {
    if (!ambisonicsMode()) {
        if (error)
            *error = QStringLiteral("Head-Locked Stereo Audio is available only in Ambisonics mode.");
        return false;
    }
    if (info.kind != app::InputKind::kAudioCandidate || ChannelCount(info) != 2) {
        if (error)
            *error = QStringLiteral("Head-Locked Stereo Audio must contain exactly two channels.");
        return false;
    }
    if (head_locked_stereo_ && !replace_existing) {
        if (error)
            *error = QStringLiteral("A Head-Locked Stereo input is already configured. Clear it first.");
        return false;
    }
    head_locked_stereo_ = info;
    return true;
}

bool GuiController::AddProbedInput(const app::MediaProbeInfo& info, const QString& slot, QString* error,
                                   bool replace_existing) {
    if (slot == QStringLiteral("video"))
        return SetVideoInput(info, error, replace_existing);
    if (slot == QStringLiteral("spatial"))
        return SetSpatialAudioInput(info, error, replace_existing);
    if (slot == QStringLiteral("headlocked"))
        return SetHeadLockedStereoInput(info, error, replace_existing);
    if (slot != QStringLiteral("auto")) {
        if (error)
            *error = SlotError(slot);
        return false;
    }

    if (info.kind == app::InputKind::kVideoContainer)
        return SetVideoInput(info, error);
    if (iamfMode()) {
        if (info.kind == app::InputKind::kStandaloneIamf)
            return SetSpatialAudioInput(info, error);
        if (error)
            *error = QStringLiteral("IAMF mode accepts only a standalone IAMF file for Spatial Audio Input.");
        return false;
    }

    if (channelBasedMode() && info.kind == app::InputKind::kAudioCandidate &&
        !info.channel_layout_candidates.empty())
        return SetSpatialAudioInput(info, error);
    if (info.kind == app::InputKind::kAudioCandidate &&
        app::IsAmbisonicsRole(info.suggested_audio_role))
        return SetSpatialAudioInput(info, error);
    if (info.kind == app::InputKind::kAudioCandidate && ChannelCount(info) == 2)
        return SetHeadLockedStereoInput(info, error);
    if (error)
        *error = channelBasedMode()
                     ? QStringLiteral("Channel-based mode requires a supported 1, 2, 6, 8, 10, or 12 channel layout.")
                     : QStringLiteral("Ambisonics mode accepts 4, 9, or 16 channel Ambisonics audio, or exactly 2 channel Head-Locked Stereo Audio.");
    return false;
}

bool GuiController::canAcceptInputPath(const QString& path, const QString& slot) const {
    if (job_running_ || path.isEmpty())
        return false;
    if (!modeSelected())
        return false;

    const auto native_path = std::filesystem::u8path(path.toUtf8().toStdString());
    const QString normalized_slot = slot.toLower();
    const bool looks_like_iamf = LooksLikeStandaloneIamf(native_path);

    // Drag feedback must stay cheap. Formal validation still happens on drop.
    if (normalized_slot == QStringLiteral("spatial") && iamfMode())
        return looks_like_iamf;
    if (looks_like_iamf)
        return false;

    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(native_path, filesystem_error) || filesystem_error)
        return false;

    // Hover feedback must not open/scan arbitrary media. The drop handler still
    // performs the authoritative FFmpeg probe and rejects mismatched streams.
    const QString extension = QFileInfo(path).suffix().toLower();
    static const QStringList video_extensions = {
        QStringLiteral("mp4"),  QStringLiteral("m4v"),  QStringLiteral("mov"),  QStringLiteral("mkv"),
        QStringLiteral("webm"), QStringLiteral("avi"),  QStringLiteral("ts"),   QStringLiteral("m2ts"),
        QStringLiteral("mts"),  QStringLiteral("mxf"),  QStringLiteral("3gp"),  QStringLiteral("3g2"),
        QStringLiteral("flv"),  QStringLiteral("f4v"),  QStringLiteral("wmv"),  QStringLiteral("asf"),
        QStringLiteral("ogv"),  QStringLiteral("mpeg"), QStringLiteral("mpg"),  QStringLiteral("m2v"),
        QStringLiteral("vob"),  QStringLiteral("ivf")};
    static const QStringList audio_extensions = {
        QStringLiteral("wav"),  QStringLiteral("wave"), QStringLiteral("flac"), QStringLiteral("ogg"),
        QStringLiteral("oga"),  QStringLiteral("opus"), QStringLiteral("mp3"),  QStringLiteral("aac"),
        QStringLiteral("m4a"),  QStringLiteral("mka"),  QStringLiteral("webm"), QStringLiteral("caf"),
        QStringLiteral("aif"),  QStringLiteral("aiff"), QStringLiteral("alac"), QStringLiteral("wma"),
        QStringLiteral("amr"),  QStringLiteral("ac3"),  QStringLiteral("eac3"),
        QStringLiteral("mxf"), QStringLiteral("w64")};
    const bool known_video_extension = video_extensions.contains(extension);
    const bool known_audio_extension = audio_extensions.contains(extension);

    if (normalized_slot == QStringLiteral("video"))
        return known_video_extension || !known_audio_extension;

    if (normalized_slot == QStringLiteral("spatial"))
        return known_audio_extension || !known_video_extension;

    if (normalized_slot == QStringLiteral("headlocked"))
        return ambisonicsMode() &&
               (known_audio_extension || !known_video_extension);

    return false;
}

bool GuiController::addInputFile(const QString& path, const QString& slot) {
    if (job_running_)
        return false;
    IAMF_MUX_LOG_INFO("[GUI] input selected: slot={} path={}",
                      slot.toUtf8().constData(), path.toUtf8().constData());
    if (path.isEmpty()) {
        SetStatus(QStringLiteral("Could not access the selected file."));
        IAMF_MUX_LOG_WARN("[GUI] input selection was empty");
        emit projectStateChanged();
        return false;
    }
    if (!modeSelected()) {
        SetStatus(QStringLiteral("Select an audio workflow before adding media inputs."));
        IAMF_MUX_LOG_WARN("[GUI] rejected input because no workflow mode is selected: path={}",
                          path.toUtf8().constData());
        emit projectStateChanged();
        return false;
    }
    app::MediaProbeInfo info;
    std::string error;
    const auto native_path = std::filesystem::u8path(path.toUtf8().toStdString());
    if (!app::MediaProbeService::Probe(native_path, &info, &error)) {
        SetStatus(QStringLiteral("Probe failed: ") + QString::fromUtf8(error.c_str()));
        emit projectStateChanged();
        return false;
    }
    QString slot_error;
    const QString normalized_slot = slot.toLower();
    const bool replace_existing = normalized_slot == QStringLiteral("video") ||
                                  normalized_slot == QStringLiteral("spatial") ||
                                  normalized_slot == QStringLiteral("headlocked");
    if (!AddProbedInput(info, normalized_slot, &slot_error, replace_existing)) {
        SetStatus(slot_error);
        IAMF_MUX_LOG_ERROR("[GUI] input could not be assigned to slot {}: path={} error={}",
                           normalized_slot.toUtf8().constData(), path.toUtf8().constData(),
                           slot_error.toUtf8().constData());
        emit projectStateChanged();
        return false;
    }
    RecalculateProject();
    SetStatus(QStringLiteral("Detected ") + detectedInputType() + QStringLiteral(": ") +
             QFileInfo(path).fileName() + QStringLiteral("."));
    IAMF_MUX_LOG_INFO("[GUI] input accepted: slot={} path={} detected_type={}",
                      normalized_slot.toUtf8().constData(), path.toUtf8().constData(),
                      detectedInputType().toUtf8().constData());
    emit projectStateChanged();
    return true;
}

bool GuiController::addInputFilesToSlot(const QStringList& paths, const QString& slot) {
    if (job_running_ || paths.isEmpty())
        return false;

    int failed_count = 0;
    for (const QString& path : paths) {
        if (slot.toLower() != QStringLiteral("headlocked"))
            clearInput(slot);
        if (!addInputFile(path, slot))
            ++failed_count;
    }

    if (failed_count == 0)
        return true;

    if (paths.size() > 1) {
        SetStatus(QStringLiteral("%1 of %2 dropped files could not be loaded.")
                      .arg(failed_count)
                      .arg(paths.size()));
        emit projectStateChanged();
    }
    return false;
}

void GuiController::addFiles(const QStringList& paths) {
    for (const QString& path : paths)
        addInputFile(path, QStringLiteral("auto"));
}

void GuiController::clearInput(const QString& slot) {
    if (job_running_)
        return;
    if (slot == QStringLiteral("video")) {
        video_.reset();
    } else if (slot == QStringLiteral("spatial")) {
        spatial_audio_.reset();
        channel_layout_ = app::ChannelLayout::kUnset;
        channel_layout_confirmed_ = false;
        mix_annotation_ = QStringLiteral("default mix presentation");
        emit projectOptionsChanged();
    } else if (slot == QStringLiteral("headlocked")) {
        head_locked_stereo_.reset();
    } else {
        SetStatus(SlotError(slot));
        return;
    }
    RecalculateProject();
    SetStatus(QStringLiteral("Input cleared."));
    emit projectStateChanged();
}

void GuiController::RecalculateProject() {
    UpdateAutomaticOutputPath();
    project_ = {};
    std::string ignored;
    if (video_)
        project_.AddInput(*video_, &ignored);
    if (modeSelected() && spatial_audio_) {
        project_.AddInput(*spatial_audio_, &ignored);
        if (iamfMode()) {
            project_.RecalculateMode();
        } else if (ambisonicsMode()) {
            project_.AssignAudioRole(spatial_audio_->path, spatial_audio_->suggested_audio_role,
                                     true, &ignored);
            if (head_locked_stereo_) {
                project_.AddInput(*head_locked_stereo_, &ignored);
                project_.AssignAudioRole(head_locked_stereo_->path, app::AudioRole::kHeadLockedStereo,
                                         true, &ignored);
            }
        } else if (channelBasedMode()) {
            project_.AssignChannelLayout(spatial_audio_->path, channel_layout_,
                                         channel_layout_confirmed_, &ignored);
        }
    }
    project_.video_mapping.preset =
        static_cast<immersive_audio_muxer::VideoMappingPreset>(video_mapping_preset_);
    project_.keep_mp4_audio = keep_mp4_audio_;
    project_.overwrite_existing_files = overwrite_existing_files_;
    project_.output_path = EffectiveOutputPath();

    std::string error;
    bool ready = false;
    if (modeSelected() && !project_.output_path.empty()) {
        if (iamfMode())
            ready = project_.ValidateForExistingIamfMux(&error);
        else if (ambisonicsMode())
            ready = project_.ValidateForAudioEncoding(&error);
        else if (channelBasedMode())
            ready = project_.ValidateForChannelBasedEncoding(&error);
    }
    if (project_ready_ != ready) {
        project_ready_ = ready;
        emit projectStateChanged();
    }
}

void GuiController::StartBackgroundJob(std::function<bool(std::string*)> operation,
                                       const QString& start_status) {
    if (job_running_ || worker_.joinable() || !operation)
        return;
    cancel_requested_.store(false);
    job_running_ = true;
    emit jobStateChanged();
    emit projectStateChanged();
    SetStatus(start_status);
    const auto started = std::chrono::steady_clock::now();
    IAMF_MUX_LOG_INFO("[GUI][BackgroundJob] started: status={}",
                      start_status.toUtf8().constData());

    worker_ = std::thread([this, operation = std::move(operation), started]() mutable {
        std::string error;
        bool success = false;
        try {
            success = operation(&error);
        } catch (const std::exception& exception) {
            error = std::string("background operation threw an exception: ") + exception.what();
        } catch (...) {
            error = "background operation threw an unknown exception";
        }
        if (!success && error.empty())
            error = "background operation failed without an error message";
        const bool cancelled = cancel_requested_.load();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (success) {
            IAMF_MUX_LOG_INFO("[GUI][BackgroundJob] completed successfully in {} ms", elapsed);
        } else if (cancelled) {
            IAMF_MUX_LOG_WARN("[GUI][BackgroundJob] cancelled after {} ms: {}", elapsed, error);
        } else {
            IAMF_MUX_LOG_ERROR("[GUI][BackgroundJob] failed after {} ms: {}", elapsed, error);
        }
        QString message;
        if (success) {
            message = QStringLiteral("IAMF MP4 mux completed.");
        } else if (cancelled) {
            message = QStringLiteral("IAMF MP4 mux cancelled.");
        } else {
            message = QStringLiteral("IAMF MP4 mux failed: ") + QString::fromUtf8(error.c_str());
        }
        QMetaObject::invokeMethod(this, [this, success, message]() {
            if (worker_.joinable())
                worker_.join();
            job_running_ = false;
            cancel_requested_.store(false);
            SetStatus(message);
            emit jobStateChanged();
            emit projectStateChanged();
            emit jobFinished(success, message);
        }, Qt::QueuedConnection);
    });
}

void GuiController::StartExistingIamfMux() {
    if (job_running_ || worker_.joinable())
        return;
    RecalculateProject();
    if (!project_ready_ || !project_.standalone_iamf) {
        SetStatus(QStringLiteral("Complete the IAMF input and choose an output path."));
        return;
    }

    MuxJobRequest request;
    request.iamf_path = project_.standalone_iamf->path;
    if (include_video_ && project_.video) {
        request.video_path = project_.video->path;
        if (const app::MediaStreamInfo* stream = FirstStream(*project_.video, app::MediaStreamKind::kVideo))
            request.video_stream_index = stream->index;
    }
    request.output_path = project_.output_path;
    request.keep_mp4_audio = !request.video_path.empty() && project_.keep_mp4_audio;
    request.overwrite_existing_files = project_.overwrite_existing_files;
    request.video_mapping = project_.video_mapping;
    request.cancellation_requested = [this]() {
        return cancel_requested_.load();
    };
    request.event_callback = [this](const MuxJobEvent& event) {
        const QString message = QString::fromUtf8(event.message.c_str());
        QMetaObject::invokeMethod(this, [this, message]() {
            if (job_running_)
                SetStatus(message);
        }, Qt::QueuedConnection);
    };
    StartBackgroundJob([request = std::move(request)](std::string* error) mutable {
        MuxJobResult result;
        return MuxJob::Run(request, &result, error);
    }, QStringLiteral("Starting IAMF MP4 mux..."));
}

void GuiController::StartAmbisonicsMux() {
    StartAudioEncodingMux(false);
}

void GuiController::StartChannelBasedMux() {
    StartAudioEncodingMux(true);
}

void GuiController::StartAudioEncodingMux(bool channel_based) {
    if (job_running_ || worker_.joinable())
        return;
    RecalculateProject();
    if (!project_ready_ || project_.audio_inputs.empty()) {
        SetStatus(channel_based
                      ? QStringLiteral("Complete the Channel-based input, confirm its layout, and choose an output path.")
                      : QStringLiteral("Complete the Ambisonics input and choose an output path."));
        return;
    }

    const app::MuxProject project_snapshot = project_;
    const bool include_video = include_video_;
    const int generated_codec = generated_codec_;
    const std::string mix_annotation = mix_annotation_.toUtf8().toStdString();
    const auto audio_it = std::find_if(
        project_snapshot.audio_inputs.begin(), project_snapshot.audio_inputs.end(),
        [channel_based](const app::ProjectAudioInput& input) {
            return channel_based ? input.channel_layout != app::ChannelLayout::kUnset
                                 : app::IsAmbisonicsRole(input.role);
        });
    if (audio_it == project_snapshot.audio_inputs.end()) {
        SetStatus(channel_based ? QStringLiteral("No valid Channel-based input is configured.")
                                : QStringLiteral("No valid Ambisonics input is configured."));
        return;
    }
    const app::ProjectAudioInput primary_input = *audio_it;
    std::optional<app::ProjectAudioInput> head_locked_input;
    if (!channel_based) {
        const auto head_locked_it = std::find_if(
            project_snapshot.audio_inputs.begin(), project_snapshot.audio_inputs.end(),
            [](const app::ProjectAudioInput& input) {
                return input.role == app::AudioRole::kHeadLockedStereo;
            });
        if (head_locked_it != project_snapshot.audio_inputs.end())
            head_locked_input = *head_locked_it;
    }

    std::vector<int> channel_order;
    if (channel_based) {
        std::string mapping_error;
        if (!app::BuildChannelOrderForIamf(primary_input.channel_layout, &channel_order,
                                           &mapping_error)) {
            SetStatus(QStringLiteral("Cannot map the source channel order: ") +
                      QString::fromUtf8(mapping_error.c_str()));
            return;
        }
    }

    std::error_code temp_error;
    const std::filesystem::path temp_root = std::filesystem::temp_directory_path(temp_error);
    if (temp_error) {
        SetStatus(QStringLiteral("Cannot locate a temporary workspace: ") +
                  QString::fromUtf8(temp_error.message().c_str()));
        return;
    }
    const auto unique_value = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path workspace =
        temp_root / "iamf_muxer" / ("job_" + std::to_string(unique_value));
    std::filesystem::path textproto_path = project_snapshot.output_path;
    textproto_path.replace_extension(".textproto");
    if ((project_snapshot.video && SamePath(textproto_path, project_snapshot.video->path)) ||
        std::any_of(project_snapshot.audio_inputs.begin(), project_snapshot.audio_inputs.end(),
                    [&textproto_path](const app::ProjectAudioInput& input) {
                        return SamePath(textproto_path, input.media.path);
                    })) {
        SetStatus(QStringLiteral("The generated metadata path would overwrite an input file. Choose another output path."));
        return;
    }
    const std::filesystem::path encoder_metadata_path = workspace / "generated.textproto";
    auto post_status = [this](const QString& status) {
        IAMF_MUX_LOG_INFO("[GUI][TaskStatus] {}", status.toUtf8().constData());
        QMetaObject::invokeMethod(this, [this, status]() {
            if (job_running_)
                SetStatus(status);
        }, Qt::QueuedConnection);
    };

    const char* generated_codec_name = generated_codec == 1 ? "LPCM"
                                      : generated_codec == 2 ? "AAC-LC"
                                      : generated_codec == 3 ? "FLAC"
                                                            : "Opus";
    IAMF_MUX_LOG_INFO(
        "[GUI] starting encoded audio mux: mode={} audio_role={} channel_layout={} generated_codec={} include_video={} video_mapping={} keep_mp4_audio={} overwrite_existing={} output={} mix_annotation={}",
        channel_based ? "channel-based" : "ambisonics",
        app::ToString(primary_input.role), app::ToString(primary_input.channel_layout),
        generated_codec_name, include_video,
        VideoMappingPresetLabel(project_snapshot.video_mapping.preset),
        project_snapshot.keep_mp4_audio, project_snapshot.overwrite_existing_files,
        project_snapshot.output_path.u8string(), mix_annotation);
    for (const app::ProjectAudioInput& input : project_snapshot.audio_inputs) {
        IAMF_MUX_LOG_INFO(
            "[GUI] audio source: path={} role={} channel_layout={} role_confirmed={} layout_confirmed={}",
            input.media.path.u8string(), app::ToString(input.role),
            app::ToString(input.channel_layout), input.role_confirmed,
            input.channel_layout_confirmed);
    }
    if (project_snapshot.video) {
        IAMF_MUX_LOG_INFO("[GUI] video source: path={} included={}",
                          project_snapshot.video->path.u8string(), include_video);
    }

    StartBackgroundJob(
        [this, project_snapshot, primary_input, head_locked_input, channel_based, channel_order,
         include_video, generated_codec, mix_annotation, workspace, textproto_path,
         encoder_metadata_path, post_status](std::string* error) mutable {
            WorkspaceCleanupGuard workspace_cleanup(workspace);
            std::error_code filesystem_error;
            const std::filesystem::path audio_directory = workspace / "audio";
            const std::filesystem::path encoded_directory = workspace / "encoded";
            std::filesystem::create_directories(audio_directory, filesystem_error);
            if (filesystem_error) {
                if (error)
                    *error = "cannot create audio workspace: " + filesystem_error.message();
                return false;
            }
            std::filesystem::create_directories(encoded_directory, filesystem_error);
            if (filesystem_error) {
                if (error)
                    *error = "cannot create IAMF workspace: " + filesystem_error.message();
                return false;
            }
            auto prepare_audio = [&](const app::ProjectAudioInput& input,
                                     const std::filesystem::path& output_path,
                                     const QString& label,
                                     const std::vector<int>* output_channel_to_input,
                                     int target_pcm_bit_depth,
                                     app::PreparedAudioInfo* prepared) {
                post_status(QStringLiteral("Preparing ") + label + QStringLiteral(" audio..."));
                app::AudioPreparationOptions options;
                options.input_path = input.media.path;
                if (const app::MediaStreamInfo* stream = FirstStream(input.media, app::MediaStreamKind::kAudio))
                    options.stream_index = stream->index;
                options.output_wav_path = output_path;
                options.output_sample_rate = 48000;
                options.precision_policy = generated_codec == 2
                                               ? app::AudioPreparationPrecisionPolicy::kForcePcmS16
                                               : app::AudioPreparationPrecisionPolicy::kPreserveSource;
                options.target_pcm_bit_depth = target_pcm_bit_depth;
                options.preserve_unspecified_channel_order = channel_based;
                if (output_channel_to_input != nullptr)
                    options.output_channel_to_input = *output_channel_to_input;
                options.cancellation_requested = [this]() {
                    return cancel_requested_.load();
                };
                std::string preparation_error;
                if (!app::AudioPreparationService::PrepareToWav(options, prepared, &preparation_error)) {
                    if (error)
                        *error = label.toUtf8().toStdString() + " preparation failed: " + preparation_error;
                    return false;
                }
                return true;
            };

            const std::filesystem::path primary_wav =
                audio_directory / (channel_based ? "channel_based.wav" : "ambisonics.wav");
            app::PreparedAudioInfo prepared_primary;
            if (!prepare_audio(primary_input, primary_wav,
                               channel_based ? QStringLiteral("Channel-based")
                                             : QStringLiteral("Ambisonics"),
                               channel_based ? &channel_order : nullptr, 0, &prepared_primary))
                return false;
            std::optional<app::PreparedAudioInfo> prepared_head_locked;
            const std::filesystem::path head_locked_wav = audio_directory / "head_locked_stereo.wav";
            if (head_locked_input) {
                prepared_head_locked.emplace();
                if (!prepare_audio(*head_locked_input, head_locked_wav,
                                   QStringLiteral("Head-Locked Stereo"), nullptr, 0,
                                   &*prepared_head_locked))
                    return false;
            }
            const bool same_pcm_codec = generated_codec == 1 || generated_codec == 3;
            if (same_pcm_codec && head_locked_input && prepared_head_locked) {
                const app::SharedPcmPreparationPlan shared_pcm_plan =
                    app::PlanSharedPcmPreparation(prepared_primary, *prepared_head_locked);
                if (shared_pcm_plan.reprepare_first) {
                    if (!prepare_audio(primary_input, primary_wav,
                                       channel_based ? QStringLiteral("Channel-based")
                                                     : QStringLiteral("Ambisonics"),
                                       channel_based ? &channel_order : nullptr,
                                       shared_pcm_plan.target_bit_depth, &prepared_primary))
                        return false;
                }
                if (shared_pcm_plan.reprepare_second) {
                    if (!prepare_audio(*head_locked_input, head_locked_wav,
                                       QStringLiteral("Head-Locked Stereo"), nullptr,
                                       shared_pcm_plan.target_bit_depth, &*prepared_head_locked))
                        return false;
                }
            }
            if (cancel_requested_.load()) {
                if (error)
                    *error = "operation cancelled while preparing audio";
                return false;
            }

            app::MetadataBuildOptions metadata;
            if (channel_based) {
                metadata.channel_based = primary_input;
                metadata.channel_based.media.path = prepared_primary.wav_path;
                metadata.channel_based_bit_depth = prepared_primary.bit_depth;
            } else {
                metadata.ambisonics = primary_input;
                metadata.ambisonics.media.path = prepared_primary.wav_path;
                metadata.ambisonics_bit_depth = prepared_primary.bit_depth;
            }
            if (head_locked_input && prepared_head_locked) {
                metadata.head_locked_stereo = *head_locked_input;
                metadata.head_locked_stereo.media.path = prepared_head_locked->wav_path;
                metadata.head_locked_stereo_bit_depth = prepared_head_locked->bit_depth;
            }
            metadata.output_path = textproto_path;
            metadata.overwrite_existing_files = project_snapshot.overwrite_existing_files;
            switch (generated_codec) {
                case 1:
                    metadata.generated_codec = app::GeneratedCodec::kLpcm;
                    break;
                case 2:
                    metadata.generated_codec = app::GeneratedCodec::kAacLc;
                    break;
                case 3:
                    metadata.generated_codec = app::GeneratedCodec::kFlac;
                    break;
                default:
                    metadata.generated_codec = app::GeneratedCodec::kOpus;
                    break;
            }
            metadata.mix_annotation = mix_annotation;
            post_status(QStringLiteral("Writing IAMF metadata..."));
            const bool metadata_written = channel_based
                                              ? app::MetadataBuilder::WriteChannelBased(metadata, error)
                                              : app::MetadataBuilder::WriteAmbisonicsAndHeadLockedStereo(metadata, error);
            if (!metadata_written)
                return false;
            filesystem_error.clear();
            const bool metadata_staged = std::filesystem::copy_file(
                textproto_path, encoder_metadata_path,
                std::filesystem::copy_options::overwrite_existing, filesystem_error);
            if (!metadata_staged || filesystem_error) {
                if (error) {
                    *error = "cannot stage IAMF metadata for official encoder: " +
                             (filesystem_error ? filesystem_error.message()
                                                : std::string("copy operation failed"));
                }
                return false;
            }

            InProcessIamfEncoderOptions encoder_options;
            encoder_options.user_metadata = encoder_metadata_path;
            encoder_options.input_wav_directory = audio_directory;
            encoder_options.output_iamf_directory = encoded_directory;
            encoder_options.cancellation_requested = [this]() {
                return cancel_requested_.load();
            };
            std::filesystem::path encoded_iamf;
            std::string encoder_output;
            post_status(QStringLiteral("Encoding IAMF with official encoder..."));
            if (!InProcessIamfEncoder::Encode(encoder_options, &encoded_iamf, error, &encoder_output)) {
                if (error && !encoder_output.empty())
                    *error += "\nOfficial encoder output:\n" + encoder_output;
                return false;
            }
            post_status(QStringLiteral("Official encoder completed. Validating MP4 inputs..."));

            MuxJobRequest request;
            request.iamf_path = encoded_iamf;
            if (include_video && project_snapshot.video) {
                request.video_path = project_snapshot.video->path;
                if (const app::MediaStreamInfo* stream = FirstStream(*project_snapshot.video, app::MediaStreamKind::kVideo))
                    request.video_stream_index = stream->index;
            }
            request.output_path = project_snapshot.output_path;
            request.keep_mp4_audio = !request.video_path.empty() && project_snapshot.keep_mp4_audio;
            request.overwrite_existing_files = project_snapshot.overwrite_existing_files;
            request.video_mapping = project_snapshot.video_mapping;
            request.cancellation_requested = [this]() {
                return cancel_requested_.load();
            };
            request.event_callback = [post_status](const MuxJobEvent& event) {
                post_status(QString::fromUtf8(event.message.c_str()));
            };
            MuxJobResult result;
            if (!MuxJob::Run(request, &result, error))
                return false;

            return true;
        },
        channel_based ? QStringLiteral("Starting Channel-based IAMF encode...")
                      : QStringLiteral("Starting Ambisonics IAMF encode..."));
}

void GuiController::cancelJob() {
    if (!job_running_)
        return;
    cancel_requested_.store(true);
    SetStatus(QStringLiteral("Cancelling IAMF MP4 mux..."));
}

void GuiController::prepareProject() {
    if (job_running_)
        return;
    if (!modeSelected()) {
        SetStatus(QStringLiteral("Select an audio workflow before continuing."));
        return;
    }
    RecalculateProject();
    if (!project_ready_) {
        SetStatus(QStringLiteral("Complete the selected workflow inputs and choose an output path."));
        return;
    }
    if (iamfMode())
        StartExistingIamfMux();
    else if (ambisonicsMode())
        StartAmbisonicsMux();
    else
        StartChannelBasedMux();
}

}  // namespace immersive_audio_muxer::gui
