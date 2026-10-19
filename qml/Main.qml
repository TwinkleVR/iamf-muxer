import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Dialogs
import QtCore
import QtMultimedia

ApplicationWindow {
    id: window
    width: windowInitialWidth
    height: windowInitialHeight
    minimumWidth: 960
    minimumHeight: windowMinimumHeight
    maximumHeight: windowMaximumHeight
    title: "Immersive Audio Muxer"
    color: "#131313"
    font.family: "Inter"

    property string monoFont: "JetBrains Mono"

    property url lastOutputFolder: ""
    property var lastInputFolders: ({
        spatial: String(inputFolderSettings.value("spatial", "")),
        headlocked: String(inputFolderSettings.value("headlocked", "")),
        video: String(inputFolderSettings.value("video", ""))
    })
    property url defaultInputFolder: ""

    Settings {
        id: inputFolderSettings
        category: "InputFolders"
    }

    Component.onCompleted: defaultInputFolder = fileDialog.currentFolder

    palette.window: "#131313"
    palette.base: "#1b1b1c"
    palette.alternateBase: "#202020"
    palette.text: "#e5e2e1"
    palette.windowText: "#e5e2e1"
    palette.button: "#2a2a2a"
    palette.buttonText: "#e5e2e1"
    palette.highlight: "#c8c8c8"
    palette.highlightedText: "#1c1c1c"
    palette.placeholderText: "#8a8a8a"

    function localPathFromUrl(url) {
        if (url && typeof url.toLocalFile === "function")
            return url.toLocalFile()
        let path = String(url)
        if (path.indexOf("file:///") === 0) {
            const localPart = path.substring(8)
            path = /^[A-Za-z]:([\\/]|$)/.test(localPart) ? localPart : "/" + localPart
        } else if (path.indexOf("file://") === 0) {
            path = "//" + path.substring(7)
        }
        try {
            return decodeURIComponent(path)
        } catch (error) {
            return path
        }
    }

    function fileUrlFromLocalPath(path) {
        let normalized = String(path).replace(/\\/g, "/")
        if (normalized.length === 0)
            return ""

        const segments = normalized.split("/")
        const encodedSegments = []
        for (const segment of segments)
            encodedSegments.push(encodeURIComponent(segment))
        let encoded = encodedSegments.join("/")
        encoded = encoded.replace(/^([A-Za-z])%3A/, "$1:")
        if (encoded.charAt(0) !== "/")
            encoded = "/" + encoded
        return "file://" + encoded
    }

    function parentFolderUrlFromLocalPath(path) {
        const normalized = String(path).replace(/\\/g, "/")
        const separator = normalized.lastIndexOf("/")
        if (separator < 0)
            return ""
        return fileUrlFromLocalPath(normalized.substring(0, separator + 1))
    }

    function inputPathForSlot(slot) {
        if (slot === "spatial")
            return String(guiController.spatialAudioPath)
        if (slot === "headlocked")
            return String(guiController.headLockedStereoPath)
        if (slot === "video")
            return String(guiController.videoPath)
        return ""
    }

    function rememberInputFolder(slot, folderUrl) {
        const normalizedFolderUrl = String(folderUrl || "")
        if (slot.length === 0 || normalizedFolderUrl.length === 0)
            return

        const updatedFolders = {}
        for (const existingSlot in lastInputFolders)
            updatedFolders[existingSlot] = lastInputFolders[existingSlot]
        updatedFolders[slot] = normalizedFolderUrl
        lastInputFolders = updatedFolders
        inputFolderSettings.setValue(slot, normalizedFolderUrl)
        inputFolderSettings.sync()
    }

    function rememberInputFileFolder(slot, path) {
        const folderUrl = parentFolderUrlFromLocalPath(path)
        if (folderUrl.length > 0)
            rememberInputFolder(slot, folderUrl)
    }

    function fileNameFromPath(path) {
        const normalized = String(path).replace(/\\/g, "/")
        const separator = normalized.lastIndexOf("/")
        return separator >= 0 ? normalized.substring(separator + 1) : normalized
    }

    function pathsFromUrls(urls) {
        const paths = []
        if (!urls)
            return paths
        for (const url of urls) {
            const path = localPathFromUrl(url)
            if (path.length > 0)
                paths.push(path)
        }
        return paths
    }

    function droppedPaths(drop) {
        return pathsFromUrls(drop ? drop.urls : [])
    }

    function dragPaths(drag) {
        return pathsFromUrls(drag ? drag.urls : [])
    }

    function canAcceptDrag(drag, slot) {
        if (guiController.jobRunning)
            return false
        const paths = dragPaths(drag)
        if (paths.length === 0)
            return false
        for (const path of paths) {
            if (!guiController.canAcceptInputPath(path, slot))
                return false
        }
        return true
    }

    function canReceiveDrop(drag) {
        if (guiController.jobRunning || !guiController.modeSelected)
            return false
        return dragPaths(drag).length > 0
    }

    function sendDrop(drop, slot) {
        if (guiController.jobRunning)
            return
        if (!guiController.addInputFilesToSlot(droppedPaths(drop), slot)) {
            inputErrorSound.play()
            return
        }
        rememberInputFileFolder(slot, inputPathForSlot(slot))
    }

    function browseInput(slot) {
        if (guiController.jobRunning || !guiController.modeSelected)
            return
        fileDialog.targetSlot = slot
        const currentPath = inputPathForSlot(slot)
        let initialFolder = String(lastInputFolders[slot] || defaultInputFolder || "")
        if (currentPath.length > 0) {
            const currentFolder = parentFolderUrlFromLocalPath(currentPath)
            if (currentFolder.length > 0)
                initialFolder = currentFolder
            rememberInputFileFolder(slot, currentPath)
        }
        if (initialFolder.length > 0)
            fileDialog.currentFolder = initialFolder
        fileDialog.currentFile = ""
        if (slot === "spatial") {
            fileDialog.title = guiController.iamfMode
                    ? "Select IAMF Audio"
                    : (guiController.channelBasedMode ? "Select Channel-based Audio" : "Select Ambisonics Audio")
            fileDialog.nameFilters = guiController.iamfMode
                    ? ["IAMF files (*.iamf)", "All files (*)"]
                    : ["Audio files (*.wav *.w64 *.ogg *.m4a *.aac *.flac *.opus *.mp3 *.ac3)", "All files (*)"]
        } else if (slot === "headlocked") {
            fileDialog.title = "Select Head-Locked Stereo Audio"
            fileDialog.nameFilters = ["Audio files (*.wav *.w64 *.ogg *.m4a *.aac *.flac *.opus *.mp3 *.ac3)", "All files (*)"]
        } else {
            fileDialog.title = "Select video"
            fileDialog.nameFilters = ["Video files (*.mp4 *.mov *.mkv *.webm)", "All files (*)"]
        }
        fileDialog.open()
    }

    function browseOutput() {
        if (!guiController.modeSelected)
            return
        const initialPath = String(guiController.outputPath).trim()

        if (initialPath.length > 0) {
            const folderUrl = parentFolderUrlFromLocalPath(initialPath)
            if (folderUrl.length > 0)
                outputFileDialog.currentFolder = folderUrl
            outputFileDialog.currentFile = fileUrlFromLocalPath(initialPath)
        } else if (String(lastOutputFolder).length > 0) {
            outputFileDialog.currentFolder = lastOutputFolder
            outputFileDialog.currentFile = ""
        }
        outputFileDialog.open()
    }

    function clearInputFocus() {
        if (mixAnnotationField.activeFocus)
            mixAnnotationField.focus = false
        if (outputPathField.activeFocus)
            outputPathField.focus = false
    }

    function isPointInsideItem(item, point) {
        const localPoint = item.mapFromItem(pageLayout, point.x, point.y)
        return localPoint.x >= 0 && localPoint.y >= 0 &&
               localPoint.x <= item.width && localPoint.y <= item.height
    }

    function buttonColor(control, primary) {
        if (primary)
            return control.enabled ? "#c8c8c8" : "#353535"
        return control.hovered ? "#353535" : "#252525"
    }

    SoundEffect {
        id: inputErrorSound
        source: "qrc:/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/input_error.wav"
        volume: 0.55
    }

    Component {
        id: metadataTagDelegate

        Rectangle {
            property string tagValue: String(modelData)
            property bool primaryTag: index === 0
            width: tagText.implicitWidth + 16
            height: 24
            radius: 4
            color: primaryTag ? "#293b2e" : "#2a2a2a"

            Text {
                id: tagText
                anchors.centerIn: parent
                text: tagValue
                color: primaryTag ? "#82c98a" : "#c0c7d4"
                font.family: window.monoFont
                font.pixelSize: 11
                font.weight: primaryTag ? Font.DemiBold : Font.Normal
            }
        }
    }
    FileDialog {
        id: fileDialog
        property string targetSlot: ""
        fileMode: FileDialog.OpenFile
        onAccepted: {
            if (guiController.jobRunning || targetSlot.length === 0)
                return
            const path = window.localPathFromUrl(selectedFile)
            window.rememberInputFileFolder(targetSlot, path)
            guiController.clearInput(targetSlot)
            if (!guiController.addInputFile(path, targetSlot))
                inputErrorSound.play()
        }
        onRejected: {
            if (targetSlot.length > 0)
                window.rememberInputFolder(targetSlot, fileDialog.currentFolder)
        }
    }

    FileDialog {
        id: outputFileDialog
        title: "Select output MP4 path"
        fileMode: FileDialog.SaveFile
        nameFilters: ["MP4 files (*.mp4)", "All files (*)"]
        onAccepted: {
            const path = window.localPathFromUrl(selectedFile)
            if (path.length > 0) {
                guiController.outputPath = path
                const folderUrl = window.parentFolderUrlFromLocalPath(path)
                if (folderUrl.length > 0)
                    window.lastOutputFolder = folderUrl
            }
        }
    }

    Dialog {
        id: jobResultDialog
        property bool resultSuccess: false
        property bool resultCancelled: false
        property string resultMessage: ""
        anchors.centerIn: Overlay.overlay
        width: Math.min(window.width - 64, 500)
        modal: true
        closePolicy: Popup.NoAutoClose
        standardButtons: Dialog.NoButton
        padding: 24

        contentItem: ColumnLayout {
            spacing: 18

            Text {
                Layout.fillWidth: true
                text: jobResultDialog.resultSuccess
                      ? "Success"
                      : (jobResultDialog.resultCancelled ? "Cancelled" : "Error")
                color: "#e8e5e2"
                font.family: window.font.family
                font.pixelSize: 16
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                Layout.fillWidth: true
                text: jobResultDialog.resultMessage
                color: "#c8c5c2"
                font.family: window.font.family
                font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                lineHeight: 1.15
            }

            Button {
                id: jobResultOkButton
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 88
                Layout.preferredHeight: 34
                text: "OK"
                onClicked: jobResultDialog.close()
                scale: jobResultOkButton.pressed ? 0.95 : (jobResultOkButton.hovered ? 1.015 : 1.0)
                Behavior on scale {
                    NumberAnimation {
                        duration: 120
                        easing.type: Easing.OutCubic
                    }
                }

                background: Rectangle {
                    radius: 6
                    color: jobResultOkButton.pressed
                           ? "#2f6bd8"
                           : (jobResultOkButton.hovered ? "#4b8cff" : "#3478f6")
                    Behavior on color {
                        ColorAnimation {
                            duration: 140
                            easing.type: Easing.OutCubic
                        }
                    }
                }

                contentItem: Text {
                    text: jobResultOkButton.text
                    color: "#ffffff"
                    font.family: window.font.family
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        background: Rectangle {
            radius: 10
            color: "#242426"
            border.width: 1
            border.color: jobResultDialog.resultSuccess ? "#4d6f54" : "#5d4b4b"
        }

        Overlay.modal: Rectangle {
            color: "#88000000"
        }
    }

    Connections {
        target: guiController

        function onJobFinished(success, message) {
            const resultText = String(message)
            jobResultDialog.resultSuccess = success
            jobResultDialog.resultCancelled = !success &&
                    resultText.toLowerCase().indexOf("cancelled") >= 0
            jobResultDialog.resultMessage = resultText
            jobResultDialog.open()
        }

        function onJobStateChanged() {
            if (!guiController.jobRunning)
                return
            codecSelector.popup.close()
            videoMappingSelector.popup.close()
        }
    }

    ColumnLayout {
        id: pageLayout
        anchors.fill: parent
        spacing: 0

        TapHandler {
            acceptedButtons: Qt.LeftButton
            onTapped: function(eventPoint) {
                const point = eventPoint.position
                if (!window.isPointInsideItem(mixAnnotationField, point) &&
                    !window.isPointInsideItem(outputPathField, point))
                    window.clearInputFocus()
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 64

            RowLayout {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: -16
                spacing: 16

                Image {
                    Layout.preferredWidth: 52
                    Layout.preferredHeight: 52
                    source: "qrc:/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/iamf_logo_symbol.svg"
                    sourceSize.width: 52
                    sourceSize.height: 52
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    mipmap: true
                }

                Label {
                    id: titleLabel
                    text: "Immersive Audio Muxer"
                    font.pixelSize: 32
                    font.weight: Font.DemiBold
                    color: "#f1f1f1"
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: "#404040"
            }
        }

        ColumnLayout {
            id: workflowSelector
            enabled: !guiController.jobRunning
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            Layout.preferredHeight: 112
            Layout.minimumHeight: 112
            Layout.maximumHeight: 112
            spacing: 7


            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 88
                Layout.alignment: Qt.AlignVCenter
                spacing: 12

                Button {
                    id: iamfWorkflowCard
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 88
                    Layout.alignment: Qt.AlignVCenter
                    enabled: !guiController.jobRunning
                    property bool selected: guiController.iamfMode
                    opacity: (!guiController.jobRunning || selected) ? 1.0 : 0.48
                    onClicked: guiController.setInputMode(0)

                    background: Rectangle {
                        radius: 8
                        color: iamfWorkflowCard.selected
                               ? "#343635"
                               : (iamfWorkflowCard.hovered ? "#292929" : "#202020")
                        border.width: iamfWorkflowCard.selected ? 2 : 1
                        border.color: iamfWorkflowCard.selected
                                      ? "#aab8ad"
                                      : (iamfWorkflowCard.hovered ? "#676767" : "#3b3b3b")
                        layer.enabled: iamfWorkflowCard.selected
                        layer.effect: MultiEffect {
                            shadowEnabled: true
                            shadowColor: "#93ad99"
                            shadowOpacity: 0.22
                            shadowBlur: 0.55
                            shadowVerticalOffset: 2
                        }
                    }

                    contentItem: Item {
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 14
                            anchors.topMargin: 10
                            anchors.bottomMargin: 10
                            anchors.rightMargin: iamfWorkflowCard.selected ? 88 : 14
                            spacing: 5

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                Image {
                                    Layout.preferredWidth: 38
                                    Layout.preferredHeight: 38
                                    source: "qrc:/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/iamf_logo_white_symbol.svg"
                                    sourceSize.width: 38
                                    sourceSize.height: 38
                                    fillMode: Image.PreserveAspectFit
                                    smooth: true
                                    mipmap: true
                                }

                                Label {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    text: "IAMF"
                                    font.pixelSize: 19
                                    font.weight: Font.DemiBold
                                    color: "#f1f1f1"
                                }

                            }

                            Label {
                                Layout.fillWidth: true
                                text: "For standalone .iamf audio files"
                                font.pixelSize: 11
                                color: "#9f9f9f"
                                elide: Text.ElideRight
                            }
                        }

                        Label {
                            anchors.right: parent.right
                            anchors.rightMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            z: 1
                            visible: iamfWorkflowCard.selected
                            text: "SELECTED"
                            font.family: window.monoFont
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            color: "#b9c8bc"
                        }
                    }
                }

                Button {
                    id: ambisonicsWorkflowCard
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 88
                    Layout.alignment: Qt.AlignVCenter
                    enabled: !guiController.jobRunning
                    property bool selected: guiController.ambisonicsMode
                    opacity: (!guiController.jobRunning || selected) ? 1.0 : 0.48
                    onClicked: guiController.setInputMode(1)

                    background: Rectangle {
                        radius: 8
                        color: ambisonicsWorkflowCard.selected
                               ? "#343635"
                               : (ambisonicsWorkflowCard.hovered ? "#292929" : "#202020")
                        border.width: ambisonicsWorkflowCard.selected ? 2 : 1
                        border.color: ambisonicsWorkflowCard.selected
                                      ? "#aab8ad"
                                      : (ambisonicsWorkflowCard.hovered ? "#676767" : "#3b3b3b")
                        layer.enabled: ambisonicsWorkflowCard.selected
                        layer.effect: MultiEffect {
                            shadowEnabled: true
                            shadowColor: "#93ad99"
                            shadowOpacity: 0.22
                            shadowBlur: 0.55
                            shadowVerticalOffset: 2
                        }
                    }

                    contentItem: Item {
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 14
                            anchors.topMargin: 10
                            anchors.bottomMargin: 10
                            anchors.rightMargin: ambisonicsWorkflowCard.selected ? 88 : 14
                            spacing: 5

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                Image {
                                    Layout.preferredWidth: 38
                                    Layout.preferredHeight: 38
                                    source: "qrc:/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/ambisonics_symbol.svg"
                                    sourceSize.width: 38
                                    sourceSize.height: 38
                                    fillMode: Image.PreserveAspectFit
                                    smooth: true
                                    mipmap: true
                                }

                                Label {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    text: "AMBISONICS"
                                    font.pixelSize: 19
                                    font.weight: Font.DemiBold
                                    color: "#f1f1f1"
                                }

                            }

                            Label {
                                Layout.fillWidth: true
                                text: "For Ambisonic audio in AmbiX format"
                                font.pixelSize: 11
                                color: "#9f9f9f"
                                elide: Text.ElideRight
                            }
                        }

                        Label {
                            anchors.right: parent.right
                            anchors.rightMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            z: 1
                            visible: ambisonicsWorkflowCard.selected
                            text: "SELECTED"
                            font.family: window.monoFont
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            color: "#b9c8bc"
                        }
                    }
                }

                Button {
                    id: channelBasedWorkflowCard
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 88
                    Layout.alignment: Qt.AlignVCenter
                    enabled: !guiController.jobRunning
                    property bool selected: guiController.channelBasedMode
                    opacity: (!guiController.jobRunning || selected) ? 1.0 : 0.48
                    onClicked: guiController.setInputMode(2)

                    background: Rectangle {
                        radius: 8
                        color: channelBasedWorkflowCard.selected
                               ? "#343635"
                               : (channelBasedWorkflowCard.hovered ? "#292929" : "#202020")
                        border.width: channelBasedWorkflowCard.selected ? 2 : 1
                        border.color: channelBasedWorkflowCard.selected
                                      ? "#aab8ad"
                                      : (channelBasedWorkflowCard.hovered ? "#676767" : "#3b3b3b")
                        layer.enabled: channelBasedWorkflowCard.selected
                        layer.effect: MultiEffect {
                            shadowEnabled: true
                            shadowColor: "#93ad99"
                            shadowOpacity: 0.22
                            shadowBlur: 0.55
                            shadowVerticalOffset: 2
                        }
                    }

                    contentItem: Item {
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 14
                            anchors.topMargin: 10
                            anchors.bottomMargin: 10
                            anchors.rightMargin: channelBasedWorkflowCard.selected ? 88 : 14
                            spacing: 5

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                Image {
                                    Layout.preferredWidth: 38
                                    Layout.preferredHeight: 38
                                    source: "qrc:/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/speaker_layout_symbol.svg"
                                    sourceSize.width: 38
                                    sourceSize.height: 38
                                    fillMode: Image.PreserveAspectFit
                                    smooth: true
                                    mipmap: true
                                }

                                Label {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    text: "CHANNEL-BASED"
                                    font.pixelSize: 17
                                    font.weight: Font.DemiBold
                                    color: "#f1f1f1"
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: "For speaker layouts like 5.1 and 7.1.4"
                                font.pixelSize: 11
                                color: "#9f9f9f"
                                elide: Text.ElideRight
                            }
                        }

                        Label {
                            anchors.right: parent.right
                            anchors.rightMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            z: 1
                            visible: channelBasedWorkflowCard.selected
                            text: "SELECTED"
                            font.family: window.monoFont
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            color: "#b9c8bc"
                        }
                    }
                }
            }


        }
        Item {
            id: editorContainer
            enabled: guiController.modeSelected
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            Rectangle {
                z: 100
                anchors.fill: parent
                visible: !guiController.modeSelected
                color: "#131313"
                opacity: 0.92
                border.width: 1
                border.color: "#343434"
                radius: 4

                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 8

                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: "CHOOSE AN AUDIO WORKFLOW"
                        font.family: window.monoFont
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: "#eeeeee"
                    }

                    Label {
                        Layout.alignment: Qt.AlignHCenter
        text: "Choose how your audio will be prepared for IAMF."
                        font.pixelSize: 14
                        color: "#b8b8b8"
                    }
                }
            }
                ScrollView {
                id: editorScrollView
                anchors.fill: parent
                anchors.bottomMargin: 70
                clip: true
                padding: 0
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ScrollBar.vertical.policy: ScrollBar.AlwaysOn
                ScrollBar.vertical.opacity: editorScrollView.contentHeight > editorScrollView.height ? 1.0 : 0.0
                ScrollBar.vertical.interactive: editorScrollView.contentHeight > editorScrollView.height

                ColumnLayout {
                    x: 20
                    width: editorScrollView.availableWidth - 40
                    height: implicitHeight
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16

                        Rectangle {
                            id: spatialCard
                            Layout.fillWidth: true
                            Layout.preferredWidth: guiController.ambisonicsMode ? 1 : 2
                            Layout.preferredHeight: 190
                            enabled: !guiController.jobRunning
                            color: "#1e1e1e"
                            border.width: 0
                            border.color: "#343434"
                            radius: 4
                            layer.enabled: true
                            layer.effect: MultiEffect {
                                shadowEnabled: true
                                shadowColor: "#000000"
                                shadowOpacity: 0.30
                                shadowBlur: 0.55
                                shadowVerticalOffset: 2
                            }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 12

                                RowLayout {
                                    id: outputPathRow
                                    Layout.fillWidth: true
                                    spacing: 8
                                    property real actionButtonWidth: Math.max(1, (videoMappingSelector.width - spacing) / 2)

                                    Rectangle {
                                        Layout.preferredWidth: 3
                                        Layout.preferredHeight: 18
                                        radius: 1
                                        color: "#f0f0f0"
                                    }

                                    Label {
                                        text: guiController.iamfMode
                                              ? "AUDIO INPUT"
                                              : (guiController.channelBasedMode
                                                 ? "AUDIO INPUT"
                                                 : "AMBISONICS AUDIO INPUT")
                                        font.pixelSize: 14
                                        font.weight: Font.DemiBold
                                        color: "#f0f0f0"
                                        Layout.fillWidth: true
                                    }

                                    Label {
                                        text: "* REQUIRED"
                                        font.family: window.monoFont
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                        color: "#ffb689"
                                        Layout.fillWidth: true
                                        horizontalAlignment: Text.AlignRight
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    visible: guiController.spatialAudioPath.length === 0
                                    enabled: !guiController.jobRunning
                                    opacity: enabled ? 1.0 : 0.48
                                    color: "#0e0e0e"
                                    border.width: spatialDropArea.compatibleDrag || spatialDropHover.hovered ? 2 : 1
                                    border.color: spatialDropArea.compatibleDrag
                                                 ? "#d0d0d0"
                                                 : (spatialDropHover.hovered ? "#6f8fb8" : "#404040")
                                    radius: 4

                                    HoverHandler {
                                        id: spatialDropHover
                                    }

                                    TapHandler {
                                        acceptedButtons: Qt.LeftButton
                                        onTapped: window.browseInput("spatial")
                                    }

                                    DropArea {
                                        id: spatialDropArea
                                        property bool compatibleDrag: false
                                        anchors.fill: parent
                                        onEntered: function(drag) {
                                            compatibleDrag = window.canAcceptDrag(drag, "spatial")
                                            drag.accepted = window.canReceiveDrop(drag)
                                        }
                                        onExited: compatibleDrag = false
                                        onDropped: function(drop) {
                                            drop.accepted = window.canReceiveDrop(drop)
                                            if (drop.accepted)
                                                window.sendDrop(drop, "spatial")
                                            compatibleDrag = false
                                        }
                                    }

                                    ColumnLayout {
                                        anchors.centerIn: parent
                                        spacing: 10

                                        Item {
                                            Layout.alignment: Qt.AlignHCenter
                                            Layout.minimumWidth: 52
                                            Layout.preferredWidth: 52
                                            Layout.maximumWidth: 52
                                            Layout.minimumHeight: 52
                                            Layout.preferredHeight: 52
                                            Layout.maximumHeight: 52

                                            Image {
                                                anchors.centerIn: parent
                                                width: guiController.iamfMode ? 52 : 44
                                                height: guiController.iamfMode ? 52 : 44
                                                source: guiController.iamfMode
                                                        ? "qrc:/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/iamf_logo_white_symbol.svg"
                                                        : (guiController.channelBasedMode
                                                           ? "qrc:/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/speaker_layout_symbol.svg"
                                                           : "qrc:/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/ambisonics_symbol.svg")
                                                sourceSize.width: width
                                                sourceSize.height: height
                                                fillMode: Image.PreserveAspectFit
                                                opacity: 1.0
                                                smooth: true
                                                mipmap: true
                                            }
                                        }

                                        Text {
                                            Layout.alignment: Qt.AlignHCenter
                                            Layout.preferredWidth: 390
                                            Layout.minimumHeight: 18
                                            Layout.preferredHeight: 18
                                            Layout.maximumHeight: 18
                                            text: guiController.iamfMode
                                                  ? 'Drag and drop standalone iamf file here or <a href="browse">Browse</a>'
                                                  : (guiController.channelBasedMode
                                                     ? 'Drag and drop channel-based audio here or <a href="browse">Browse</a>'
                                                     : 'Drag and drop an Ambisonic file (AmbiX only) here or <a href="browse">Browse</a>')
                                            textFormat: Text.RichText
                                            linkColor: "#a3c9ff"
                                            color: "#c0c0c0"
                                            font.pixelSize: 13
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                            wrapMode: Text.NoWrap
                                        }


                                    }
                                }

                                Rectangle {
                                    id: spatialAudioInfoCard
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    visible: guiController.spatialAudioPath.length > 0
                                    enabled: !guiController.jobRunning
                                    opacity: enabled ? 1.0 : 0.48
                                    color: "#0e0e0e"
                                    border.width: spatialInfoDropArea.compatibleDrag || spatialInfoHover.hovered ? 2 : 1
                                    border.color: spatialInfoDropArea.compatibleDrag
                                                 ? "#d0d0d0"
                                                 : (spatialInfoHover.hovered ? "#8fcf9a" : "#5f9668")
                                    radius: 4

                                    HoverHandler {
                                        id: spatialInfoHover
                                    }

                                    DropArea {
                                        id: spatialInfoDropArea
                                        property bool compatibleDrag: false
                                        anchors.fill: parent
                                        onEntered: function(drag) {
                                            compatibleDrag = window.canAcceptDrag(drag, "spatial")
                                            drag.accepted = window.canReceiveDrop(drag)
                                        }
                                        onExited: compatibleDrag = false
                                        onDropped: function(drop) {
                                            drop.accepted = window.canReceiveDrop(drop)
                                            if (drop.accepted)
                                                window.sendDrop(drop, "spatial")
                                            compatibleDrag = false
                                        }
                                    }

                                    Button {
                                        id: clearSpatialInfoButton
                                        anchors.right: parent.right
                                        anchors.rightMargin: 16
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 30
                                        height: 30
                                        text: "\u2715"
                                        onClicked: guiController.clearInput("spatial")
                                        background: Rectangle {
                                            radius: 15
                                            color: clearSpatialInfoButton.hovered ? "#343434" : "transparent"
                                            border.width: 0
                                        }
                                        contentItem: Text {
                                            anchors.centerIn: parent
                                            text: clearSpatialInfoButton.text
                                            color: clearSpatialInfoButton.hovered ? "#f0f0f0" : "#a7a7a7"
                                            font.pixelSize: 19
                                            font.weight: Font.DemiBold
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                    }
                                    Item {
                                        anchors.left: parent.left
                                        anchors.leftMargin: 18
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 52
                                        height: 52

                                        Image {
                                            anchors.centerIn: parent
                                            width: guiController.iamfMode ? 52 : 44
                                            height: guiController.iamfMode ? 52 : 44
                                            source: guiController.iamfMode
                                                    ? "qrc:/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/iamf_logo_white_symbol.svg"
                                                    : (guiController.channelBasedMode
                                                       ? "qrc:/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/speaker_layout_symbol.svg"
                                                       : "qrc:/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/ambisonics_symbol.svg")
                                            sourceSize.width: width
                                            sourceSize.height: height
                                            fillMode: Image.PreserveAspectFit
                                            smooth: true
                                            mipmap: true
                                        }
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: 18
                                        anchors.leftMargin: 84
                                        anchors.rightMargin: 52
                                        spacing: 16

                                        Item {
                                            id: spatialAudioMetadataColumn
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true
                                            Layout.minimumWidth: 0
                                            Layout.alignment: Qt.AlignVCenter

                                            TapHandler {
                                                acceptedButtons: Qt.LeftButton
                                                onTapped: window.browseInput("spatial")
                                            }

                                            Label {
                                                id: spatialAudioFileNameLabel
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                anchors.bottom: spatialAudioPathLabel.top
                        anchors.bottomMargin: 10
                                                text: window.fileNameFromPath(guiController.spatialAudioPath)
                                                font.family: window.font.family
                                                font.pixelSize: 17
                                                font.weight: Font.DemiBold
                                                color: "#e5e2e1"
                                                elide: Text.ElideRight

                                                HoverHandler { id: spatialAudioFileNameHover }

                                                ToolTip {
                                                    parent: spatialAudioFileNameLabel
                                                    visible: spatialAudioFileNameHover.hovered &&
                                                             spatialAudioFileNameLabel.truncated
                                                    delay: 500
                                                    contentItem: Text {
                                                        text: spatialAudioFileNameLabel.text
                                                        font.family: window.font.family
                                                        font.pixelSize: 13
                                                        font.weight: Font.Normal
                                                        color: "#f1f1f1"
                                                        textFormat: Text.PlainText
                                                        wrapMode: Text.NoWrap
                                                    }
                                                    background: Rectangle {
                                                        radius: 5
                                                        color: "#242426"
                                                        border.width: 1
                                                        border.color: "#4f4d51"
                                                    }
                                                }
                                            }

                                            Label {
                                                id: spatialAudioPathLabel
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                anchors.verticalCenter: parent.verticalCenter
                                                text: guiController.spatialAudioPath
                                                font.family: window.monoFont
                                                font.pixelSize: 12
                                                color: "#9d9d9d"
                                                elide: Text.ElideMiddle
                                            }

                                            Flow {
                                                id: spatialAudioTags
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                anchors.top: spatialAudioPathLabel.bottom
                        anchors.topMargin: 10
                                                spacing: 6

                                                Repeater {
                                                    model: guiController.spatialAudioTags
                                                    delegate: metadataTagDelegate
                                                }
                                            }

                                        }

                                        Item {
                                            id: channelLayoutControls
                                            visible: guiController.channelBasedMode
                                            Layout.preferredWidth: 280
                                            Layout.minimumWidth: 280
                                            Layout.maximumWidth: 280
                                            Layout.fillHeight: true

                                            RowLayout {
                                                id: channelLayoutHeader
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                y: {
                                                    const selectorCenterY = channelLayoutSelector.y +
                                                                            channelLayoutSelector.height / 2
                                                    const detailsCenterY = channelLayoutDetails.y +
                                                                           channelLayoutDetails.implicitHeight / 2
                                                    const mirroredCenterY = 2 * selectorCenterY - detailsCenterY
                                                    const centeredY = mirroredCenterY - implicitHeight / 2
                                                    const maximumY = Math.max(0,
                                                                              channelLayoutControls.height - implicitHeight)
                                                    return Math.max(0, Math.min(maximumY, centeredY))
                                                }
                                                spacing: 8

                                                Label {
                                                    text: "INPUT LAYOUT"
                                                    font.family: window.monoFont
                                                    font.pixelSize: 11
                                                    font.weight: Font.DemiBold
                                                    color: "#bcbcbc"
                                                }

                                                Item {
                                                    Layout.fillWidth: true
                                                }

                                                Label {
                                                    visible: guiController.channelLayoutConfirmed
                                                    Layout.preferredHeight: 24
                                                    verticalAlignment: Text.AlignVCenter
                                                    text: guiController.channelLayoutDetected
                                                          ? "DETECTED" : "SELECTED"
                                                    font.family: window.monoFont
                                                    font.pixelSize: 9
                                                    font.weight: Font.DemiBold
                                                    color: "#a7c7ab"
                                                }

                                            }

                                            Basic.ComboBox {
                                                id: channelLayoutSelector
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                anchors.verticalCenter: parent.verticalCenter
                                                height: 34
                                                model: guiController.channelLayoutOptions
                                                currentIndex: guiController.channelLayoutOptions.indexOf(guiController.channelLayoutName)
                                                enabled: !guiController.jobRunning &&
                                                         guiController.channelLayoutOptions.length > 0
                                                font.family: window.font.family
                                                font.pixelSize: 15
                                                font.weight: Font.DemiBold

                                                contentItem: Text {
                                                    leftPadding: 12
                                                    rightPadding: 30
                                                    text: channelLayoutSelector.displayText.length > 0
                                                          ? channelLayoutSelector.displayText
                                                          : (guiController.channelLayoutOptions.length > 0
                                                             ? "! SELECT LAYOUT" : "UNAVAILABLE")
                                                    color: channelLayoutSelector.displayText.length > 0
                                                           ? "#d8d6d4"
                                                           : (guiController.channelLayoutOptions.length > 0
                                                              ? "#ffc48a" : "#9d9d9d")
                                                    font.family: channelLayoutSelector.font.family
                                                    font.pixelSize: channelLayoutSelector.displayText.length > 0
                                                                    ? channelLayoutSelector.font.pixelSize
                                                                    : (guiController.channelLayoutOptions.length > 0
                                                                       ? 12 : 11)
                                                    font.weight: channelLayoutSelector.displayText.length > 0
                                                                 ? channelLayoutSelector.font.weight
                                                                 : (guiController.channelLayoutOptions.length > 0
                                                                    ? Font.DemiBold : Font.Normal)
                                                    horizontalAlignment: Text.AlignHCenter
                                                    verticalAlignment: Text.AlignVCenter
                                                    elide: Text.ElideRight
                                                }

                                                indicator: Item {
                                                    visible: guiController.channelLayoutOptions.length > 0
                                                    x: channelLayoutSelector.width - width - 10
                                                    y: (channelLayoutSelector.height - height) / 2
                                                    width: 12
                                                    height: 12

                                                    Canvas {
                                                        anchors.fill: parent
                                                        onPaint: {
                                                            const context = getContext("2d")
                                                            context.clearRect(0, 0, width, height)
                                                            context.strokeStyle = channelLayoutSelector.pressed ? "#c7c4c1" : "#999795"
                                                            context.lineWidth = 1.25
                                                            context.lineCap = "round"
                                                            context.beginPath()
                                                            context.moveTo(2.5, 4.5)
                                                            context.lineTo(6, 8)
                                                            context.lineTo(9.5, 4.5)
                                                            context.stroke()
                                                        }
                                                    }
                                                }

                                                background: Rectangle {
                                                    radius: 4
                                                    color: channelLayoutSelector.pressed
                                                           ? "#2f2f31"
                                                           : (channelLayoutSelector.hovered ? "#2b2b2d" : "#252527")
                                                    border.width: 1
                                                    border.color: channelLayoutSelector.activeFocus
                                                                  ? "#777477"
                                                                  : (channelLayoutSelector.hovered ? "#5d5b5f" : "#454347")
                                                }

                                                delegate: Basic.ItemDelegate {
                                                    id: channelLayoutDelegate
                                                    width: channelLayoutSelector.popup.width - 8
                                                    height: 36
                                                    highlighted: channelLayoutSelector.highlightedIndex === index

                                                    contentItem: Text {
                                                        leftPadding: 10
                                                        text: modelData
                                                        color: channelLayoutDelegate.highlighted ? "#e5e2df" : "#d2d0ce"
                                                        font.family: window.font.family
                                                        font.pixelSize: 14
                                                        verticalAlignment: Text.AlignVCenter
                                                    }

                                                    background: Rectangle {
                                                        radius: 3
                                                        color: channelLayoutDelegate.hovered
                                                               ? "#373638"
                                                               : (channelLayoutDelegate.highlighted ? "#303033" : "transparent")
                                                    }
                                                }

                                                popup: Popup {
                                                    y: channelLayoutSelector.height + 4
                                                    width: channelLayoutSelector.width
                                                    padding: 4

                                                    contentItem: ListView {
                                                        clip: true
                                                        implicitWidth: channelLayoutSelector.width - 8
                                                        implicitHeight: contentHeight
                                                        model: channelLayoutSelector.popup.visible
                                                               ? channelLayoutSelector.delegateModel
                                                               : null
                                                        currentIndex: channelLayoutSelector.highlightedIndex
                                                        highlightMoveDuration: 0
                                                        boundsBehavior: Flickable.StopAtBounds
                                                        ScrollIndicator.vertical: ScrollIndicator { }
                                                    }

                                                    background: Rectangle {
                                                        radius: 5
                                                        color: "#242426"
                                                        border.width: 1
                                                        border.color: "#4f4d51"
                                                    }
                                                }

                                                onActivated: function(index) {
                                                    guiController.selectChannelLayout(guiController.channelLayoutOptions[index])
                                                }
                                            }

                                            ColumnLayout {
                                                id: channelLayoutDetails
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                y: {
                                                    const tagsBottomY = spatialAudioMetadataColumn.y +
                                                                        spatialAudioTags.y +
                                                                        spatialAudioTags.height -
                                                                        channelLayoutControls.y
                                                    // Font glyphs sit slightly above the bottom of their control box.
                                                    const visualBottomAdjustment = 2
                                                    const targetY = tagsBottomY - implicitHeight +
                                                                    visualBottomAdjustment
                                                    const maximumY = Math.max(0,
                                                                              channelLayoutControls.height - implicitHeight)
                                                    if (guiController.channelLayoutOptions.length === 0)
                                                        return Math.max(0, Math.min(maximumY, targetY))

                                                    const minimumY = channelLayoutSelector.y +
                                                                     channelLayoutSelector.height + 6
                                                    const boundedMinimumY = Math.min(minimumY, maximumY)
                                                    return Math.max(boundedMinimumY,
                                                                    Math.min(maximumY, targetY))
                                                }
                                                spacing: 3

                                                Label {
                                                    id: channelOrderDescriptionLabel
                                                    Layout.fillWidth: true
                                                    text: guiController.channelOrderDescription
                                                    font.family: window.monoFont
                                                    font.pixelSize: 10
                                                    color: "#b4b4b4"
                                                    wrapMode: Text.WordWrap
                                                    maximumLineCount: 2
                                                    elide: Text.ElideRight

                                                    HoverHandler {
                                                        id: channelOrderDescriptionHover
                                                    }

                                                    ToolTip {
                                                        parent: channelOrderDescriptionLabel
                                                        visible: channelOrderDescriptionHover.hovered &&
                                                                 channelOrderDescriptionLabel.text.length > 0
                                                        delay: 500
                                                        contentItem: Text {
                                                            text: "Channels will be interpreted in this order and remapped for IAMF."
                                                            width: 320
                                                            font.family: window.font.family
                                                            font.pixelSize: 12
                                                            color: "#f1f1f1"
                                                            textFormat: Text.PlainText
                                                            wrapMode: Text.WordWrap
                                                        }
                                                        background: Rectangle {
                                                            radius: 5
                                                            color: "#242426"
                                                            border.width: 1
                                                            border.color: "#4f4d51"
                                                        }
                                                    }
                                                }

                                                Label {
                                                    visible: !guiController.channelLayoutConfirmed &&
                                                             guiController.channelLayoutStatus.length > 0
                                                    Layout.fillWidth: true
                                                    text: guiController.channelLayoutStatus
                                                    font.pixelSize: 12
                                                    color: guiController.channelLayoutConfirmed ? "#a7c7ab" : "#ffc48a"
                                                    wrapMode: Text.WordWrap
                                                    maximumLineCount: 2
                                                    elide: Text.ElideRight
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle {
                            visible: guiController.ambisonicsMode
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.preferredHeight: 190
                            enabled: !guiController.jobRunning
                            color: "#1e1e1e"
                            border.width: 0
                            border.color: "#343434"
                            radius: 4
                            layer.enabled: true
                            layer.effect: MultiEffect {
                                shadowEnabled: true
                                shadowColor: "#000000"
                                shadowOpacity: 0.30
                                shadowBlur: 0.55
                                shadowVerticalOffset: 2
                            }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 12

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Rectangle {
                                        Layout.preferredWidth: 3
                                        Layout.preferredHeight: 18
                                        radius: 1
                                        color: "#bdbdbd"
                                    }

                                    Label {
                                        text: "HEAD-LOCKED AUDIO INPUT"
                                        font.pixelSize: 14
                                        font.weight: Font.DemiBold
                                        color: "#f0f0f0"
                                        Layout.fillWidth: true
                                    }

                                    Label {
                                        text: "OPTIONAL"
                                        font.family: window.monoFont
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                        color: "#9bbbe0"
                                        Layout.fillWidth: true
                                        horizontalAlignment: Text.AlignRight
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    visible: guiController.headLockedStereoPath.length === 0
                                    enabled: !guiController.jobRunning
                                    opacity: enabled ? 1.0 : 0.48
                                    color: "#0e0e0e"
                                    border.width: headDropArea.compatibleDrag || headDropHover.hovered ? 2 : 1
                                    border.color: headDropArea.compatibleDrag
                                                 ? "#d0d0d0"
                                                 : (headDropHover.hovered ? "#6f8fb8" : "#404040")
                                    radius: 4

                                    HoverHandler {
                                        id: headDropHover
                                    }

                                    TapHandler {
                                        acceptedButtons: Qt.LeftButton
                                        onTapped: window.browseInput("headlocked")
                                    }

                                    DropArea {
                                        id: headDropArea
                                        property bool compatibleDrag: false
                                        anchors.fill: parent
                                        onEntered: function(drag) {
                                            compatibleDrag = window.canAcceptDrag(drag, "headlocked")
                                            drag.accepted = window.canReceiveDrop(drag)
                                        }
                                        onExited: compatibleDrag = false
                                        onDropped: function(drop) {
                                            drop.accepted = window.canReceiveDrop(drop)
                                            if (drop.accepted)
                                                window.sendDrop(drop, "headlocked")
                                            compatibleDrag = false
                                        }
                                    }

                                    ColumnLayout {
                                        anchors.centerIn: parent
                                        spacing: 10

                                        Image {
                                            Layout.alignment: Qt.AlignHCenter
                                            Layout.preferredWidth: 44
                                            Layout.preferredHeight: 44
                                            source: "qrc:/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/stereo_symbol.svg"
                                            sourceSize.width: 44
                                            sourceSize.height: 44
                                            fillMode: Image.PreserveAspectFit
                                            opacity: 1.0
                                            smooth: true
                                            mipmap: true
                                        }

                                        Text {
                                            Layout.alignment: Qt.AlignHCenter
                                            Layout.preferredWidth: 390
                                            text: 'Drag and drop a stereo file (2ch only) here or <a href="browse">Browse</a>'
                                            textFormat: Text.RichText
                                            linkColor: "#a3c9ff"
                                            color: "#c0c0c0"
                                            font.pixelSize: 13
                                            horizontalAlignment: Text.AlignHCenter
                                            wrapMode: Text.WordWrap
                                        }


                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    visible: guiController.headLockedStereoPath.length > 0
                                    enabled: !guiController.jobRunning
                                    opacity: enabled ? 1.0 : 0.48
                                    color: "#0e0e0e"
                                    border.width: headInfoHover.hovered ? 2 : 1
                                    border.color: headInfoHover.hovered ? "#8fcf9a" : "#5f9668"
                                    radius: 4

                                    HoverHandler {
                                        id: headInfoHover
                                    }

                                    TapHandler {
                                        acceptedButtons: Qt.LeftButton
                                        onTapped: window.browseInput("headlocked")
                                    }

                                    Button {
                                        id: clearHeadInfoButton
                                        anchors.right: parent.right
                                        anchors.rightMargin: 16
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 30
                                        height: 30
                                        text: "\u2715"
                                        onClicked: guiController.clearInput("headlocked")
                                        background: Rectangle {
                                            radius: 15
                                            color: clearHeadInfoButton.hovered ? "#343434" : "transparent"
                                            border.width: 0
                                        }
                                        contentItem: Text {
                                            anchors.centerIn: parent
                                            text: clearHeadInfoButton.text
                                            color: clearHeadInfoButton.hovered ? "#f0f0f0" : "#a7a7a7"
                                            font.pixelSize: 19
                                            font.weight: Font.DemiBold
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                    }
                                    Item {
                                        anchors.left: parent.left
                                        anchors.leftMargin: 18
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 52
                                        height: 52

                                        Image {
                                            anchors.centerIn: parent
                                            width: 44
                                            height: 44
                                            source: "qrc:/qt/qml/ImmersiveAudioMuxer/Gui/qml/assets/stereo_symbol.svg"
                                            sourceSize.width: width
                                            sourceSize.height: height
                                            fillMode: Image.PreserveAspectFit
                                            smooth: true
                                            mipmap: true
                                        }
                                    }

                                    Item {
                                        anchors.fill: parent
                                        anchors.margins: 18
                                        anchors.leftMargin: 84
                                        anchors.rightMargin: 52

                                        Label {
                                            id: headLockedStereoFileNameLabel
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.bottom: headLockedStereoPathLabel.top
                        anchors.bottomMargin: 10
                                            text: window.fileNameFromPath(guiController.headLockedStereoPath)
                                            font.family: window.font.family
                                            font.pixelSize: 17
                                            font.weight: Font.DemiBold
                                            color: "#e5e2e1"
                                            elide: Text.ElideRight

                                            HoverHandler { id: headLockedStereoFileNameHover }

                                            ToolTip {
                                                parent: headLockedStereoFileNameLabel
                                                visible: headLockedStereoFileNameHover.hovered &&
                                                         headLockedStereoFileNameLabel.truncated
                                                delay: 500
                                                contentItem: Text {
                                                    text: headLockedStereoFileNameLabel.text
                                                    font.family: window.font.family
                                                    font.pixelSize: 13
                                                    font.weight: Font.Normal
                                                    color: "#f1f1f1"
                                                    textFormat: Text.PlainText
                                                    wrapMode: Text.NoWrap
                                                }
                                                background: Rectangle {
                                                    radius: 5
                                                    color: "#242426"
                                                    border.width: 1
                                                    border.color: "#4f4d51"
                                                }
                                            }
                                        }

                                        Label {
                                            id: headLockedStereoPathLabel
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: guiController.headLockedStereoPath
                                            font.family: window.monoFont
                                            font.pixelSize: 12
                                            color: "#9d9d9d"
                                            elide: Text.ElideMiddle
                                        }

                                        Flow {
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.top: headLockedStereoPathLabel.bottom
                        anchors.topMargin: 10
                                            spacing: 6

                                            Repeater {
                                                model: guiController.headLockedStereoTags
                                                delegate: metadataTagDelegate
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 190
                        enabled: !guiController.jobRunning
                        color: "#1e1e1e"
                        border.width: 0
                        border.color: "#343434"
                        radius: 4
                        layer.enabled: true
                        layer.effect: MultiEffect {
                            shadowEnabled: true
                            shadowColor: "#000000"
                            shadowOpacity: 0.30
                            shadowBlur: 0.55
                            shadowVerticalOffset: 2
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Rectangle {
                                    Layout.preferredWidth: 3
                                    Layout.preferredHeight: 18
                                    radius: 1
                                    color: "#d0d0d0"
                                }

                                Label {
                                    text: "VIDEO INPUT"
                                    font.pixelSize: 14
                                    font.weight: Font.DemiBold
                                    color: "#f0f0f0"
                                    Layout.fillWidth: true
                                }

                                Label {
                                    text: "OPTIONAL"
                                    font.family: window.monoFont
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                    color: "#9bbbe0"
                                    Layout.fillWidth: true
                                    horizontalAlignment: Text.AlignRight
                                }

                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                visible: !guiController.hasVideo
                                enabled: !guiController.jobRunning
                                opacity: enabled ? 1.0 : 0.48
                                color: "#0e0e0e"
                                border.width: videoDropArea.compatibleDrag || videoDropHover.hovered ? 2 : 1
                                border.color: videoDropArea.compatibleDrag
                                             ? "#d0d0d0"
                                             : (videoDropHover.hovered ? "#6f8fb8" : "#404040")
                                radius: 4

                                HoverHandler {
                                    id: videoDropHover
                                }

                                TapHandler {
                                    acceptedButtons: Qt.LeftButton
                                    onTapped: window.browseInput("video")
                                }

                                DropArea {
                                    id: videoDropArea
                                    property bool compatibleDrag: false
                                    anchors.fill: parent
                                    onEntered: function(drag) {
                                        compatibleDrag = window.canAcceptDrag(drag, "video")
                                        drag.accepted = window.canReceiveDrop(drag)
                                    }
                                    onExited: compatibleDrag = false
                                    onDropped: function(drop) {
                                        drop.accepted = window.canReceiveDrop(drop)
                                        if (drop.accepted)
                                            window.sendDrop(drop, "video")
                                        compatibleDrag = false
                                    }
                                }

                                ColumnLayout {
                                    anchors.centerIn: parent
                                    spacing: 10

                                    Item {
                                        Layout.alignment: Qt.AlignHCenter
                                        Layout.preferredWidth: 34
                                        Layout.preferredHeight: 34

                                        Canvas {
                                            id: videoIcon
                                            anchors.fill: parent
                                            antialiasing: true

                                            onPaint: {
                                                const context = getContext("2d")
                                                const stroke = "#ffffff"
                                                context.clearRect(0, 0, width, height)
                                                context.strokeStyle = stroke
                                                context.lineWidth = 1.6
                                                context.lineJoin = "round"
                                                context.lineCap = "round"

                                                context.beginPath()
                                                context.moveTo(8, 6)
                                                context.lineTo(22, 6)
                                                context.quadraticCurveTo(26, 6, 26, 10)
                                                context.lineTo(26, 24)
                                                context.quadraticCurveTo(26, 28, 22, 28)
                                                context.lineTo(8, 28)
                                                context.quadraticCurveTo(4, 28, 4, 24)
                                                context.lineTo(4, 10)
                                                context.quadraticCurveTo(4, 6, 8, 6)
                                                context.closePath()
                                                context.stroke()

                                                context.beginPath()
                                                context.moveTo(26, 13)
                                                context.lineTo(31, 10)
                                                context.lineTo(31, 24)
                                                context.lineTo(26, 21)
                                                context.closePath()
                                                context.stroke()
                                            }
                                        }

                                        Connections {
                                            target: videoDropArea
                                            function onCompatibleDragChanged() {
                                                videoIcon.requestPaint()
                                            }
                                        }
                                    }

                                    Text {
                                        Layout.alignment: Qt.AlignHCenter
                                        Layout.preferredWidth: 390
                                        text: 'Drag and drop video file here or <a href="browse">Browse</a>'
                                        textFormat: Text.RichText
                                        linkColor: "#a3c9ff"
                                        color: "#c0c0c0"
                                        font.pixelSize: 13
                                        horizontalAlignment: Text.AlignHCenter
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                visible: guiController.hasVideo
                                enabled: !guiController.jobRunning
                                opacity: enabled ? 1.0 : 0.48
                                color: "#0e0e0e"
                                border.width: videoInfoDropArea.compatibleDrag || videoInfoHover.hovered ? 2 : 1
                                border.color: videoInfoDropArea.compatibleDrag
                                             ? "#d0d0d0"
                                             : (videoInfoHover.hovered ? "#8fcf9a" : "#5f9668")
                                radius: 4

                                HoverHandler {
                                    id: videoInfoHover
                                }

                                DropArea {
                                    id: videoInfoDropArea
                                    property bool compatibleDrag: false
                                    anchors.fill: parent
                                    onEntered: function(drag) {
                                        compatibleDrag = window.canAcceptDrag(drag, "video")
                                        drag.accepted = window.canReceiveDrop(drag)
                                    }
                                    onExited: compatibleDrag = false
                                    onDropped: function(drop) {
                                        drop.accepted = window.canReceiveDrop(drop)
                                        if (drop.accepted)
                                            window.sendDrop(drop, "video")
                                        compatibleDrag = false
                                    }
                                }

                                TapHandler {
                                    acceptedButtons: Qt.LeftButton
                                    onTapped: window.browseInput("video")
                                }

                                Button {
                                    id: clearVideoInfoButton
                                    anchors.right: parent.right
                                    anchors.rightMargin: 16
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 30
                                    height: 30
                                    text: "\u2715"
                                    onClicked: guiController.clearInput("video")
                                    background: Rectangle {
                                        radius: 15
                                        color: clearVideoInfoButton.hovered ? "#343434" : "transparent"
                                        border.width: 0
                                    }
                                    contentItem: Text {
                                        anchors.centerIn: parent
                                        text: clearVideoInfoButton.text
                                        color: clearVideoInfoButton.hovered ? "#f0f0f0" : "#a7a7a7"
                                        font.pixelSize: 19
                                        font.weight: Font.DemiBold
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }
                                Item {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 18
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 52
                                    height: 52

                                    Canvas {
                                        anchors.centerIn: parent
                                        width: 34
                                        height: 34
                                        antialiasing: true

                                        onPaint: {
                                            const context = getContext("2d")
                                            context.clearRect(0, 0, width, height)
                                            context.strokeStyle = "#ffffff"
                                            context.lineWidth = 1.6
                                            context.lineJoin = "round"
                                            context.lineCap = "round"

                                            context.beginPath()
                                            context.moveTo(8, 6)
                                            context.lineTo(22, 6)
                                            context.quadraticCurveTo(26, 6, 26, 10)
                                            context.lineTo(26, 24)
                                            context.quadraticCurveTo(26, 28, 22, 28)
                                            context.lineTo(8, 28)
                                            context.quadraticCurveTo(4, 28, 4, 24)
                                            context.lineTo(4, 10)
                                            context.quadraticCurveTo(4, 6, 8, 6)
                                            context.closePath()
                                            context.stroke()

                                            context.beginPath()
                                            context.moveTo(26, 13)
                                            context.lineTo(31, 10)
                                            context.lineTo(31, 24)
                                            context.lineTo(26, 21)
                                            context.closePath()
                                            context.stroke()
                                        }
                                    }
                                }

                                Item {
                                    anchors.fill: parent
                                    anchors.margins: 18
                                    anchors.leftMargin: 84
                                    anchors.rightMargin: 52

                                    Label {
                                        id: videoFileNameLabel
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.bottom: videoPathLabel.top
                        anchors.bottomMargin: 10
                                        text: window.fileNameFromPath(guiController.videoPath)
                                        font.family: window.font.family
                                        font.pixelSize: 17
                                        font.weight: Font.DemiBold
                                        color: "#e5e2e1"
                                        elide: Text.ElideRight

                                        HoverHandler { id: videoFileNameHover }

                                        ToolTip {
                                            parent: videoFileNameLabel
                                            visible: videoFileNameHover.hovered && videoFileNameLabel.truncated
                                            delay: 500
                                            contentItem: Text {
                                                text: videoFileNameLabel.text
                                                font.family: window.font.family
                                                font.pixelSize: 13
                                                font.weight: Font.Normal
                                                color: "#f1f1f1"
                                                textFormat: Text.PlainText
                                                wrapMode: Text.NoWrap
                                            }
                                            background: Rectangle {
                                                radius: 5
                                                color: "#242426"
                                                border.width: 1
                                                border.color: "#4f4d51"
                                            }
                                        }
                                    }

                                    Label {
                                        id: videoPathLabel
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: guiController.videoPath
                                        font.family: window.monoFont
                                        font.pixelSize: 12
                                        color: "#9d9d9d"
                                        elide: Text.ElideMiddle
                                    }

                                    Flow {
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.top: videoPathLabel.bottom
                        anchors.topMargin: 10
                                        spacing: 6

                                        Repeater {
                                            model: guiController.videoTags
                                            delegate: metadataTagDelegate
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 238
                        color: "#1e1e1e"
                        border.width: 0
                        border.color: "#343434"
                        radius: 4
                        layer.enabled: true
                        layer.effect: MultiEffect {
                            shadowEnabled: true
                            shadowColor: "#000000"
                            shadowOpacity: 0.30
                            shadowBlur: 0.55
                            shadowVerticalOffset: 2
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Rectangle {
                                    Layout.preferredWidth: 3
                                    Layout.preferredHeight: 18
                                    radius: 1
                                    color: "#eeeeee"
                                }

                                Label {
                                    text: "OUTPUT OPTIONS"
                                    font.pixelSize: 14
                                    font.weight: Font.DemiBold
                                    color: "#f0f0f0"
                                    Layout.fillWidth: true
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12

                                ColumnLayout {
                                    enabled: !guiController.jobRunning &&
                                             (guiController.ambisonicsMode || guiController.channelBasedMode)
                                    opacity: enabled ? 1.0 : 0.48
                                    Layout.fillWidth: true
                                    spacing: 4

                                    Label {
                                        text: "MIX PRESENTATION ANNOTATION"
                                        font.family: window.monoFont
                                        font.pixelSize: 10
                                        color: "#8f8f8f"
                                    }

                                    TextField {
                                        id: mixAnnotationField
                                        Layout.fillWidth: true
                                        text: guiController.mixAnnotation
                                        onTextChanged: if (text !== guiController.mixAnnotation) guiController.mixAnnotation = text
                                        font.family: window.monoFont
                                        font.pixelSize: 11
                                        color: "#dedede"
                                        placeholderText: "default mix presentation"
                                        placeholderTextColor: "#777777"
                                        leftPadding: 12
                                        rightPadding: 12
                                        background: Rectangle {
                                            radius: 4
                                            color: !mixAnnotationField.enabled
                                                   ? "#202020"
                                                   : (mixAnnotationField.activeFocus
                                                      ? "#292b30"
                                                      : (mixAnnotationField.hovered ? "#2b2b2e" : "#242426"))
                                            border.width: 1
                                            border.color: !mixAnnotationField.enabled
                                                          ? "#363638"
                                                          : (mixAnnotationField.activeFocus
                                                             ? "#4f8df7"
                                                             : (mixAnnotationField.hovered ? "#66646a" : "#454449"))
                                            layer.enabled: mixAnnotationField.activeFocus
                                            layer.effect: MultiEffect {
                                                shadowEnabled: true
                                                shadowColor: "#3478f6"
                                                shadowOpacity: 0.18
                                                shadowBlur: 0.35
                                                shadowVerticalOffset: 0
                                            }
                                        }
                                    }
                                }

                                ColumnLayout {
                                    enabled: !guiController.jobRunning &&
                                             (guiController.ambisonicsMode || guiController.iamfMode ||
                                              guiController.channelBasedMode)
                                    opacity: enabled ? 1.0 : 0.48
                                    width: 104
                                    Layout.minimumWidth: 104
                                    Layout.preferredWidth: 104
                                    Layout.maximumWidth: 104
                                    spacing: 4

                                    Label {
                                        text: "CODEC"
                                        font.family: window.monoFont
                                        font.pixelSize: 10
                                        color: "#8f8f8f"
                                    }

                                    Basic.ComboBox {
                                        id: codecSelector
                                        width: 104
                                        implicitWidth: 104
                                        Layout.fillWidth: false
                                        Layout.minimumWidth: 104
                                        Layout.preferredWidth: 104
                                        Layout.maximumWidth: 104
                                        Layout.minimumHeight: outputPathField.implicitHeight
                                        Layout.preferredHeight: outputPathField.implicitHeight
                                        Layout.maximumHeight: outputPathField.implicitHeight
                                        model: guiController.iamfMode
                                               ? [guiController.standaloneIamfCodec]
                                               : ["OPUS", "LPCM", "AAC-LC", "FLAC"]
                                        currentIndex: guiController.iamfMode ? 0 : guiController.generatedCodec
                                        enabled: (guiController.ambisonicsMode || guiController.channelBasedMode) &&
                                                 !guiController.iamfMode && !guiController.jobRunning
                                        onActivated: function(index) {
                                            if ((guiController.ambisonicsMode || guiController.channelBasedMode) &&
                                                !guiController.iamfMode)
                                                guiController.generatedCodec = index
                                        }
                                        font.family: window.monoFont
                                        font.pixelSize: 11

                                        contentItem: Text {
                                            leftPadding: 0
                                            rightPadding: 0
                                            text: codecSelector.displayText
                                            color: codecSelector.enabled ? "#d8d6d4" : "#92908f"
                                            font: codecSelector.font
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                            elide: Text.ElideRight
                                        }

                                        indicator: Item {
                                            visible: codecSelector.enabled
                                            x: codecSelector.width - width - 10
                                            y: (codecSelector.height - height) / 2
                                            width: 12
                                            height: 12

                                            Canvas {
                                                anchors.fill: parent
                                                onPaint: {
                                                    const context = getContext("2d")
                                                    context.clearRect(0, 0, width, height)
                                                    context.strokeStyle = codecSelector.pressed ? "#c7c4c1" : "#999795"
                                                    context.lineWidth = 1.25
                                                    context.lineCap = "round"
                                                    context.beginPath()
                                                    context.moveTo(2.5, 4.5)
                                                    context.lineTo(6, 8)
                                                    context.lineTo(9.5, 4.5)
                                                    context.stroke()
                                                }
                                            }
                                        }

                                        background: Rectangle {
                                            radius: 4
                                            color: !codecSelector.enabled
                                                   ? "#202022"
                                                   : (codecSelector.pressed ? "#2f2f31" : (codecSelector.hovered ? "#2b2b2d" : "#252527"))
                                            border.width: 1
                                            border.color: !codecSelector.enabled
                                                          ? "#39383a"
                                                          : (codecSelector.activeFocus ? "#777477" : (codecSelector.hovered ? "#5d5b5f" : "#454347"))
                                        }

                                        delegate: Basic.ItemDelegate {
                                            width: codecSelector.popup.width - 8
                                            height: 32
                                            highlighted: codecSelector.highlightedIndex === index

                                            contentItem: Text {
                                                leftPadding: 10
                                                text: modelData
                                                color: highlighted ? "#e5e2df" : "#d2d0ce"
                                                font.family: window.monoFont
                                                font.pixelSize: 11
                                                verticalAlignment: Text.AlignVCenter
                                            }

                                            background: Rectangle {
                                                radius: 3
                                                color: parent.hovered ? "#373638" : (parent.highlighted ? "#303033" : "transparent")
                                            }
                                        }

                                        popup: Popup {
                                            y: -(height + 4)
                                            width: 104
                                            padding: 4

                                            contentItem: ListView {
                                                clip: true
                                                implicitWidth: 172
                                                implicitHeight: contentHeight
                                                model: codecSelector.popup.visible ? codecSelector.delegateModel : null
                                                currentIndex: codecSelector.highlightedIndex
                                                highlightMoveDuration: 0
                                                boundsBehavior: Flickable.StopAtBounds
                                                ScrollIndicator.vertical: ScrollIndicator { }
                                            }

                                            background: Rectangle {
                                                radius: 5
                                                color: "#242426"
                                                border.width: 1
                                                border.color: "#4f4d51"
                                            }
                                        }
                                    }
                                }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 180
                                Layout.preferredWidth: 210
                                Layout.maximumWidth: 230
                                spacing: 4

                                Label {
                                    text: "VIDEO MAPPING"
                                    font.family: window.monoFont
                                    font.pixelSize: 10
                                    color: "#8f8f8f"
                                }

                                Basic.ComboBox {
                                    id: videoMappingSelector
                                    Layout.fillWidth: true
                                    Layout.minimumHeight: outputPathField.implicitHeight
                                    Layout.preferredHeight: outputPathField.implicitHeight
                                    Layout.maximumHeight: outputPathField.implicitHeight
                                    model: [
                                        "Keep source mapping",
                                        "None - Regular video",
                                        "VR180 - Mono",
                                        "VR180 - Side-by-Side",
                                        "VR180 - Top-Bottom",
                                        "VR360 - Mono",
                                        "VR360 - Side-by-Side",
                                        "VR360 - Top-Bottom"
                                    ]
                                    currentIndex: guiController.videoMappingPreset
                                    onActivated: function(index) { guiController.videoMappingPreset = index }
                                    enabled: guiController.hasVideo && guiController.includeVideo &&
                                             !guiController.jobRunning
                                    opacity: enabled ? 1.0 : 0.48
                                    font.family: window.monoFont
                                    font.pixelSize: 11

                                    contentItem: Text {
                                        leftPadding: 0
                                        rightPadding: 0
                                        text: videoMappingSelector.displayText
                                        color: "#d8d6d4"
                                        font: videoMappingSelector.font
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }

                                    indicator: Item {
                                        x: videoMappingSelector.width - width - 10
                                        y: (videoMappingSelector.height - height) / 2
                                        width: 12
                                        height: 12

                                        Canvas {
                                            anchors.fill: parent
                                            onPaint: {
                                                const context = getContext("2d")
                                                context.clearRect(0, 0, width, height)
                                                context.strokeStyle = videoMappingSelector.pressed ? "#c7c4c1" : "#999795"
                                                context.lineWidth = 1.25
                                                context.lineCap = "round"
                                                context.beginPath()
                                                context.moveTo(2.5, 4.5)
                                                context.lineTo(6, 8)
                                                context.lineTo(9.5, 4.5)
                                                context.stroke()
                                            }
                                        }
                                    }

                                    background: Rectangle {
                                        radius: 4
                                        color: videoMappingSelector.pressed
                                               ? "#2f2f31"
                                               : (videoMappingSelector.hovered ? "#2b2b2d" : "#252527")
                                        border.width: 1
                                        border.color: videoMappingSelector.activeFocus
                                                      ? "#777477"
                                                      : (videoMappingSelector.hovered ? "#5d5b5f" : "#454347")
                                    }

                                    delegate: Basic.ItemDelegate {
                                        width: videoMappingSelector.popup.width - 8
                                        height: 32
                                        highlighted: videoMappingSelector.highlightedIndex === index

                                        contentItem: Text {
                                            leftPadding: 10
                                            text: modelData
                                            color: highlighted ? "#e5e2df" : "#d2d0ce"
                                            font.family: window.monoFont
                                            font.pixelSize: 11
                                            verticalAlignment: Text.AlignVCenter
                                        }

                                        background: Rectangle {
                                            radius: 3
                                            color: parent.hovered
                                                   ? "#373638"
                                                   : (parent.highlighted ? "#303033" : "transparent")
                                        }
                                    }

                                    popup: Popup {
                                        y: -(height + 4)
                                        width: 220
                                        padding: 4

                                        contentItem: ListView {
                                            clip: true
                                            implicitWidth: 252
                                            implicitHeight: contentHeight
                                            model: videoMappingSelector.popup.visible
                                                   ? videoMappingSelector.delegateModel
                                                   : null
                                            currentIndex: videoMappingSelector.highlightedIndex
                                            highlightMoveDuration: 0
                                            boundsBehavior: Flickable.StopAtBounds
                                            ScrollIndicator.vertical: ScrollIndicator { }
                                        }

                                        background: Rectangle {
                                            radius: 5
                                            color: "#242426"
                                            border.width: 1
                                            border.color: "#4f4d51"
                                        }
                                    }
                                }
                            }
                            }


                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4

                                Label {
                                    text: "OUTPUT MP4 PATH"
                                    font.family: window.monoFont
                                    font.pixelSize: 10
                                    color: "#8f8f8f"
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    TextField {
                                        id: outputPathField
                                        Layout.fillWidth: true
                                        enabled: !guiController.jobRunning
                                        opacity: enabled ? 1.0 : 0.48
                                        text: guiController.outputPath
                                        onTextEdited: guiController.outputPath = text
                                        font.family: window.monoFont
                                        font.pixelSize: 11
                                        color: "#dedede"
                                        placeholderText: guiController.outputPathPlaceholder
                                        placeholderTextColor: "#777777"
                                        leftPadding: 12
                                        rightPadding: 12
                                        background: Rectangle {
                                            radius: 4
                                            color: outputPathField.activeFocus
                                                   ? "#292b30"
                                                   : (outputPathField.hovered ? "#2b2b2e" : "#242426")
                                            border.width: 1
                                            border.color: outputPathField.activeFocus
                                                          ? "#4f8df7"
                                                          : (outputPathField.hovered ? "#66646a" : "#454449")
                                            layer.enabled: outputPathField.activeFocus
                                            layer.effect: MultiEffect {
                                                shadowEnabled: true
                                                shadowColor: "#3478f6"
                                                shadowOpacity: 0.18
                                                shadowBlur: 0.35
                                                shadowVerticalOffset: 0
                                            }
                                        }
                                    }

                                    Button {
                                        id: outputBrowseButton
                                        Layout.minimumWidth: outputPathRow.actionButtonWidth
                                        Layout.preferredWidth: outputPathRow.actionButtonWidth
                                        Layout.maximumWidth: outputPathRow.actionButtonWidth
                                        Layout.preferredHeight: outputPathField.implicitHeight
                                        Layout.alignment: Qt.AlignVCenter
                                        text: "Save as..."
                                        enabled: !guiController.jobRunning
                                        opacity: enabled ? 1.0 : 0.48
                                        onClicked: window.browseOutput()
                                        background: Rectangle {
                                            radius: 3
                                            color: outputBrowseButton.hovered ? "#303030" : "#222222"
                                            border.width: 1
                                            border.color: outputBrowseButton.hovered ? "#707070" : "#484848"
                                        }
                                        contentItem: Item {
                                            anchors.fill: parent

                                            Text {
                                                anchors.centerIn: parent
                                                text: outputBrowseButton.text
                                                color: "#d2d2d2"
                                                font.family: window.font.family
                                                font.pixelSize: 12
                                                font.weight: Font.Medium
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                        }
                                    }

                                    Button {
                                        id: openOutputFolderButton
                                        Layout.minimumWidth: outputPathRow.actionButtonWidth
                                        Layout.preferredWidth: outputPathRow.actionButtonWidth
                                        Layout.maximumWidth: outputPathRow.actionButtonWidth
                                        Layout.preferredHeight: outputPathField.implicitHeight
                                        Layout.alignment: Qt.AlignVCenter
                                        text: "Open folder"
                                        enabled: guiController.modeSelected &&
                                                 (String(guiController.outputPath).trim().length > 0 ||
                                                  String(guiController.outputPathPlaceholder).trim().length > 0)
                                        opacity: enabled ? 1.0 : 0.48
                                        onClicked: guiController.openOutputLocation()
                                        ToolTip.visible: hovered
                                        ToolTip.text: "Open the output folder or select the output file"
                                        background: Rectangle {
                                            radius: 3
                                            color: openOutputFolderButton.hovered ? "#303030" : "#222222"
                                            border.width: 1
                                            border.color: openOutputFolderButton.hovered ? "#707070" : "#484848"
                                        }
                                        contentItem: Item {
                                            anchors.fill: parent

                                            Text {
                                                anchors.centerIn: parent
                                                text: openOutputFolderButton.text
                                                color: "#d2d2d2"
                                                font.family: window.font.family
                                                font.pixelSize: 12
                                                font.weight: Font.Medium
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 20

                                CheckBox {
                                    id: includeVideoOption
                                    text: "Include video"
                                    checked: guiController.includeVideo
                                    enabled: guiController.hasVideo && !guiController.jobRunning
                                    onToggled: guiController.includeVideo = checked
                                    leftPadding: 0
                                    rightPadding: 0
                                    spacing: 10
                                    indicator: Rectangle {
                                        x: includeVideoOption.leftPadding
                                        y: includeVideoOption.topPadding + (includeVideoOption.availableHeight - height) / 2
                                        implicitWidth: 16
                                        implicitHeight: 16
                                        radius: 4
                                        color: !includeVideoOption.enabled
                                               ? "#202020"
                                               : (includeVideoOption.checked
                                                  ? "#425b7c"
                                                  : (includeVideoOption.hovered ? "#303033" : "#242426"))
                                        border.width: 1
                                        border.color: !includeVideoOption.enabled
                                                      ? "#3a3a3c"
                                                      : (includeVideoOption.checked
                                                         ? "#60789a"
                                                         : (includeVideoOption.hovered ? "#67656a" : "#4a484d"))
                                        Canvas {
                                            anchors.centerIn: parent
                                            width: 10
                                            height: 8
                                            visible: includeVideoOption.checked
                                            property color checkColor: includeVideoOption.enabled ? "#ffffff" : "#666a72"
                                            onCheckColorChanged: requestPaint()
                                            onVisibleChanged: if (visible) requestPaint()
                                            onPaint: {
                                                var context = getContext("2d")
                                                context.clearRect(0, 0, width, height)
                                                context.strokeStyle = checkColor
                                                context.lineWidth = 1.6
                                                context.lineCap = "square"
                                                context.lineJoin = "miter"
                                                context.beginPath()
                                                context.moveTo(1, 4)
                                                context.lineTo(3.5, 6.5)
                                                context.lineTo(9, 1)
                                                context.stroke()
                                            }
                                        }
                                    }
                                    contentItem: Text {
                                        text: includeVideoOption.text
                                        leftPadding: includeVideoOption.indicator.implicitWidth + includeVideoOption.spacing
                                        color: includeVideoOption.enabled ? "#e5e2e1" : "#6b6b6b"
                                        font.family: window.font.family
                                        font.pixelSize: 13
                                        font.weight: Font.Medium
                                        verticalAlignment: Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }
                                }

                                CheckBox {
                                    id: keepAudioOption
                                    text: "Keep audio tracks from source"
                                    checked: guiController.keepMp4Audio
                                    enabled: guiController.hasVideo && guiController.videoHasAudio &&
                                             !guiController.jobRunning
                                    onToggled: guiController.keepMp4Audio = checked
                                    leftPadding: 0
                                    rightPadding: 0
                                    spacing: 10
                                    indicator: Rectangle {
                                        x: keepAudioOption.leftPadding
                                        y: keepAudioOption.topPadding + (keepAudioOption.availableHeight - height) / 2
                                        implicitWidth: 16
                                        implicitHeight: 16
                                        radius: 4
                                        color: !keepAudioOption.enabled
                                               ? "#202020"
                                               : (keepAudioOption.checked
                                                  ? "#425b7c"
                                                  : (keepAudioOption.hovered ? "#303033" : "#242426"))
                                        border.width: 1
                                        border.color: !keepAudioOption.enabled
                                                      ? "#3a3a3c"
                                                      : (keepAudioOption.checked
                                                         ? "#60789a"
                                                         : (keepAudioOption.hovered ? "#67656a" : "#4a484d"))
                                        Canvas {
                                            anchors.centerIn: parent
                                            width: 10
                                            height: 8
                                            visible: keepAudioOption.checked
                                            property color checkColor: keepAudioOption.enabled ? "#ffffff" : "#666a72"
                                            onCheckColorChanged: requestPaint()
                                            onVisibleChanged: if (visible) requestPaint()
                                            onPaint: {
                                                var context = getContext("2d")
                                                context.clearRect(0, 0, width, height)
                                                context.strokeStyle = checkColor
                                                context.lineWidth = 1.6
                                                context.lineCap = "square"
                                                context.lineJoin = "miter"
                                                context.beginPath()
                                                context.moveTo(1, 4)
                                                context.lineTo(3.5, 6.5)
                                                context.lineTo(9, 1)
                                                context.stroke()
                                            }
                                        }
                                    }
                                    contentItem: Text {
                                        text: keepAudioOption.text
                                        leftPadding: keepAudioOption.indicator.implicitWidth + keepAudioOption.spacing
                                        color: keepAudioOption.enabled ? "#e5e2e1" : "#6b6b6b"
                                        font.family: window.font.family
                                        font.pixelSize: 13
                                        font.weight: Font.Medium
                                        verticalAlignment: Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }
                                }

                                CheckBox {
                                    id: overwriteFilesOption
                                    text: "Overwrite existing files"
                                    checked: guiController.overwriteExistingFiles
                                    enabled: !guiController.jobRunning
                                    opacity: enabled ? 1.0 : 0.48
                                    onToggled: guiController.overwriteExistingFiles = checked
                                    leftPadding: 0
                                    rightPadding: 0
                                    spacing: 10
                                    indicator: Rectangle {
                                        x: overwriteFilesOption.leftPadding
                                        y: overwriteFilesOption.topPadding + (overwriteFilesOption.availableHeight - height) / 2
                                        implicitWidth: 16
                                        implicitHeight: 16
                                        radius: 4
                                        color: overwriteFilesOption.checked
                                               ? "#425b7c"
                                               : (overwriteFilesOption.hovered ? "#303033" : "#242426")
                                        border.width: 1
                                        border.color: overwriteFilesOption.checked
                                                      ? "#60789a"
                                                      : (overwriteFilesOption.hovered ? "#67656a" : "#4a484d")
                                        Canvas {
                                            anchors.centerIn: parent
                                            width: 10
                                            height: 8
                                            visible: overwriteFilesOption.checked
                                            property color checkColor: "#ffffff"
                                            onCheckColorChanged: requestPaint()
                                            onVisibleChanged: if (visible) requestPaint()
                                            onPaint: {
                                                var context = getContext("2d")
                                                context.clearRect(0, 0, width, height)
                                                context.strokeStyle = checkColor
                                                context.lineWidth = 1.6
                                                context.lineCap = "square"
                                                context.lineJoin = "miter"
                                                context.beginPath()
                                                context.moveTo(1, 4)
                                                context.lineTo(3.5, 6.5)
                                                context.lineTo(9, 1)
                                                context.stroke()
                                            }
                                        }
                                    }
                                    contentItem: Text {
                                        text: overwriteFilesOption.text
                                        leftPadding: overwriteFilesOption.indicator.implicitWidth + overwriteFilesOption.spacing
                                        color: "#e5e2e1"
                                        font.family: window.font.family
                                        font.pixelSize: 13
                                        font.weight: Font.Medium
                                        verticalAlignment: Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }
                                }

                                Item {
                                    Layout.fillWidth: true
                                }
                            }


                        }
                    }
                }
            }

            Item {
                id: startActionArea
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 70
                z: 1

                Button {
                    id: startButton
                    anchors.centerIn: parent

                    width: 220
                    height: 46
                    text: guiController.jobRunning ? "CANCEL" : "START"
                    enabled: guiController.jobRunning || guiController.projectReady
                    onClicked: guiController.jobRunning ? guiController.cancelJob() : guiController.prepareProject()
                    scale: !startButton.enabled
                           ? 1.0
                           : (startButton.pressed ? 0.95 : (startButton.hovered ? 1.015 : 1.0))
                    Behavior on scale {
                        NumberAnimation {
                            duration: 120
                            easing.type: Easing.OutCubic
                        }
                    }
                    background: Rectangle {
                        radius: 10
                        color: startButton.enabled
                               ? (startButton.pressed ? "#2f6bd8" : (startButton.hovered ? "#4f91ff" : "#3478f6"))
                               : "#303d52"
                        border.width: 0
                        layer.enabled: startButton.enabled
                        layer.effect: MultiEffect {
                            shadowEnabled: true
                            shadowColor: "#3478f6"
                            shadowOpacity: 0.42
                            shadowBlur: 0.72
                            shadowVerticalOffset: 2
                        }
                        Behavior on color {
                            ColorAnimation {
                                duration: 140
                                easing.type: Easing.OutCubic
                            }
                        }
                    }
                    contentItem: Text {
                        text: startButton.text
                        color: startButton.enabled ? "#ffffff" : "#9baaca"
                        font.family: window.font.family
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: "#0e0e0e"
            border.width: 1
            border.color: "#303030"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 12

                Label {
                    text: guiController.statusText
                    color: "#a0a0a0"
                    font.family: window.monoFont
                    font.pixelSize: 10
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Label {
                    text: "Based on iamf-tools (git " + iamfToolsRevision + ")"
                    color: "#a0a0a0"
                    font.family: window.monoFont
                    font.pixelSize: 10
                    font.weight: Font.Medium
                }
            }
        }
    }
}
