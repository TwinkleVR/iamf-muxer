cmake_minimum_required(VERSION 3.20)

foreach(_required_arg IN ITEMS STAGE_DIR PROJECT_ROOT VERSION PLATFORM ARCH)
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
  "The license texts included in this package are in the licenses/ directory.\n")
if(NOT "${_missing_notice_text}" STREQUAL "")
  string(APPEND _notices_text "\n${_missing_notice_text}")
endif()
file(WRITE "${_stage_dir}/THIRD_PARTY_NOTICES.txt" "${_notices_text}")
