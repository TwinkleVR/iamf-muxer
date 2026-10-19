cmake_minimum_required(VERSION 3.20)

foreach(_required REPOSITORY REVISION SOURCE_DIR GIT_EXECUTABLE)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "clone_iamf_tools.cmake requires ${_required}")
  endif()
endforeach()

if(EXISTS "${SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse HEAD
    RESULT_VARIABLE _head_result
    OUTPUT_VARIABLE _head
    ERROR_VARIABLE _head_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _head_result EQUAL 0)
    message(FATAL_ERROR "Cannot inspect existing iamf-tools checkout: ${_head_error}")
  endif()
  if(_head STREQUAL "${REVISION}")
    message(STATUS "iamf-tools checkout already matches ${REVISION}")
    return()
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" status --porcelain
    RESULT_VARIABLE _status_result
    OUTPUT_VARIABLE _status
    ERROR_VARIABLE _status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _status_result EQUAL 0 OR NOT _status STREQUAL "")
    message(FATAL_ERROR
      "Existing iamf-tools checkout has local changes or cannot be inspected. "
      "Refusing to change ${SOURCE_DIR} automatically.")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" fetch --no-tags origin "${REVISION}"
    RESULT_VARIABLE _fetch_result
    OUTPUT_VARIABLE _fetch_output
    ERROR_VARIABLE _fetch_error)
  if(NOT _fetch_result EQUAL 0)
    message(FATAL_ERROR "Failed to fetch iamf-tools revision: ${_fetch_error}")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" checkout --detach "${REVISION}"
    RESULT_VARIABLE _checkout_result
    OUTPUT_VARIABLE _checkout_output
    ERROR_VARIABLE _checkout_error)
  if(NOT _checkout_result EQUAL 0)
    message(FATAL_ERROR "Failed to checkout iamf-tools revision: ${_checkout_error}")
  endif()
  return()
endif()

if(EXISTS "${SOURCE_DIR}")
  file(GLOB _existing_entries "${SOURCE_DIR}/*" "${SOURCE_DIR}/.*")
  list(LENGTH _existing_entries _entry_count)
  if(_entry_count GREATER 0)
    message(FATAL_ERROR
      "iamf-tools source directory exists and is not empty: ${SOURCE_DIR}")
  endif()
else()
  get_filename_component(_parent "${SOURCE_DIR}" DIRECTORY)
  file(MAKE_DIRECTORY "${_parent}")
endif()

execute_process(
  COMMAND "${GIT_EXECUTABLE}" clone --no-checkout "${REPOSITORY}" "${SOURCE_DIR}"
  RESULT_VARIABLE _clone_result
  OUTPUT_VARIABLE _clone_output
  ERROR_VARIABLE _clone_error)
if(NOT _clone_result EQUAL 0)
  message(FATAL_ERROR "Failed to clone iamf-tools: ${_clone_error}")
endif()
execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" fetch --no-tags origin "${REVISION}"
  RESULT_VARIABLE _fetch_result
  OUTPUT_VARIABLE _fetch_output
  ERROR_VARIABLE _fetch_error)
if(NOT _fetch_result EQUAL 0)
  message(FATAL_ERROR "Failed to fetch iamf-tools revision: ${_fetch_error}")
endif()
execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" checkout --detach "${REVISION}"
  RESULT_VARIABLE _checkout_result
  OUTPUT_VARIABLE _checkout_output
  ERROR_VARIABLE _checkout_error)
if(NOT _checkout_result EQUAL 0)
  message(FATAL_ERROR "Failed to checkout iamf-tools revision: ${_checkout_error}")
endif()
message(STATUS "Cloned iamf-tools at ${REVISION}")