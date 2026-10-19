cmake_minimum_required(VERSION 3.20)

foreach(_required SOURCE_DIR PATCH_FILES GIT_EXECUTABLE)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "apply_iamf_tools_patches.cmake requires ${_required}")
  endif()
endforeach()

if(NOT EXISTS "${SOURCE_DIR}/.git")
  message(FATAL_ERROR "iamf-tools checkout is not a Git work tree: ${SOURCE_DIR}")
endif()

string(REPLACE "|" ";" _patch_files "${PATCH_FILES}")
foreach(_patch_file IN LISTS _patch_files)
  if(NOT EXISTS "${_patch_file}")
    message(FATAL_ERROR "iamf-tools patch does not exist: ${_patch_file}")
  endif()

  # Git on Windows may check out the target files as CRLF while the
  # maintained patch is stored as LF. Normalize only the files named by this
  # patch; this preserves their content and makes forward/reverse checks
  # deterministic across Git configurations.
  file(STRINGS "${_patch_file}" _patch_targets REGEX "^\\+\\+\\+ b/")
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
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" apply --check --unidiff-zero "${_patch_file}"
    RESULT_VARIABLE _check_result
    ERROR_VARIABLE _check_error)
  if(_check_result EQUAL 0)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" apply --unidiff-zero "${_patch_file}"
      RESULT_VARIABLE _apply_result
      ERROR_VARIABLE _apply_error)
    if(NOT _apply_result EQUAL 0)
      message(FATAL_ERROR "Failed to apply iamf-tools patch ${_patch_file}: ${_apply_error}")
    endif()
    message(STATUS "Applied iamf-tools patch: ${_patch_file}")
    continue()
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" apply --reverse --check --unidiff-zero "${_patch_file}"
    RESULT_VARIABLE _reverse_result
    ERROR_VARIABLE _reverse_error)
  if(_reverse_result EQUAL 0)
    message(STATUS "iamf-tools patch is already applied: ${_patch_file}")
    continue()
  endif()

  message(FATAL_ERROR
    "iamf-tools patch does not apply cleanly and is not already applied.\n"
    "Patch: ${_patch_file}\n"
    "Forward check: ${_check_error}\n"
    "Reverse check: ${_reverse_error}")
endforeach()
