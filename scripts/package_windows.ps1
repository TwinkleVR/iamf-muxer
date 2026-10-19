[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string]$QtPrefix = "",
    [string]$FfmpegPrefix = "",
    [string]$OutputDir = "",
    [string]$Version = "",
    [string]$Arch = "x86_64",
    [string]$IamfToolsRevision = "",
    [string]$IamfToolsSourceDir = "",
    [string]$FfmpegRevision = "",
    [string]$FfmpegLicenseFile = "",
    [switch]$KeepOptionalQtRuntime,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $false)][string[]]$Arguments = @()
    )
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
    }
}

function Resolve-Executable {
    param([Parameter(Mandatory = $true)][string]$Name)
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "Required executable was not found: $Name"
    }
    return $command.Source
}

function Deploy-QtManually {
    param(
        [Parameter(Mandatory = $true)][string]$QtRoot,
        [Parameter(Mandatory = $true)][string]$RuntimeDirectory
    )
    $qtBin = Join-Path $QtRoot "bin"
    $qtPlugins = Join-Path $QtRoot "plugins"
    $qtQml = Join-Path $QtRoot "qml"
    if (-not (Test-Path $qtBin)) {
        throw "Qt bin directory was not found: $qtBin"
    }
    $platformPlugins = Join-Path $qtPlugins "platforms"
    if (Test-Path $platformPlugins) {
        New-Item -ItemType Directory -Path (Join-Path $RuntimeDirectory "plugins") -Force | Out-Null
        Copy-Item -LiteralPath $platformPlugins -Destination (Join-Path $RuntimeDirectory "plugins") -Recurse -Force
    }
    $multimediaPlugins = Join-Path $qtPlugins "multimedia"
    if (Test-Path $multimediaPlugins) {
        New-Item -ItemType Directory -Path (Join-Path $RuntimeDirectory "plugins") -Force | Out-Null
        Copy-Item -LiteralPath $multimediaPlugins -Destination (Join-Path $RuntimeDirectory "plugins") -Recurse -Force
    }
    if (Test-Path $qtQml) {
        $qmlDestination = Join-Path $RuntimeDirectory "qml"
        New-Item -ItemType Directory -Path $qmlDestination -Force | Out-Null
        foreach ($qmlModule in @("Qt", "QtCore", "QtMultimedia", "QtQml", "QtQuick", "QML")) {
            $qmlSource = Join-Path $qtQml $qmlModule
            if (Test-Path $qmlSource) {
                Copy-Item -LiteralPath $qmlSource -Destination $qmlDestination -Recurse -Force
            }
        }
    }
    @"
[Paths]
Prefix=..
Plugins=plugins
Qml2Imports=qml
"@ | Set-Content -LiteralPath (Join-Path $RuntimeDirectory "qt.conf") -Encoding ascii
}

function Get-ImportedDllNames {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string]$Objdump
    )
    & $Objdump -p $FilePath 2>$null |
        Select-String "DLL Name:" |
        ForEach-Object { ($_.Line -replace ".*DLL Name:\s*", "").Trim() } |
        Where-Object { $_ -and $_ -notmatch "^(api-ms-win-|ext-ms-win-|KERNEL32|USER32|ADVAPI32|GDI32|SHELL32|OLE32|OLEAUT32|UUID|COMDLG32|WS2_32|WSOCK32|WINMM|VERSION|MSVCRT|UCRTBASE|NTDLL|DBGHELP|AUTHZ|MPR|NETAPI32|USERENV|D2D1|D3D11|D3D12|DXGI|DXGUID|DWRITE|SHLWAPI|UXTHEME|AVRT|PROPSYS|DNSAPI|IPHLPAPI|SECUR32|WINHTTP|WINSPOOL|DWMAPI|MF|MFPLAT|MFREADWRITE|D3D9|DXVA2|EVR|CRYPT32|BCRYPT|NCRYPT|RPCRT4|IMAGEHLP|OLEACC|IMM32|SHCORE|SETUPAPI|DEVOBJ|POWRPROF|WTSAPI32)" }
}

function Assert-WindowsAbiCompatibility {
    param(
        [Parameter(Mandatory = $true)][string]$EncoderPath,
        [Parameter(Mandatory = $true)][string]$RuntimeDirectory
    )
    $objdumpCommand = Get-Command objdump.exe -ErrorAction SilentlyContinue
    if (-not $objdumpCommand) {
        Write-Warning "objdump was not found; skipping the in-process encoder ABI symbol check."
        return
    }

    $imports = (& $objdumpCommand.Source -p $EncoderPath 2>$null) -join "`n"
    if ($imports -notmatch "(?i)\bpthread_cond_timedwait64\b") {
        return
    }

    $providerPath = Join-Path $RuntimeDirectory "libwinpthread-1.dll"
    if (-not (Test-Path $providerPath)) {
        throw "The in-process encoder imports pthread_cond_timedwait64, but libwinpthread-1.dll is missing from the release package."
    }
    $exports = (& $objdumpCommand.Source -p $providerPath 2>$null) -join "`n"
    if ($exports -notmatch "(?i)\bpthread_cond_timedwait64\b") {
        throw "The release libwinpthread-1.dll does not export pthread_cond_timedwait64 required by iamf_tools_encoder_inprocess.dll. Rebuild the encoder with the static winpthreads link or update the MinGW winpthreads package."
    }
}

function Remove-OptionalQtRuntime {
    param(
        [Parameter(Mandatory = $true)][string]$RuntimeDirectory,
        [switch]$KeepRuntime
    )
    if ($KeepRuntime) {
        Write-Output "Keeping optional Qt runtime files."
        return
    }

    $optionalFiles = @(
        "Qt6Quick3DUtils.dll",
        "Qt6QuickShapes.dll",
        "Qt6LabsFolderListModel.dll",
        "Qt6QuickControls2Imagine.dll",
        "Qt6QuickControls2ImagineStyleImpl.dll",
        "Qt6QuickControls2Material.dll",
        "Qt6QuickControls2MaterialStyleImpl.dll",
        "Qt6QuickControls2Fusion.dll",
        "Qt6QuickControls2FusionStyleImpl.dll",
        "Qt6QuickControls2Universal.dll",
        "Qt6QuickControls2UniversalStyleImpl.dll",
        "Qt6QuickControls2FluentWinUI3StyleImpl.dll",
        "Qt6QuickControls2WindowsStyleImpl.dll"
    )
    $optionalFiles += @(
        "generic\qtuiotouchplugin.dll",
        "networkinformation\qnetworklistmanager.dll",
        "tls\qcertonlybackend.dll",
        "tls\qschannelbackend.dll",
        "imageformats\qgif.dll",
        "imageformats\qico.dll",
        "imageformats\qjpeg.dll"
    )
    $optionalDirectories = @(
        "qmltooling",
        "qml\QtQuick\Controls\Imagine",
        "qml\QtQuick\Controls\Material",
        "qml\QtQuick\Controls\Fusion",
        "qml\QtQuick\Controls\Universal",
        "qml\QtQuick\Controls\Windows",
        "qml\QtQuick\Controls\FluentWinUI3",
        "qml\QtQuick\Controls\NativeStyle",
        "qml\QtQuick\NativeStyle",
        "qml\QtQuick\Particles",
        "qml\QtQuick\Shapes",
        "qml\QtQuick\Timeline",
        "qml\QtQuick\VectorImage",
        "qml\QtQuick\LocalStorage"
    )

    $removedBytes = [int64]0
    $removedItems = New-Object System.Collections.Generic.List[string]
    foreach ($relativePath in ($optionalFiles + $optionalDirectories)) {
        $target = Join-Path $RuntimeDirectory $relativePath
        if (-not (Test-Path -LiteralPath $target)) {
            continue
        }
        $entry = Get-Item -LiteralPath $target
        $entries = if ($entry.PSIsContainer) {
            @(Get-ChildItem -LiteralPath $target -Recurse -File -ErrorAction SilentlyContinue)
        } else {
            @($entry)
        }
        $removedBytes += [int64](($entries | Measure-Object -Property Length -Sum).Sum)
        Remove-Item -LiteralPath $target -Recurse -Force
        $removedItems.Add($relativePath)
    }
    Write-Output ("Pruned optional Qt runtime: {0} item(s), {1:N2} MB." -f
        $removedItems.Count, ($removedBytes / 1MB))
}

function Copy-WindowsDependencyClosure {
    param(
        [Parameter(Mandatory = $true)][string]$RuntimeDirectory,
        [Parameter(Mandatory = $true)][string[]]$SearchRoots,
        [Parameter(Mandatory = $true)][string[]]$InitialFiles
    )
    $objdumpCommand = Get-Command objdump.exe -ErrorAction SilentlyContinue
    if (-not $objdumpCommand) {
        Write-Warning "objdump was not found; copying the known FFmpeg DLL families without dependency closure."
        foreach ($pattern in @("avcodec-*.dll", "avformat-*.dll", "avutil-*.dll", "swresample-*.dll")) {
            $candidate = Get-ChildItem -LiteralPath $SearchRoots -Filter $pattern -File -ErrorAction SilentlyContinue |
                Select-Object -First 1
            if ($candidate) {
                Copy-Item -LiteralPath $candidate.FullName -Destination $RuntimeDirectory -Force
            } else {
                Write-Warning "Could not find FFmpeg runtime DLL matching '$pattern'."
            }
        }
        return
    }

    $queue = New-Object System.Collections.Generic.Queue[string]
    $visited = New-Object System.Collections.Generic.HashSet[string]([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($initial in $InitialFiles) {
        if (Test-Path $initial) {
            $queue.Enqueue((Resolve-Path $initial).Path)
        }
    }
    while ($queue.Count -gt 0) {
        $current = $queue.Dequeue()
        if (-not $visited.Add($current)) {
            continue
        }
        foreach ($dependencyName in (Get-ImportedDllNames -FilePath $current -Objdump $objdumpCommand.Source)) {
            $dependency = Get-ChildItem -LiteralPath $RuntimeDirectory -Filter $dependencyName -File -Recurse -ErrorAction SilentlyContinue |
                Select-Object -First 1
            if (-not $dependency) {
                foreach ($rootPath in $SearchRoots) {
                    $candidate = Join-Path $rootPath $dependencyName
                    if (Test-Path $candidate) {
                        Copy-Item -LiteralPath $candidate -Destination $RuntimeDirectory -Force
                        $dependency = Get-Item -LiteralPath (Join-Path $RuntimeDirectory $dependencyName)
                        break
                    }
                }
            }
            if ($dependency) {
                $queue.Enqueue($dependency.FullName)
            } else {
        Write-Warning "Could not resolve Windows runtime dependency '$dependencyName' imported by '$current'."
            }
        }
    }
}

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $root "build-MINGW64"
}
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $root "package"
}
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = (Get-Content -LiteralPath (Join-Path $root "VERSION.txt") -Raw).Trim()
}
if ([string]::IsNullOrWhiteSpace($IamfToolsRevision)) {
    if ($env:IAMF_TOOLS_GIT_TAG) {
        $IamfToolsRevision = $env:IAMF_TOOLS_GIT_TAG
    } else {
        $IamfToolsRevision = "5ad3728679495d0037c9be7334338f2859d2a3fa"
    }
}
if ([string]::IsNullOrWhiteSpace($QtPrefix) -and $env:Qt6_DIR) {
    $QtPrefix = Split-Path (Split-Path $env:Qt6_DIR -Parent) -Parent
}
if ([string]::IsNullOrWhiteSpace($FfmpegPrefix) -and $env:IMMERSIVE_AUDIO_MUXER_FFMPEG_PREFIX) {
    $FfmpegPrefix = $env:IMMERSIVE_AUDIO_MUXER_FFMPEG_PREFIX
}
if ([string]::IsNullOrWhiteSpace($FfmpegRevision) -and $env:FFMPEG_GIT_TAG) {
    $FfmpegRevision = $env:FFMPEG_GIT_TAG
}

$cmake = Resolve-Executable "cmake.exe"
$buildPath = (Resolve-Path $BuildDir).Path
$iamfSourceWasSpecified = -not [string]::IsNullOrWhiteSpace($IamfToolsSourceDir)
if (-not $iamfSourceWasSpecified -and $env:IAMF_TOOLS_SOURCE_DIR) {
    $IamfToolsSourceDir = $env:IAMF_TOOLS_SOURCE_DIR
    $iamfSourceWasSpecified = $true
}
if ([string]::IsNullOrWhiteSpace($IamfToolsSourceDir)) {
    $IamfToolsSourceDir = Join-Path $buildPath "_deps\official_iamf_tools-src"
}
$iamfSource = ""
if (Test-Path -LiteralPath $IamfToolsSourceDir) {
    $iamfSource = (Resolve-Path -LiteralPath $IamfToolsSourceDir).Path
} elseif ($iamfSourceWasSpecified) {
    throw "iamf-tools source directory was not found: $IamfToolsSourceDir"
}
$outputPath = [System.IO.Path]::GetFullPath($OutputDir)
$stagePath = Join-Path $outputPath "windows-stage"
$packageName = "iamf-muxer-$Version-windows-$Arch"
$packagePath = Join-Path $outputPath $packageName
$zipPath = Join-Path $outputPath "$packageName.zip"

if (-not $NoBuild) {
    Invoke-Native $cmake @("--build", $buildPath, "--config", "Release")
}

if (Test-Path $stagePath) {
    Remove-Item -LiteralPath $stagePath -Recurse -Force
}
if (Test-Path $packagePath) {
    Remove-Item -LiteralPath $packagePath -Recurse -Force
}
if (Test-Path $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null

Invoke-Native $cmake @("--install", $buildPath, "--prefix", $stagePath, "--config", "Release")
$installedArchiveDirectory = Join-Path $stagePath "lib"
if (Test-Path $installedArchiveDirectory) {
    Remove-Item -LiteralPath $installedArchiveDirectory -Recurse -Force
}

$gui = Get-ChildItem -LiteralPath $stagePath -Filter "iamf-muxer.exe" -Recurse -File |
    Select-Object -First 1
if (-not $gui) {
    throw "The installed GUI executable was not found below $stagePath"
}

if ([string]::IsNullOrWhiteSpace($QtPrefix)) {
    throw "QtPrefix is required. Pass -QtPrefix <Qt install directory> or set Qt6_DIR."
}
$windeployqt = Join-Path $QtPrefix "bin\windeployqt.exe"
if (-not (Test-Path $windeployqt)) {
    throw "windeployqt was not found: $windeployqt"
}
try {
    Invoke-Native $windeployqt @(
        "--release",
        "--no-translations",
        "--qmldir", (Join-Path $root "qml"),
        $gui.FullName
    )
} catch {
    Write-Warning "windeployqt failed; falling back to manual Qt deployment: $($_.Exception.Message)"
    Get-ChildItem -LiteralPath $gui.Directory.FullName -Filter "Qt*.dll" -File -ErrorAction SilentlyContinue |
        Remove-Item -Force
    foreach ($partialDirectory in @("plugins", "qml")) {
        $partialPath = Join-Path $gui.Directory.FullName $partialDirectory
        if (Test-Path $partialPath) {
            Remove-Item -LiteralPath $partialPath -Recurse -Force
        }
    }
    $partialQtConf = Join-Path $gui.Directory.FullName "qt.conf"
    if (Test-Path $partialQtConf) {
        Remove-Item -LiteralPath $partialQtConf -Force
    }
    Deploy-QtManually -QtRoot $QtPrefix -RuntimeDirectory $gui.Directory.FullName
}

if ([string]::IsNullOrWhiteSpace($FfmpegPrefix)) {
    throw "FfmpegPrefix is required to collect runtime DLLs."
}
$ffmpegBin = Join-Path $FfmpegPrefix "bin"
if (-not (Test-Path $ffmpegBin)) {
    throw "FFmpeg bin directory was not found: $ffmpegBin"
}
if ([string]::IsNullOrWhiteSpace($FfmpegRevision)) {
    $revisionFile = Join-Path $FfmpegPrefix "FFmpeg-revision.txt"
    if (Test-Path $revisionFile) {
        $FfmpegRevision = (Get-Content -LiteralPath $revisionFile -Raw).Trim()
    }
}
if ([string]::IsNullOrWhiteSpace($FfmpegRevision)) {
    $FfmpegRevision = "unknown"
}
$ffmpegLicense = if (-not [string]::IsNullOrWhiteSpace($FfmpegLicenseFile)) {
    $FfmpegLicenseFile
} else {
    Join-Path $FfmpegPrefix "FFmpeg-LICENSES.txt"
}
$runtimeDir = $gui.Directory.FullName
$gxx = Get-Command g++.exe -ErrorAction SilentlyContinue
$mingwBin = if ($gxx) { Split-Path $gxx.Source -Parent } else { $null }
if (-not $gxx -or [string]::IsNullOrWhiteSpace($mingwBin)) {
    throw "g++.exe is required to collect the MinGW runtime used by the Windows release."
}
$compilerRuntimeRoots = @()
if ($gxx) {
    foreach ($runtimeName in @("libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll")) {
        $runtimePath = Join-Path $mingwBin $runtimeName
        if (-not (Test-Path $runtimePath)) {
            $reportedRuntimePath = (& $gxx.Source "-print-file-name=$runtimeName").Trim()
            if ($reportedRuntimePath -and [System.IO.Path]::IsPathRooted($reportedRuntimePath) -and (Test-Path $reportedRuntimePath)) {
                $runtimePath = $reportedRuntimePath
            }
        }
        if (Test-Path $runtimePath) {
            Copy-Item -LiteralPath $runtimePath -Destination $runtimeDir -Force
            $compilerRuntimeRoots += Split-Path $runtimePath -Parent
        } else {
            throw "Could not find the MinGW runtime DLL '$runtimeName' below '$mingwBin'."
        }
    }
}

$dependencyRoots = @(
    $runtimeDir,
    $ffmpegBin,
    (Join-Path $QtPrefix "bin")
) + $compilerRuntimeRoots + @($mingwBin)
$initialRuntimeFiles = Get-ChildItem -LiteralPath $runtimeDir -Filter "*.dll" -File -Recurse |
    Select-Object -ExpandProperty FullName
Copy-WindowsDependencyClosure -RuntimeDirectory $runtimeDir `
    -SearchRoots ($dependencyRoots | Select-Object -Unique) `
    -InitialFiles (@($gui.FullName) + $initialRuntimeFiles)

# Re-apply the compiler runtime after dependency closure collection. A Qt
# deployment can contain an older DLL with the same basename; the runtime
# selected from the active MinGW toolchain must win in the final package.
foreach ($runtimeName in @("libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll")) {
    $runtimePath = Join-Path $mingwBin $runtimeName
    if (-not (Test-Path $runtimePath)) {
        throw "Could not reapply the MinGW runtime DLL '$runtimeName' from '$mingwBin'."
    }
    Copy-Item -LiteralPath $runtimePath -Destination $runtimeDir -Force
}

Remove-OptionalQtRuntime `
    -RuntimeDirectory $runtimeDir `
    -KeepRuntime:$KeepOptionalQtRuntime

$metadataArgs = @(
    "-DSTAGE_DIR=$stagePath",
    "-DPROJECT_ROOT=$root",
    "-DVERSION=$Version",
    "-DPLATFORM=windows",
    "-DARCH=$Arch",
    "-DIAMF_TOOLS_REVISION=$IamfToolsRevision",
    "-DFFMPEG_REVISION=$FfmpegRevision",
    "-P", (Join-Path $root "cmake\write_release_metadata.cmake")
)
if (-not [string]::IsNullOrWhiteSpace($ffmpegLicense) -and (Test-Path $ffmpegLicense)) {
    $metadataArgs = @("-DFFMPEG_LICENSE_FILE=$ffmpegLicense") + $metadataArgs
}
if (Test-Path (Join-Path $iamfSource "LICENSE")) {
    $metadataArgs = @("-DIAMF_TOOLS_SOURCE_DIR=$iamfSource") + $metadataArgs
}
$spdlogLicense = Join-Path $buildPath "_deps\spdlog-src\LICENSE"
if (Test-Path -LiteralPath $spdlogLicense) {
    $metadataArgs = @("-DSPDLOG_LICENSE_FILE=$spdlogLicense") + $metadataArgs
}
$fmtLicense = Join-Path $buildPath "_deps\spdlog-src\include\spdlog\fmt\bundled\fmt.license.rst"
if (Test-Path -LiteralPath $fmtLicense) {
    $metadataArgs = @("-DSPDLOG_FMT_LICENSE_FILE=$fmtLicense") + $metadataArgs
}
$metadataArgs = @("-DSPDLOG_VERSION=1.17.0") + $metadataArgs
$metadataArgs = @("-DSPDLOG_FMT_VERSION=12.1.0") + $metadataArgs
Invoke-Native $cmake $metadataArgs

$shareDirectory = Join-Path $stagePath "share"
$installedIamfDataDirectory = Join-Path $shareDirectory "iamf_muxer"
$installedLicenseDirectory = Join-Path $installedIamfDataDirectory "licenses"
$packageLicenseDirectory = Join-Path $stagePath "licenses"
# Keep CMake's build metadata out of the portable package and merge its licenses.
if (Test-Path -LiteralPath $installedLicenseDirectory) {
    New-Item -ItemType Directory -Path $packageLicenseDirectory -Force | Out-Null
    foreach ($licenseFile in @(Get-ChildItem -LiteralPath $installedLicenseDirectory -File -Recurse)) {
        $relativeLicensePath = $licenseFile.FullName.Substring(
            $installedLicenseDirectory.Length
        ).TrimStart([char[]]@([char]92, [char]47))
        $destinationLicense = Join-Path $packageLicenseDirectory $relativeLicensePath
        if (Test-Path -LiteralPath $destinationLicense) {
            $sourceHash = (Get-FileHash -LiteralPath $licenseFile.FullName -Algorithm SHA256).Hash
            $destinationHash = (Get-FileHash -LiteralPath $destinationLicense -Algorithm SHA256).Hash
            if ($sourceHash -ne $destinationHash) {
                throw "Conflicting license files found while merging share into licenses: $relativeLicensePath"
            }
        } else {
            $destinationDirectory = Split-Path -Parent $destinationLicense
            New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
            Copy-Item -LiteralPath $licenseFile.FullName -Destination $destinationLicense
        }
    }
}

if (Test-Path -LiteralPath $installedIamfDataDirectory) {
    Remove-Item -LiteralPath $installedIamfDataDirectory -Recurse -Force
}
if (Test-Path -LiteralPath $shareDirectory) {
    $remainingShareItems = @(Get-ChildItem -LiteralPath $shareDirectory -Force)
    if ($remainingShareItems.Count -gt 0) {
        $remainingNames = ($remainingShareItems | Select-Object -ExpandProperty Name) -join ", "
        throw "Unexpected files remain under the Windows package share directory: $remainingNames"
    }
    Remove-Item -LiteralPath $shareDirectory -Force
}

$requiredFiles = @(
    (Join-Path $stagePath "bin\iamf-muxer.exe"),
    (Join-Path $stagePath "bin\iamf_tools_encoder_inprocess.dll"),
    (Join-Path $stagePath "bin\Qt6Core.dll"),
    (Join-Path $stagePath "bin\Qt6Gui.dll"),
    (Join-Path $stagePath "bin\Qt6Multimedia.dll"),
    (Join-Path $stagePath "bin\Qt6MultimediaQuick.dll"),
    (Join-Path $stagePath "bin\Qt6Qml.dll"),
    (Join-Path $stagePath "bin\Qt6Quick.dll"),
    (Join-Path $stagePath "bin\Qt6QuickControls2.dll"),
    (Join-Path $stagePath "bin\Qt6QuickControls2Basic.dll"),
    (Join-Path $stagePath "bin\Qt6QuickControls2BasicStyleImpl.dll"),
    (Join-Path $stagePath "bin\Qt6QuickDialogs2.dll"),
    (Join-Path $stagePath "bin\Qt6QuickDialogs2QuickImpl.dll"),
    (Join-Path $stagePath "bin\Qt6QuickEffects.dll"),
    (Join-Path $stagePath "bin\Qt6QuickLayouts.dll"),
    (Join-Path $stagePath "bin\Qt6Svg.dll"),
    (Join-Path $stagePath "bin\imageformats\qsvg.dll"),
    (Join-Path $stagePath "bin\iconengines\qsvgicon.dll")
)
foreach ($requiredFile in $requiredFiles) {
    if (-not (Test-Path $requiredFile)) {
        throw "Release package is missing required file: $requiredFile"
    }
}
Assert-WindowsAbiCompatibility `
    -EncoderPath (Join-Path $stagePath "bin\iamf_tools_encoder_inprocess.dll") `
    -RuntimeDirectory (Join-Path $stagePath "bin")
if (-not (Get-ChildItem -LiteralPath (Join-Path $stagePath "bin") -Filter "avformat-*.dll" -File -ErrorAction SilentlyContinue)) {
    throw "Release package is missing the FFmpeg avformat runtime DLL"
}
$qtPlatformPluginCandidates = @(
    (Join-Path $stagePath "bin\platforms\qwindows.dll"),
    (Join-Path $stagePath "bin\plugins\platforms\qwindows.dll")
)
if (-not ($qtPlatformPluginCandidates | Where-Object { Test-Path $_ })) {
    throw "Release package is missing the Qt Windows platform plugin"
}
$qtMultimediaPluginCandidates = @(
    (Join-Path $stagePath "bin\multimedia\ffmpegmediaplugin.dll"),
    (Join-Path $stagePath "bin\plugins\multimedia\ffmpegmediaplugin.dll")
)
if (-not ($qtMultimediaPluginCandidates | Where-Object { Test-Path $_ })) {
    throw "Release package is missing the Qt Multimedia FFmpeg backend plugin"
}
if (-not (Test-Path (Join-Path $stagePath "bin\qml\QtQuick"))) {
    throw "Release package is missing the QtQuick QML module"
}
foreach ($requiredQmlDirectory in @(
    "qml\QtQuick\Controls\Basic",
    "qml\QtQuick\Controls\impl",
    "qml\QtQuick\Dialogs",
    "qml\QtQuick\Effects",
    "qml\QtQuick\Layouts"
)) {
    if (-not (Test-Path (Join-Path $stagePath "bin\$requiredQmlDirectory"))) {
        throw "Release package is missing the required QML module directory: $requiredQmlDirectory"
    }
}

New-Item -ItemType Directory -Path $packagePath -Force | Out-Null
Get-ChildItem -LiteralPath $stagePath -Force |
    Copy-Item -Destination $packagePath -Recurse -Force
Compress-Archive -Path (Join-Path $packagePath "*") -DestinationPath $zipPath -CompressionLevel Optimal

Write-Output "Windows package: $zipPath"
