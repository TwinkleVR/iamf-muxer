cmake_minimum_required(VERSION 3.20)

foreach(_required SOURCE_DIR PATCH_FILE GIT_EXECUTABLE)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR
      "apply_audio_to_tactile_rf64_patch.cmake requires ${_required}")
  endif()
endforeach()

if(NOT EXISTS "${SOURCE_DIR}/.git")
  message(FATAL_ERROR
    "audio-to-tactile checkout is not a Git work tree: ${SOURCE_DIR}")
endif()
if(NOT EXISTS "${PATCH_FILE}")
  message(FATAL_ERROR
    "audio-to-tactile RF64/Wave64 patch does not exist: ${PATCH_FILE}")
endif()

# Normalize only patch targets so the check is stable on Windows checkouts.
file(STRINGS "${PATCH_FILE}" _patch_targets REGEX "^\\+\\+\\+ b/")
foreach(_patch_target IN LISTS _patch_targets)
  string(REGEX REPLACE "^\\+\\+\\+ b/" "" _relative_target
    "${_patch_target}")
  if(_relative_target STREQUAL "/dev/null")
    continue()
  endif()
  set(_target_file "${SOURCE_DIR}/${_relative_target}")
  if(EXISTS "${_target_file}")
    file(READ "${_target_file}" _target_contents)
    string(REPLACE "\r\n" "\n" _normalized_contents
      "${_target_contents}")
    if(NOT _normalized_contents STREQUAL _target_contents)
      file(WRITE "${_target_file}" "${_normalized_contents}")
    endif()
  endif()
endforeach()

execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" apply --check
          --unidiff-zero "${PATCH_FILE}"
  RESULT_VARIABLE _check_result
  ERROR_VARIABLE _check_error)
if(_check_result EQUAL 0)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" apply --unidiff-zero
            "${PATCH_FILE}"
    RESULT_VARIABLE _apply_result
    ERROR_VARIABLE _apply_error)
  if(NOT _apply_result EQUAL 0)
    message(FATAL_ERROR
      "Failed to apply audio-to-tactile RF64 patch: ${_apply_error}")
  endif()
  message(STATUS "Applied audio-to-tactile RF64/Wave64 patch")
  return()
endif()

execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" apply --reverse --check
          --unidiff-zero "${PATCH_FILE}"
  RESULT_VARIABLE _reverse_result
  ERROR_VARIABLE _reverse_error)
if(_reverse_result EQUAL 0)
  message(STATUS "audio-to-tactile RF64/Wave64 patch is already applied")
  return()
endif()

message(FATAL_ERROR
  "audio-to-tactile RF64/Wave64 patch does not apply cleanly and is not already applied.\n"
  "Patch: ${PATCH_FILE}\n"
  "Forward check: ${_check_error}\n"
  "Reverse check: ${_reverse_error}")
