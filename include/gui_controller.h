#pragma once

#include "app/project_types.h"
#include "gui_input_model.h"

#include <QObject>
#include <QStringList>

#include <atomic>
#include <functional>
#include <optional>
#include <thread>

namespace immersive_audio_muxer::gui {

class GuiController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(QString detectedInputType READ detectedInputType NOTIFY projectStateChanged)
    Q_PROPERTY(int inputMode READ inputMode NOTIFY projectStateChanged)
    Q_PROPERTY(bool iamfMode READ iamfMode NOTIFY projectStateChanged)
    Q_PROPERTY(bool ambisonicsMode READ ambisonicsMode NOTIFY projectStateChanged)
    Q_PROPERTY(bool channelBasedMode READ channelBasedMode NOTIFY projectStateChanged)
    Q_PROPERTY(bool modeSelected READ modeSelected NOTIFY projectStateChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY projectStateChanged)
    Q_PROPERTY(bool videoHasAudio READ videoHasAudio NOTIFY projectStateChanged)
    Q_PROPERTY(QString videoPath READ videoPath NOTIFY projectStateChanged)
    Q_PROPERTY(QString videoSummary READ videoSummary NOTIFY projectStateChanged)
    Q_PROPERTY(QStringList videoTags READ videoTags NOTIFY projectStateChanged)
    Q_PROPERTY(QString spatialAudioPath READ spatialAudioPath NOTIFY projectStateChanged)
    Q_PROPERTY(QString spatialAudioSummary READ spatialAudioSummary NOTIFY projectStateChanged)
    Q_PROPERTY(QStringList spatialAudioTags READ spatialAudioTags NOTIFY projectStateChanged)
    Q_PROPERTY(QStringList channelLayoutOptions READ channelLayoutOptions NOTIFY projectStateChanged)
    Q_PROPERTY(QString channelLayoutName READ channelLayoutName NOTIFY projectStateChanged)
    Q_PROPERTY(QString channelLayoutStatus READ channelLayoutStatus NOTIFY projectStateChanged)
    Q_PROPERTY(bool channelLayoutDetected READ channelLayoutDetected NOTIFY projectStateChanged)
    Q_PROPERTY(QString channelOrderDescription READ channelOrderDescription NOTIFY projectStateChanged)
    Q_PROPERTY(bool channelLayoutConfirmed READ channelLayoutConfirmed NOTIFY projectStateChanged)
    Q_PROPERTY(QString standaloneIamfCodec READ standaloneIamfCodec NOTIFY projectStateChanged)
    Q_PROPERTY(QString headLockedStereoPath READ headLockedStereoPath NOTIFY projectStateChanged)
    Q_PROPERTY(QString headLockedStereoSummary READ headLockedStereoSummary NOTIFY projectStateChanged)
    Q_PROPERTY(QStringList headLockedStereoTags READ headLockedStereoTags NOTIFY projectStateChanged)
    Q_PROPERTY(QString mixAnnotation READ mixAnnotation WRITE setMixAnnotation NOTIFY projectOptionsChanged)
    Q_PROPERTY(int generatedCodec READ generatedCodec WRITE setGeneratedCodec NOTIFY projectOptionsChanged)
    Q_PROPERTY(int videoMappingPreset READ videoMappingPreset WRITE setVideoMappingPreset NOTIFY projectOptionsChanged)
    Q_PROPERTY(bool includeVideo READ includeVideo WRITE setIncludeVideo NOTIFY projectOptionsChanged)
    Q_PROPERTY(bool keepMp4Audio READ keepMp4Audio WRITE setKeepMp4Audio NOTIFY projectOptionsChanged)
    Q_PROPERTY(bool overwriteExistingFiles READ overwriteExistingFiles WRITE setOverwriteExistingFiles NOTIFY projectOptionsChanged)
    Q_PROPERTY(QString outputPath READ outputPath WRITE setOutputPath NOTIFY outputPathChanged)
    Q_PROPERTY(QString outputPathPlaceholder READ outputPathPlaceholder NOTIFY projectStateChanged)
    Q_PROPERTY(bool jobRunning READ jobRunning NOTIFY jobStateChanged)
    Q_PROPERTY(bool projectReady READ projectReady NOTIFY projectStateChanged)

public:
    ~GuiController() override;

    explicit GuiController(QObject* parent = nullptr);

    QString statusText() const { return status_text_; }
    QString detectedInputType() const;
    int inputMode() const { return input_mode_; }
    bool iamfMode() const;
    bool ambisonicsMode() const;
    bool channelBasedMode() const;
    bool modeSelected() const { return input_mode_ >= 0; }
    bool hasVideo() const { return video_.has_value(); }
    bool videoHasAudio() const;
    QString videoPath() const;
    QString videoSummary() const;
    QStringList videoTags() const;
    QString spatialAudioPath() const;
    QString spatialAudioSummary() const;
    QStringList spatialAudioTags() const;
    QStringList channelLayoutOptions() const;
    QString channelLayoutName() const;
    QString channelLayoutStatus() const;
    bool channelLayoutDetected() const;
    QString channelOrderDescription() const;
    bool channelLayoutConfirmed() const { return channel_layout_confirmed_; }
    QString standaloneIamfCodec() const;
    QString headLockedStereoPath() const;
    QString headLockedStereoSummary() const;
    QStringList headLockedStereoTags() const;
    QString mixAnnotation() const { return mix_annotation_; }
    void setMixAnnotation(const QString& value);
    int generatedCodec() const { return generated_codec_; }
    void setGeneratedCodec(int value);
    int videoMappingPreset() const { return video_mapping_preset_; }
    void setVideoMappingPreset(int value);
    bool includeVideo() const { return include_video_; }
    void setIncludeVideo(bool enabled);
    bool keepMp4Audio() const { return keep_mp4_audio_; }
    void setKeepMp4Audio(bool enabled);
    bool overwriteExistingFiles() const { return overwrite_existing_files_; }
    void setOverwriteExistingFiles(bool enabled);
    QString outputPath() const { return output_path_; }
    QString outputPathPlaceholder() const;
    bool jobRunning() const { return job_running_; }
    void setOutputPath(const QString& path);
    bool projectReady() const { return project_ready_; }

    Q_INVOKABLE void setInputMode(int mode);
    Q_INVOKABLE void selectChannelLayout(const QString& layout_name);
    Q_INVOKABLE bool canAcceptInputPath(const QString& path, const QString& slot) const;
    Q_INVOKABLE bool addInputFile(const QString& path, const QString& slot);
    Q_INVOKABLE bool addInputFilesToSlot(const QStringList& paths, const QString& slot);
    Q_INVOKABLE void addFiles(const QStringList& paths);
    Q_INVOKABLE void cancelJob();
    Q_INVOKABLE void clearInput(const QString& slot);
    Q_INVOKABLE void openOutputLocation();
    Q_INVOKABLE void prepareProject();

signals:
    void statusTextChanged();
    void outputPathChanged();
    void jobStateChanged();
    void jobFinished(bool success, const QString& message);
    void projectStateChanged();
    void projectOptionsChanged();

private:
    void SetStatus(const QString& status);
    bool AddProbedInput(const app::MediaProbeInfo& info, const QString& slot, QString* error,
                        bool replace_existing = false);
    bool SetVideoInput(const app::MediaProbeInfo& info, QString* error, bool replace_existing = false);
    bool SetSpatialAudioInput(const app::MediaProbeInfo& info, QString* error,
                              bool replace_existing = false);
    void StartExistingIamfMux();
    void StartAmbisonicsMux();
    void StartChannelBasedMux();
    void StartAudioEncodingMux(bool channel_based);
    void StartBackgroundJob(std::function<bool(std::string*)> operation,
                           const QString& start_status);
    bool SetHeadLockedStereoInput(const app::MediaProbeInfo& info, QString* error,
                                  bool replace_existing = false);
    void RecalculateProject();
    void UpdateAutomaticOutputPath();
    std::filesystem::path SuggestedOutputPath() const;
    std::filesystem::path EffectiveOutputPath() const;
    static int ChannelCount(const app::MediaProbeInfo& info);

    app::MuxProject project_;
    std::optional<app::MediaProbeInfo> video_;
    std::optional<app::MediaProbeInfo> spatial_audio_;
    std::optional<app::MediaProbeInfo> head_locked_stereo_;
    QString status_text_ = QStringLiteral("Choose IAMF or Ambisonics to begin.");
    QString output_path_;
    bool output_path_manual_ = false;
    bool updating_automatic_output_path_ = false;
    QString mix_annotation_ = QStringLiteral("default mix presentation");
    int generated_codec_ = 0;
    int video_mapping_preset_ = static_cast<int>(immersive_audio_muxer::VideoMappingPreset::kPreserveSource);
    bool include_video_ = true;
    bool keep_mp4_audio_ = false;
    bool overwrite_existing_files_ = true;
    std::atomic_bool cancel_requested_{false};
    bool job_running_ = false;
    std::thread worker_;
    int input_mode_ = -1;
    app::ChannelLayout channel_layout_ = app::ChannelLayout::kUnset;
    bool channel_layout_confirmed_ = false;
    bool project_ready_ = false;
};

}  // namespace immersive_audio_muxer::gui
