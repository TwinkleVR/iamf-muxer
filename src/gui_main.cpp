#include "gui_controller.h"
#include "iamf_tools_version.h"
#include "avformat_input.h"
#include "runtime_logging.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QMargins>
#include <QPoint>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QRect>
#include <QScreen>
#include <QStandardPaths>

extern "C" {
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

#ifndef IMMERSIVE_AUDIO_MUXER_VERSION
#define IMMERSIVE_AUDIO_MUXER_VERSION "unknown"
#endif
#ifndef IMMERSIVE_AUDIO_MUXER_SPDLOG_VERSION
#define IMMERSIVE_AUDIO_MUXER_SPDLOG_VERSION "unknown"
#endif

namespace {

QtMessageHandler g_previous_qt_message_handler = nullptr;

std::filesystem::path PathFromQString(const QString& path) {
#if defined(Q_OS_WIN)
    return std::filesystem::path(path.toStdWString());
#else
    const QByteArray utf8 = path.toUtf8();
    return std::filesystem::u8path(utf8.constData());
#endif
}

spdlog::level::level_enum QtLogLevel(QtMsgType type) {
    switch (type) {
        case QtDebugMsg:
            return spdlog::level::debug;
        case QtInfoMsg:
            return spdlog::level::info;
        case QtWarningMsg:
            return spdlog::level::warn;
        case QtCriticalMsg:
            return spdlog::level::err;
        case QtFatalMsg:
            return spdlog::level::critical;
    }
    return spdlog::level::info;
}

void QtLogHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    try {
        const QByteArray utf8_message = message.toUtf8();
        const char* category = context.category == nullptr ? "default" : context.category;
        const char* file = context.file == nullptr ? "" : context.file;
        const char* function = context.function == nullptr ? "" : context.function;
        if (auto logger = immersive_audio_muxer::logging::GetLogger()) {
            logger->log(spdlog::source_loc{file, context.line, function}, QtLogLevel(type),
                        "[Qt][{}] {}", category, utf8_message.constData());
            if (type == QtFatalMsg)
                logger->flush();
            return;
        }
    } catch (...) {
    }

    if (g_previous_qt_message_handler != nullptr) {
        g_previous_qt_message_handler(type, context, message);
        return;
    }
    const QByteArray fallback_message = message.toUtf8();
    const char* fallback_category = context.category == nullptr ? "default" : context.category;
    std::fprintf(stderr, "[Qt][%s] %s\n", fallback_category, fallback_message.constData());
}

}  // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("Immersive Audio Muxer"));
    QGuiApplication::setApplicationName(QStringLiteral("Immersive Audio Muxer"));
    QGuiApplication::setApplicationVersion(QStringLiteral(IMMERSIVE_AUDIO_MUXER_VERSION));

    QString fallback_base = qEnvironmentVariable("LOCALAPPDATA");
    if (fallback_base.isEmpty())
        fallback_base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!fallback_base.isEmpty())
        fallback_base = QDir(fallback_base).filePath(QStringLiteral("ImmersiveAudioMuxer"));
    std::string logging_error;
    const bool logging_ready = immersive_audio_muxer::logging::Initialize(
        PathFromQString(QCoreApplication::applicationDirPath()),
        PathFromQString(fallback_base), &logging_error);
    g_previous_qt_message_handler = qInstallMessageHandler(QtLogHandler);
    if (logging_ready) {
        immersive_audio_muxer::avformat_input::InitializeFfmpegLogging();
        IAMF_MUX_LOG_INFO(
            "[Startup] executable={} cwd={} version={} Qt={} FFmpeg={} spdlog={} iamf_tools_revision={} log_file={}",
            QCoreApplication::applicationFilePath().toUtf8().constData(),
            QDir::currentPath().toUtf8().constData(),
            QCoreApplication::applicationVersion().toUtf8().constData(), qVersion(),
            av_version_info(), IMMERSIVE_AUDIO_MUXER_SPDLOG_VERSION,
            IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_REVISION,
            immersive_audio_muxer::logging::CurrentLogPath().u8string());
    } else {
        std::fprintf(stderr, "Runtime logging initialization failed: %s\n", logging_error.c_str());
    }
#if defined(Q_OS_MACOS)
    // Qt uses the runtime window icon for Dock; keep its safe area aligned with the bundle .icns.
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/iamf_muxer_macos_dock_icon.svg")));
#else
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/iamf_muxer_app_icon.svg")));
#endif
    QScreen* primaryScreen = QGuiApplication::primaryScreen();
    const QRect availableGeometry = primaryScreen
        ? primaryScreen->availableGeometry()
        : QRect(0, 0, 1920, 1010);
    for (QScreen* screen : QGuiApplication::screens()) {
        const QByteArray screenNameUtf8 = screen->name().toUtf8();
        const QRect screenGeometry = screen->geometry();
        const QRect screenAvailableGeometry = screen->availableGeometry();
        IAMF_MUX_LOG_INFO(
            "[Startup][WindowGeometry] unit=qt_dip screen name={} primary={} geometry=({},{} {}x{}) "
            "available=({},{} {}x{}) dpr={} logical_dpi={}x{}",
            screenNameUtf8.constData(), screen == primaryScreen,
            screenGeometry.x(), screenGeometry.y(), screenGeometry.width(), screenGeometry.height(),
            screenAvailableGeometry.x(), screenAvailableGeometry.y(),
            screenAvailableGeometry.width(), screenAvailableGeometry.height(),
            screen->devicePixelRatio(), screen->logicalDotsPerInchX(), screen->logicalDotsPerInchY());
    }
    constexpr int initialWindowWidth = 1180;
    const int availableScreenHeight = availableGeometry.height();
    const int maximumWindowHeight = std::max(420, availableScreenHeight - 48);
    const int initialWindowHeight = std::min(922, maximumWindowHeight);
    const int minimumWindowHeight = std::min(620, maximumWindowHeight);
    IAMF_MUX_LOG_INFO(
        "[Startup][WindowGeometry] unit=qt_dip sizing primary_present={} available=({},{} {}x{}) "
        "requested_client={}x{} minimum_height={} maximum_height={} initial_height={}",
        primaryScreen != nullptr, availableGeometry.x(), availableGeometry.y(),
        availableGeometry.width(), availableGeometry.height(), initialWindowWidth,
        initialWindowHeight, minimumWindowHeight, maximumWindowHeight, initialWindowHeight);
    const int result = [&application, primaryScreen, availableGeometry, initialWindowWidth,
                        initialWindowHeight, minimumWindowHeight, maximumWindowHeight]() {
        immersive_audio_muxer::gui::GuiController controller;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("guiController"), &controller);
        engine.rootContext()->setContextProperty(QStringLiteral("windowInitialWidth"), initialWindowWidth);
        engine.rootContext()->setContextProperty(QStringLiteral("windowInitialHeight"), initialWindowHeight);
        engine.rootContext()->setContextProperty(QStringLiteral("windowMinimumHeight"), minimumWindowHeight);
        engine.rootContext()->setContextProperty(QStringLiteral("windowMaximumHeight"), maximumWindowHeight);
        engine.rootContext()->setContextProperty(
            QStringLiteral("iamfToolsRevision"),
            QString::fromUtf8(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_REVISION));
        IAMF_MUX_LOG_INFO("[Startup] loading QML module ImmersiveAudioMuxer.Gui/Main");
        engine.loadFromModule("ImmersiveAudioMuxer.Gui", "Main");
        if (engine.rootObjects().isEmpty()) {
            IAMF_MUX_LOG_ERROR("[Startup] QML load completed without root objects");
            return 1;
        }
        auto* mainWindow = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
        if (!mainWindow) {
            IAMF_MUX_LOG_ERROR("[Startup] QML root object is not a QQuickWindow");
            return 1;
        }
        if (primaryScreen)
            mainWindow->setScreen(primaryScreen);
        mainWindow->create();
        const QMargins frameMargins = mainWindow->frameMargins();
        const int frameWidth = mainWindow->width() + frameMargins.left() + frameMargins.right();
        const int frameHeight = mainWindow->height() + frameMargins.top() + frameMargins.bottom();
        const QPoint framePosition(
            availableGeometry.x() + (availableGeometry.width() - frameWidth) / 2,
            availableGeometry.y() + (availableGeometry.height() - frameHeight) / 2);
        IAMF_MUX_LOG_INFO(
            "[Startup][WindowGeometry] unit=qt_dip before_show client={}x{} min={}x{} max={}x{} "
            "frame_margins=(l{} t{} r{} b{}) estimated_frame=({},{} {}x{})",
            mainWindow->width(), mainWindow->height(),
            mainWindow->minimumWidth(), mainWindow->minimumHeight(),
            mainWindow->maximumWidth(), mainWindow->maximumHeight(),
            frameMargins.left(), frameMargins.top(), frameMargins.right(), frameMargins.bottom(),
            framePosition.x(), framePosition.y(), frameWidth, frameHeight);
        mainWindow->setFramePosition(framePosition);
        mainWindow->show();
        const QRect shownFrameGeometry = mainWindow->frameGeometry();
        const QMargins shownFrameMargins = mainWindow->frameMargins();
        const QPoint shownPosition = mainWindow->position();
        QScreen* shownScreen = mainWindow->screen();
        const QByteArray shownScreenNameUtf8 = shownScreen
            ? shownScreen->name().toUtf8()
            : QByteArrayLiteral("<none>");
        IAMF_MUX_LOG_INFO(
            "[Startup][WindowGeometry] unit=qt_dip after_show client_position=({}, {}) client={}x{} "
            "frame_position=({}, {}) frame={}x{} frame_margins=(l{} t{} r{} b{}) "
            "screen={} dpr={}",
            shownPosition.x(), shownPosition.y(), mainWindow->width(), mainWindow->height(),
            shownFrameGeometry.x(), shownFrameGeometry.y(),
            shownFrameGeometry.width(), shownFrameGeometry.height(),
            shownFrameMargins.left(), shownFrameMargins.top(),
            shownFrameMargins.right(), shownFrameMargins.bottom(),
            shownScreenNameUtf8.constData(), mainWindow->devicePixelRatio());
        return application.exec();
    }();
    IAMF_MUX_LOG_INFO("[Shutdown] application event loop exited with code={}", result);
    qInstallMessageHandler(g_previous_qt_message_handler);
    immersive_audio_muxer::logging::Shutdown();
    return result;
}
