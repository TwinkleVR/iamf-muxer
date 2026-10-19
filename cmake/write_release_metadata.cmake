cmake_minimum_required(VERSION 3.20)

foreach(_required_arg IN ITEMS STAGE_DIR PROJECT_ROOT VERSION PLATFORM ARCH QT_VERSION)
  if(NOT DEFINED ${_required_arg} OR "${${_required_arg}}" STREQUAL "")
    message(FATAL_ERROR "Missing required argument: -D${_required_arg}=...")
  endif()
endforeach()

get_filename_component(_stage_dir "${STAGE_DIR}" ABSOLUTE)
get_filename_component(_project_root "${PROJECT_ROOT}" ABSOLUTE)
set(_licenses_dir "${_stage_dir}/licenses")
file(MAKE_DIRECTORY "${_licenses_dir}")

file(WRITE "${_stage_dir}/VERSION.txt"
  "Immersive Audio Muxer\n"
  "Version: ${VERSION}\n"
  "Platform: ${PLATFORM}\n"
  "Architecture: ${ARCH}\n")

set(_project_license "${_project_root}/LICENSE")
set(_missing_notice_text "")
if(EXISTS "${_project_license}")
  configure_file("${_project_license}" "${_licenses_dir}/PROJECT-LICENSE.txt" COPYONLY)
else()
  file(WRITE "${_licenses_dir}/PROJECT-LICENSE-STATUS.txt"
    "The source checkout does not contain a LICENSE file.\n"
    "Select and add the project's license before publishing a public release.\n")
  string(APPEND _missing_notice_text
    "WARNING: the project license is not included.\n")
endif()

if(DEFINED IAMF_TOOLS_SOURCE_DIR AND
   EXISTS "${IAMF_TOOLS_SOURCE_DIR}/LICENSE")
  configure_file("${IAMF_TOOLS_SOURCE_DIR}/LICENSE"
                 "${_licenses_dir}/iamf-tools-LICENSE.txt" COPYONLY)
else()
  string(APPEND _missing_notice_text
    "WARNING: iamf-tools license text was not found in the supplied source.\n")
endif()

if(DEFINED FFMPEG_LICENSE_FILE AND
   NOT "${FFMPEG_LICENSE_FILE}" STREQUAL "" AND
   EXISTS "${FFMPEG_LICENSE_FILE}")
  configure_file("${FFMPEG_LICENSE_FILE}"
                 "${_licenses_dir}/FFmpeg-LICENSE.txt" COPYONLY)
else()
  string(APPEND _missing_notice_text
    "WARNING: FFmpeg license text was not supplied.\n")
endif()

if(DEFINED SPDLOG_LICENSE_FILE AND
   NOT "${SPDLOG_LICENSE_FILE}" STREQUAL "" AND
   EXISTS "${SPDLOG_LICENSE_FILE}")
  configure_file("${SPDLOG_LICENSE_FILE}"
                 "${_licenses_dir}/spdlog-LICENSE.txt" COPYONLY)
else()
  string(APPEND _missing_notice_text
    "WARNING: spdlog license text was not found in the build dependency cache.\n")
endif()
if(DEFINED SPDLOG_FMT_LICENSE_FILE AND
   NOT "${SPDLOG_FMT_LICENSE_FILE}" STREQUAL "" AND
   EXISTS "${SPDLOG_FMT_LICENSE_FILE}")
  configure_file("${SPDLOG_FMT_LICENSE_FILE}"
                 "${_licenses_dir}/fmt-LICENSE.txt" COPYONLY)
else()
  string(APPEND _missing_notice_text
    "WARNING: bundled fmt license text was not found in the build dependency cache.\n")
endif()

set(_qt_licenses_dir "${_licenses_dir}/Qt")
file(MAKE_DIRECTORY "${_qt_licenses_dir}")
set(_qt_license_destination "${_qt_licenses_dir}/LGPL-3.0-only.txt")
set(_qt_license_source "")
set(_qt_license_download "")

function(_is_qt_lgpl_v3_license _path _result_var)
  set(_valid FALSE)
  if(EXISTS "${_path}")
    file(READ "${_path}" _license_header LIMIT 512)
    if(_license_header MATCHES "GNU LESSER GENERAL PUBLIC LICENSE" AND
       _license_header MATCHES "Version 3")
      set(_valid TRUE)
    endif()
  endif()
  set(${_result_var} "${_valid}" PARENT_SCOPE)
endfunction()

if(DEFINED QT_LICENSE_FILE AND NOT "${QT_LICENSE_FILE}" STREQUAL "" AND
   EXISTS "${QT_LICENSE_FILE}")
  _is_qt_lgpl_v3_license("${QT_LICENSE_FILE}" _qt_local_license_valid)
  if(_qt_local_license_valid)
    set(_qt_license_source "${QT_LICENSE_FILE}")
  else()
    message(WARNING
      "The supplied Qt license file is not a standalone LGPLv3 text: ${QT_LICENSE_FILE}; trying the official download.")
  endif()
endif()

if(_qt_license_source STREQUAL "")
  set(_qt_license_url
    "https://raw.githubusercontent.com/qt/qtbase/v${QT_VERSION}/LICENSES/LGPL-3.0-only.txt")
  set(_qt_license_download "${_stage_dir}/.qt-lgpl-license-download.tmp")
  file(DOWNLOAD "${_qt_license_url}" "${_qt_license_download}"
    STATUS _qt_download_status
    TIMEOUT 20
    TLS_VERIFY ON)
  list(GET _qt_download_status 0 _qt_download_code)
  list(GET _qt_download_status 1 _qt_download_message)

  if("${_qt_download_code}" STREQUAL "0" AND
     EXISTS "${_qt_license_download}")
    _is_qt_lgpl_v3_license("${_qt_license_download}" _qt_downloaded_license_valid)
    set(_qt_downloaded_license_hash_valid TRUE)
    if("${QT_VERSION}" STREQUAL "6.11.1")
      file(SHA256 "${_qt_license_download}" _qt_downloaded_license_hash)
      if(NOT "${_qt_downloaded_license_hash}" STREQUAL
         "da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768")
        set(_qt_downloaded_license_hash_valid FALSE)
      endif()
    endif()

    if(_qt_downloaded_license_valid AND _qt_downloaded_license_hash_valid)
      set(_qt_license_source "${_qt_license_download}")
    else()
      message(WARNING
        "The downloaded Qt LGPLv3 license failed its content or SHA-256 check: ${_qt_license_url}; continuing without the license text.")
    endif()
  else()
    message(WARNING
      "Could not download the Qt LGPLv3 license from ${_qt_license_url} (${_qt_download_code}: ${_qt_download_message}); continuing without the license text.")
  endif()
endif()

if(NOT _qt_license_source STREQUAL "")
  configure_file("${_qt_license_source}" "${_qt_license_destination}" COPYONLY)
else()
  string(APPEND _missing_notice_text
    "WARNING: Qt LGPLv3 license text could not be obtained; see the packaging log.\n")
endif()
if(NOT "${_qt_license_download}" STREQUAL "")
  file(REMOVE "${_qt_license_download}")
endif()

set(_qt_sbom_modules qtbase qtdeclarative qtmultimedia qtsvg)
set(_qt_sbom_missing_modules "")
foreach(_qt_module IN LISTS _qt_sbom_modules)
  set(_qt_sbom_file "${QT_SBOM_DIR}/${_qt_module}-${QT_VERSION}.cdx.json")
  if(DEFINED QT_SBOM_DIR AND EXISTS "${_qt_sbom_file}")
    configure_file("${_qt_sbom_file}"
      "${_qt_licenses_dir}/${_qt_module}-${QT_VERSION}.cdx.json" COPYONLY)
  else()
    list(APPEND _qt_sbom_missing_modules "${_qt_module}")
  endif()
endforeach()
if(_qt_sbom_missing_modules)
  string(JOIN ", " _qt_sbom_missing_text ${_qt_sbom_missing_modules})
  message(WARNING
    "Qt module SBOM file(s) are missing for: ${_qt_sbom_missing_text}; packaging will continue.")
  string(APPEND _missing_notice_text
    "WARNING: Qt module SBOM file(s) were not included for: ${_qt_sbom_missing_text}.\n")
endif()

set(_iamf_tools_revision "${IAMF_TOOLS_REVISION}")
if(_iamf_tools_revision STREQUAL "")
  set(_iamf_tools_revision "not supplied")
endif()
set(_ffmpeg_revision "${FFMPEG_REVISION}")
if(_ffmpeg_revision STREQUAL "")
  set(_ffmpeg_revision "not supplied")
endif()
set(_spdlog_version "${SPDLOG_VERSION}")
if(_spdlog_version STREQUAL "")
  set(_spdlog_version "not supplied")
endif()
set(_fmt_version "${SPDLOG_FMT_VERSION}")
if(_fmt_version STREQUAL "")
  set(_fmt_version "not supplied")
endif()
string(CONCAT _notices_text
  "Immersive Audio Muxer third-party build information\n"
  "=============================================\n\n"
  "iamf-tools revision: ${_iamf_tools_revision}\n"
  "FFmpeg revision: ${_ffmpeg_revision}\n"
  "spdlog version: ${_spdlog_version}\n"
  "bundled fmt version: ${_fmt_version}\n"
  "Qt version: ${QT_VERSION}\n\n"
  "The complete license texts included in this package are in the licenses/ directory.\n")
if(NOT "${_missing_notice_text}" STREQUAL "")
  string(APPEND _notices_text "\n${_missing_notice_text}")
endif()
file(WRITE "${_stage_dir}/THIRD_PARTY_NOTICES.txt" "${_notices_text}")
