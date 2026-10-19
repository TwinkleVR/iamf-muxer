cmake_minimum_required(VERSION 3.22)

# Keep native Apple Silicon builds runnable on the project's supported macOS
# baseline. These are defaults; callers may still override either cache value.
if(CMAKE_HOST_APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING
      "Minimum supported macOS version")
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING
      "macOS architectures to build")
endif()

project(immersive_audio_muxer VERSION 0.9.0 LANGUAGES CXX)

# MinGW's archiver can fail when Ninja runs multiple archive steps at once.
# Keep link/archive steps serial while allowing compilation to remain parallel.
if(MINGW AND CMAKE_GENERATOR MATCHES "Ninja")
  set(CMAKE_JOB_POOLS "iamf_mp4_link=1")
  set(CMAKE_JOB_POOL_LINK "iamf_mp4_link")
endif()

include(GNUInstallDirs)
include(FetchContent)

set(IMMERSIVE_AUDIO_MUXER_SPDLOG_VERSION "1.17.0")
set(IMMERSIVE_AUDIO_MUXER_SPDLOG_FMT_VERSION "12.1.0")
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "Build spdlog as a static library" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "Disable spdlog tests" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "Disable spdlog examples" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "Disable spdlog benchmarks" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "Disable spdlog install rules" FORCE)
if(WIN32)
  set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "Use Unicode Windows log paths" FORCE)
endif()
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG "v${IMMERSIVE_AUDIO_MUXER_SPDLOG_VERSION}"
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(spdlog)

option(IMMERSIVE_AUDIO_MUXER_BUILD_TESTS "Build focused box writer tests" ON)
option(IMMERSIVE_AUDIO_MUXER_BUILD_GUI "Build the Qt Quick desktop GUI" OFF)
option(IMMERSIVE_AUDIO_MUXER_BUILD_RELEASE_BUNDLE
       "Enable release metadata and install rules used by packaging scripts" OFF)
option(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_FETCH_OFFICIAL "Clone, patch, and build the pinned official iamf-tools encoder" ON)
option(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_PATCH_EXTERNAL_SOURCE "Apply the maintained patch to an explicitly supplied source checkout" OFF)

set(IMMERSIVE_AUDIO_MUXER_RELEASE_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Version string written into release packages")
set(IMMERSIVE_AUDIO_MUXER_RELEASE_CHANNEL "development" CACHE STRING
    "Release channel written into build metadata")
set(IMMERSIVE_AUDIO_MUXER_FFMPEG_PREFIX "" CACHE PATH "FFmpeg installation prefix")
set(IMMERSIVE_AUDIO_MUXER_FFMPEG_LICENSE_FILE "" CACHE FILEPATH
    "Optional FFmpeg license or notices file copied into release packages")
set(IMMERSIVE_AUDIO_MUXER_GIT_EXECUTABLE "" CACHE FILEPATH
    "Git executable used to clone and patch official iamf-tools")
set(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_GIT_REPOSITORY "https://github.com/AOMediaCodec/iamf-tools.git" CACHE STRING "Official iamf-tools repository")
set(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_GIT_TAG
    "5ad3728679495d0037c9be7334338f2859d2a3fa" CACHE STRING
    "Pinned official iamf-tools revision")
set(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_SOURCE_DIR "" CACHE PATH "Existing official iamf-tools checkout; skips clone/update")
set(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_CMAKE_PATCH
    "${CMAKE_SOURCE_DIR}/cmake/patches/iamf-tools-cmake.patch"
    CACHE FILEPATH "CMake patch applied to the pinned official iamf-tools checkout")
set(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_INPROCESS_PATCH
    "${CMAKE_SOURCE_DIR}/cmake/patches/iamf-tools-inprocess.patch"
    CACHE FILEPATH "In-process encoder patch applied after the CMake patch")
set(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_COMPAT_PATCH
    "${CMAKE_SOURCE_DIR}/cmake/patches/iamf-tools-compat.patch"
    CACHE FILEPATH "Compiler compatibility patch for official iamf-tools")
set(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_CMAKE_BUILD_DIR "" CACHE PATH
    "Separate CMake build directory for official iamf-tools")
set(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_BUILD_CONFIG "" CACHE STRING
    "Configuration used to build official iamf-tools for multi-config generators")
if(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_FETCH_OFFICIAL)
  if(IMMERSIVE_AUDIO_MUXER_GIT_EXECUTABLE)
    set(GIT_EXECUTABLE "${IMMERSIVE_AUDIO_MUXER_GIT_EXECUTABLE}" CACHE FILEPATH
        "Git executable used to clone and patch official iamf-tools" FORCE)
  elseif(WIN32)
    # Prefer Git for Windows over an MSYS Git picked up from a MinGW shell.
    # The latter can fail to start under some Windows permission policies
    # before ExternalProject gets a chance to clone or patch the checkout.
    set(_git_for_windows_hints)
    foreach(_git_install_root IN ITEMS
        "$ENV{ProgramFiles}"
        "$ENV{ProgramFiles\(x86\)}")
      if(_git_install_root)
        list(APPEND _git_for_windows_hints
          "${_git_install_root}/Git/cmd"
          "${_git_install_root}/Git/bin"
          "${_git_install_root}/Git/mingw64/bin")
      endif()
    endforeach()
    find_program(_git_for_windows_executable
      NAMES git.exe
      HINTS ${_git_for_windows_hints}
      NO_DEFAULT_PATH)
    if(_git_for_windows_executable)
      set(GIT_EXECUTABLE "${_git_for_windows_executable}" CACHE FILEPATH
          "Git executable used to clone and patch official iamf-tools" FORCE)
      message(STATUS "Using Git for Windows: ${GIT_EXECUTABLE}")
    else()
      find_package(Git REQUIRED)
    endif()
  else()
    find_package(Git REQUIRED)
  endif()
  include(ExternalProject)

  set(_iamf_tools_source_dir "${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_SOURCE_DIR}")
  set(_iamf_tools_managed_source FALSE)
  if(NOT _iamf_tools_source_dir)
    set(_iamf_tools_source_dir
        "${CMAKE_BINARY_DIR}/_deps/official_iamf_tools-src")
    set(_iamf_tools_managed_source TRUE)
  else()
    get_filename_component(_iamf_tools_source_dir
      "${_iamf_tools_source_dir}" ABSOLUTE
      BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(NOT IS_DIRECTORY "${_iamf_tools_source_dir}")
      message(FATAL_ERROR
        "IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_SOURCE_DIR is not a directory: "
        "${_iamf_tools_source_dir}")
    endif()
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${_iamf_tools_source_dir}" rev-parse HEAD
      RESULT_VARIABLE _iamf_tools_revision_result
      OUTPUT_VARIABLE _iamf_tools_revision
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _iamf_tools_revision_result EQUAL 0)
      message(FATAL_ERROR
        "The supplied iamf-tools source is not a Git checkout: "
        "${_iamf_tools_source_dir}")
    endif()
    if(NOT _iamf_tools_revision STREQUAL "${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_GIT_TAG}")
      message(FATAL_ERROR
        "The supplied iamf-tools checkout is at ${_iamf_tools_revision}, "
        "but this muxer requires ${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_GIT_TAG}."
        " Use a clean checkout of the pinned revision.")
    endif()
  endif()

  if(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_CMAKE_BUILD_DIR)
    get_filename_component(_iamf_tools_binary_dir
      "${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_CMAKE_BUILD_DIR}" ABSOLUTE
      BASE_DIR "${CMAKE_BINARY_DIR}")
  else()
    set(_iamf_tools_binary_dir
        "${CMAKE_BINARY_DIR}/_deps/official_iamf_tools-build")
  endif()

  if(CMAKE_CONFIGURATION_TYPES)
    if(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_BUILD_CONFIG)
      set(_iamf_tools_build_config
          "${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_BUILD_CONFIG}")
    else()
      set(_iamf_tools_build_config "Release")
    endif()
    list(FIND CMAKE_CONFIGURATION_TYPES "${_iamf_tools_build_config}"
         _iamf_tools_config_index)
    if(_iamf_tools_config_index LESS 0)
      message(FATAL_ERROR
        "IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_BUILD_CONFIG must be one of: "
        "${CMAKE_CONFIGURATION_TYPES}")
    endif()
    set(_iamf_tools_output_dir
        "${_iamf_tools_binary_dir}/bin/${_iamf_tools_build_config}")
    set(_iamf_tools_archive_dir
        "${_iamf_tools_binary_dir}/lib/${_iamf_tools_build_config}")
  else()
    if(CMAKE_BUILD_TYPE)
      set(_iamf_tools_build_config "${CMAKE_BUILD_TYPE}")
    else()
      set(_iamf_tools_build_config "Release")
    endif()
    set(_iamf_tools_output_dir "${_iamf_tools_binary_dir}/bin")
    set(_iamf_tools_archive_dir "${_iamf_tools_binary_dir}/lib")
  endif()

  set(_iamf_tools_clone_script
      "${CMAKE_SOURCE_DIR}/cmake/clone_iamf_tools.cmake")
  set(_iamf_tools_patches_script
      "${CMAKE_SOURCE_DIR}/cmake/apply_iamf_tools_patches.cmake")
  if(NOT EXISTS "${_iamf_tools_clone_script}" OR
     NOT EXISTS "${_iamf_tools_patches_script}")
    message(FATAL_ERROR "iamf-tools CMake helper scripts are missing")
  endif()
  if(NOT EXISTS "${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_CMAKE_PATCH}")
    message(FATAL_ERROR
      "iamf-tools CMake patch does not exist: ${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_CMAKE_PATCH}")
  endif()
  if(NOT EXISTS "${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_INPROCESS_PATCH}")
    message(FATAL_ERROR
      "iamf-tools in-process patch does not exist: ${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_INPROCESS_PATCH}")
  endif()
  if(NOT EXISTS "${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_COMPAT_PATCH}")
    message(FATAL_ERROR
      "iamf-tools compatibility patch does not exist: ${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_COMPAT_PATCH}")
  endif()
  set(_iamf_tools_download_command "")
  if(_iamf_tools_managed_source)
    set(_iamf_tools_download_command
      "${CMAKE_COMMAND}"
      "-DREPOSITORY=${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_GIT_REPOSITORY}"
      "-DREVISION=${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_GIT_TAG}"
      "-DSOURCE_DIR=${_iamf_tools_source_dir}"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
      -P "${_iamf_tools_clone_script}")
  endif()

  set(_iamf_tools_configure_args
    "-DIAMF_TOOLS_BUILD_SHARED=OFF"
    "-DIAMF_TOOLS_BUILD_CLI=OFF"
    "-DIAMF_TOOLS_BUILD_INPROCESS_ENCODER=ON"
    "-DIMMERSIVE_AUDIO_MUXER_API_INCLUDE_DIR=${CMAKE_SOURCE_DIR}/include")
  if(MINGW AND CMAKE_GENERATOR MATCHES "Ninja")
    list(APPEND _iamf_tools_configure_args
      "-DCMAKE_JOB_POOLS=iamf_mp4_link=1"
      "-DCMAKE_JOB_POOL_LINK=iamf_mp4_link")
  endif()
  if(NOT CMAKE_CONFIGURATION_TYPES)
    list(APPEND _iamf_tools_configure_args
      "-DCMAKE_BUILD_TYPE=${_iamf_tools_build_config}")
  endif()
  if(APPLE)
    list(APPEND _iamf_tools_configure_args
      "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
      "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
    if(CMAKE_OSX_SYSROOT)
      list(APPEND _iamf_tools_configure_args
        "-DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}")
    endif()
  endif()
  if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND _iamf_tools_configure_args
      "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
  endif()

  set(_iamf_tools_patch_command "")
  if(_iamf_tools_managed_source OR
     IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_PATCH_EXTERNAL_SOURCE)
    set(_iamf_tools_patch_command
      "${CMAKE_COMMAND}"
      "-DSOURCE_DIR=${_iamf_tools_source_dir}"
      "-DPATCH_FILES=${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_CMAKE_PATCH}|${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_INPROCESS_PATCH}|${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_COMPAT_PATCH}"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
      -P "${_iamf_tools_patches_script}")
  endif()

  if(WIN32)
    set(_iamf_tools_encoder_runtime
        "${_iamf_tools_output_dir}/iamf_tools_encoder_inprocess.dll")
    if(MINGW)
      set(_iamf_tools_encoder_implib
          "${_iamf_tools_archive_dir}/iamf_tools_encoder_inprocess.dll.a")
    else()
      set(_iamf_tools_encoder_implib
          "${_iamf_tools_archive_dir}/iamf_tools_encoder_inprocess.lib")
    endif()
  else()
    set(_iamf_tools_encoder_runtime
        "${_iamf_tools_output_dir}/libiamf_tools_encoder_inprocess${CMAKE_SHARED_LIBRARY_SUFFIX}")
  endif()
  set(_iamf_tools_encoder_byproducts "${_iamf_tools_encoder_runtime}")
  if(WIN32)
    list(APPEND _iamf_tools_encoder_byproducts "${_iamf_tools_encoder_implib}")
  endif()

  ExternalProject_Add(official_iamf_tools
    SOURCE_DIR "${_iamf_tools_source_dir}"
    BINARY_DIR "${_iamf_tools_binary_dir}"
    DOWNLOAD_COMMAND ${_iamf_tools_download_command}
    UPDATE_COMMAND ""
    PATCH_COMMAND ${_iamf_tools_patch_command}
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -S <SOURCE_DIR> -B <BINARY_DIR>
                      -G "${CMAKE_GENERATOR}"
                      ${_iamf_tools_configure_args}
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR>
                  --target iamf_tools_encoder_inprocess
                  --config "${_iamf_tools_build_config}"
    BUILD_BYPRODUCTS ${_iamf_tools_encoder_byproducts}
    INSTALL_COMMAND ""
    BUILD_IN_SOURCE FALSE)

  add_library(iamf_tools_encoder_inprocess SHARED IMPORTED GLOBAL)
  set_target_properties(iamf_tools_encoder_inprocess PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_iamf_tools_source_dir}")
  if(CMAKE_CONFIGURATION_TYPES)
    foreach(_iamf_tools_consumer_config IN LISTS CMAKE_CONFIGURATION_TYPES)
      string(TOUPPER "${_iamf_tools_consumer_config}"
             _iamf_tools_consumer_config_upper)
      set_property(TARGET iamf_tools_encoder_inprocess PROPERTY
        "IMPORTED_LOCATION_${_iamf_tools_consumer_config_upper}"
        "${_iamf_tools_encoder_runtime}")
      if(WIN32)
        set_property(TARGET iamf_tools_encoder_inprocess PROPERTY
          "IMPORTED_IMPLIB_${_iamf_tools_consumer_config_upper}"
          "${_iamf_tools_encoder_implib}")
      endif()
    endforeach()
  else()
    set_target_properties(iamf_tools_encoder_inprocess PROPERTIES
      IMPORTED_LOCATION "${_iamf_tools_encoder_runtime}")
    if(WIN32)
      set_target_properties(iamf_tools_encoder_inprocess PROPERTIES
        IMPORTED_IMPLIB "${_iamf_tools_encoder_implib}")
    endif()
  endif()
  add_dependencies(iamf_tools_encoder_inprocess official_iamf_tools)
  if(NOT EXISTS "${_iamf_tools_source_dir}/CMakeLists.txt" AND
     NOT _iamf_tools_managed_source AND
     NOT IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_PATCH_EXTERNAL_SOURCE)
    message(FATAL_ERROR
      "The supplied iamf-tools source has no CMakeLists.txt. Either prepare it "
      "first or enable IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_PATCH_EXTERNAL_SOURCE.")
  endif()
  message(STATUS "Official iamf-tools source: ${_iamf_tools_source_dir}")
  message(STATUS "Official iamf-tools revision: ${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_GIT_TAG}")
  message(STATUS "Official iamf-tools CMake build: ${_iamf_tools_binary_dir}")
endif()

if(APPLE AND TARGET iamf_tools_encoder_inprocess)
  # CTest runs before packaging copies this external-project dylib into the app bundle.
  list(APPEND CMAKE_BUILD_RPATH "${_iamf_tools_output_dir}")
endif()

if(IMMERSIVE_AUDIO_MUXER_FFMPEG_PREFIX)
  # The explicit FFmpeg prefix must win over CMAKE_PREFIX_PATH, which may also
  # contain Qt and its own FFmpeg libraries.
  set(_ffmpeg_include_search_args PATHS "${IMMERSIVE_AUDIO_MUXER_FFMPEG_PREFIX}/include" NO_DEFAULT_PATH)
  set(_ffmpeg_library_search_args PATHS "${IMMERSIVE_AUDIO_MUXER_FFMPEG_PREFIX}/lib" NO_DEFAULT_PATH)
else()
  set(_ffmpeg_include_search_args HINTS /usr/local/include)
  set(_ffmpeg_library_search_args HINTS /usr/local/lib)
endif()
find_path(IMMERSIVE_AUDIO_MUXER_FFMPEG_INCLUDE_DIR NAMES libavformat/avformat.h
  ${_ffmpeg_include_search_args})
find_library(IMMERSIVE_AUDIO_MUXER_AVFORMAT_LIBRARY NAMES avformat libavformat
  ${_ffmpeg_library_search_args})
find_library(IMMERSIVE_AUDIO_MUXER_AVCODEC_LIBRARY NAMES avcodec libavcodec
  ${_ffmpeg_library_search_args})
find_library(IMMERSIVE_AUDIO_MUXER_AVUTIL_LIBRARY NAMES avutil libavutil
  ${_ffmpeg_library_search_args})
find_path(IMMERSIVE_AUDIO_MUXER_SWRESAMPLE_INCLUDE_DIR NAMES libswresample/swresample.h
  ${_ffmpeg_include_search_args})
find_library(IMMERSIVE_AUDIO_MUXER_SWRESAMPLE_LIBRARY NAMES swresample libswresample
  ${_ffmpeg_library_search_args})
if(NOT IMMERSIVE_AUDIO_MUXER_FFMPEG_INCLUDE_DIR OR NOT IMMERSIVE_AUDIO_MUXER_AVFORMAT_LIBRARY OR
   NOT IMMERSIVE_AUDIO_MUXER_AVCODEC_LIBRARY OR NOT IMMERSIVE_AUDIO_MUXER_AVUTIL_LIBRARY OR
   NOT IMMERSIVE_AUDIO_MUXER_SWRESAMPLE_INCLUDE_DIR OR NOT IMMERSIVE_AUDIO_MUXER_SWRESAMPLE_LIBRARY)
  message(FATAL_ERROR
    "FFmpeg libraries and headers are required; set IMMERSIVE_AUDIO_MUXER_FFMPEG_PREFIX")
endif()
add_library(immersive_audio_muxer_core STATIC
  src/atomic_output_transaction.cpp
  src/avformat_input.cpp
  src/container_normalization_service.cpp
  src/ffmpeg_interleaved_muxer.cpp
  src/ffmpeg_iamf_track_adapter.cpp
  src/in_process_iamf_encoder.cpp
  src/mux_job.cpp
  src/runtime_logging.cpp
  src/standalone_iamf_reader.cpp
  src/iamf_box_writer.cpp
  src/iamf_mp4_file_writer.cpp
  src/mp4_iamf_audio_tracks.cpp
  src/video_input.cpp
  src/video_spatial_metadata.cpp)
set_target_properties(immersive_audio_muxer_core PROPERTIES
  OUTPUT_NAME "iamf_muxer_core")
target_compile_features(immersive_audio_muxer_core PUBLIC cxx_std_17)
target_include_directories(immersive_audio_muxer_core PUBLIC include)

target_compile_definitions(immersive_audio_muxer_core PUBLIC
  IMMERSIVE_AUDIO_MUXER_HAVE_VIDEO)
target_include_directories(immersive_audio_muxer_core PRIVATE
  "${IMMERSIVE_AUDIO_MUXER_FFMPEG_INCLUDE_DIR}")
target_link_libraries(immersive_audio_muxer_core PUBLIC
  spdlog::spdlog
  "${IMMERSIVE_AUDIO_MUXER_AVFORMAT_LIBRARY}"
  "${IMMERSIVE_AUDIO_MUXER_AVCODEC_LIBRARY}"
  "${IMMERSIVE_AUDIO_MUXER_AVUTIL_LIBRARY}")
if(TARGET iamf_tools_encoder_inprocess)
  target_compile_definitions(immersive_audio_muxer_core PRIVATE
    IMMERSIVE_AUDIO_MUXER_HAVE_INPROCESS_IAMF_TOOLS)
  target_link_libraries(immersive_audio_muxer_core PUBLIC iamf_tools_encoder_inprocess)
endif()
if(WIN32)
  target_compile_definitions(immersive_audio_muxer_core PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_link_libraries(immersive_audio_muxer_core PRIVATE ${CMAKE_DL_LIBS})
endif()

add_library(immersive_audio_muxer_app STATIC
  src/app/project_types.cpp
  src/app/media_probe_service.cpp
  src/app/audio_preparation_service.cpp
  src/app/metadata_builder.cpp)
set_target_properties(immersive_audio_muxer_app PROPERTIES
  OUTPUT_NAME "iamf_muxer_app")
target_compile_features(immersive_audio_muxer_app PUBLIC cxx_std_17)
target_include_directories(immersive_audio_muxer_app PUBLIC include)
target_include_directories(immersive_audio_muxer_app PRIVATE
  "${IMMERSIVE_AUDIO_MUXER_FFMPEG_INCLUDE_DIR}"
  "${IMMERSIVE_AUDIO_MUXER_SWRESAMPLE_INCLUDE_DIR}")
target_link_libraries(immersive_audio_muxer_app PUBLIC immersive_audio_muxer_core)
target_link_libraries(immersive_audio_muxer_app PRIVATE "${IMMERSIVE_AUDIO_MUXER_SWRESAMPLE_LIBRARY}")
if(IMMERSIVE_AUDIO_MUXER_BUILD_GUI)
  if(NOT TARGET iamf_tools_encoder_inprocess)
    message(FATAL_ERROR
      "IMMERSIVE_AUDIO_MUXER_BUILD_GUI requires the in-process official iamf-tools encoder. "
      "Enable IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_FETCH_OFFICIAL or provide a patched checkout.")
  endif()
  set(_iamf_tools_display_revision "${IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_GIT_TAG}")
  string(LENGTH "${_iamf_tools_display_revision}" _iamf_tools_revision_length)
  if(_iamf_tools_revision_length GREATER 8 AND
     _iamf_tools_display_revision MATCHES "^[0-9A-Fa-f]+$")
    string(SUBSTRING "${_iamf_tools_display_revision}" 0 8
           _iamf_tools_display_revision)
  endif()
  set(IMMERSIVE_AUDIO_MUXER_GENERATED_INCLUDE_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/generated")
  file(MAKE_DIRECTORY "${IMMERSIVE_AUDIO_MUXER_GENERATED_INCLUDE_DIR}")
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/iamf_tools_version.h.in"
    "${IMMERSIVE_AUDIO_MUXER_GENERATED_INCLUDE_DIR}/iamf_tools_version.h"
    @ONLY)
  find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Multimedia Qml Quick QuickControls2 QuickDialogs2 Svg)
  qt_standard_project_setup()
  qt_policy(SET QTP0001 NEW)
  qt_policy(SET QTP0004 NEW)
  set(CMAKE_AUTOMOC ON)
  set(_immersive_audio_muxer_gui_target_options)
  if(APPLE)
    list(APPEND _immersive_audio_muxer_gui_target_options MACOSX_BUNDLE)
  endif()
  qt_add_executable(immersive_audio_muxer_gui ${_immersive_audio_muxer_gui_target_options}
    include/gui_controller.h
    include/gui_input_model.h
    src/gui_main.cpp
    src/gui_controller.cpp
    src/gui_input_model.cpp)
  set_target_properties(immersive_audio_muxer_gui PROPERTIES
    OUTPUT_NAME "iamf_muxer")
  if(WIN32)
    set_target_properties(immersive_audio_muxer_gui PROPERTIES
      WIN32_EXECUTABLE TRUE)
    target_sources(immersive_audio_muxer_gui PRIVATE src/iamf_muxer.rc)
  endif()
  if(APPLE)
    set(_immersive_audio_muxer_macos_icon
        "${CMAKE_SOURCE_DIR}/qml/assets/iamf_muxer.icns")
    set_source_files_properties("${_immersive_audio_muxer_macos_icon}"
      PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(immersive_audio_muxer_gui PRIVATE
      "${_immersive_audio_muxer_macos_icon}")
    set_target_properties(immersive_audio_muxer_gui PROPERTIES
      MACOSX_BUNDLE_ICON_FILE "iamf_muxer.icns"
      MACOSX_BUNDLE_BUNDLE_NAME "iamf_muxer"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.twinkle.iamf_muxer"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${IMMERSIVE_AUDIO_MUXER_RELEASE_VERSION}"
      MACOSX_BUNDLE_BUNDLE_VERSION "${IMMERSIVE_AUDIO_MUXER_RELEASE_VERSION}")
  endif()
  qt_add_qml_module(immersive_audio_muxer_gui
    URI ImmersiveAudioMuxer.Gui
    VERSION 1.0
    QML_FILES qml/Main.qml
    RESOURCES qml/assets/iamf_logo_color.png qml/assets/iamf_logo_symbol.svg qml/assets/iamf_muxer_app_icon.svg qml/assets/iamf_muxer_macos_dock_icon.svg qml/assets/iamf_logo_white_symbol.svg qml/assets/ambisonics_symbol.svg qml/assets/stereo_symbol.svg qml/assets/speaker_layout_symbol.svg qml/assets/input_error.wav)
  target_compile_features(immersive_audio_muxer_gui PRIVATE cxx_std_17)
  target_include_directories(immersive_audio_muxer_gui PRIVATE
    "${IMMERSIVE_AUDIO_MUXER_GENERATED_INCLUDE_DIR}"
    "${IMMERSIVE_AUDIO_MUXER_FFMPEG_INCLUDE_DIR}")
  target_link_libraries(immersive_audio_muxer_gui PRIVATE
    immersive_audio_muxer_app
    Qt6::Core
    Qt6::Gui
    Qt6::Multimedia
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
    Qt6::Svg)
  target_compile_definitions(immersive_audio_muxer_gui PRIVATE
    IMMERSIVE_AUDIO_MUXER_VERSION="${IMMERSIVE_AUDIO_MUXER_RELEASE_VERSION}"
    IMMERSIVE_AUDIO_MUXER_SPDLOG_VERSION="${IMMERSIVE_AUDIO_MUXER_SPDLOG_VERSION}")
  if(WIN32)
    target_link_libraries(immersive_audio_muxer_gui PRIVATE shell32 ole32)
  endif()
endif()

if(WIN32 AND TARGET immersive_audio_muxer_gui)
  if(NOT IMMERSIVE_AUDIO_MUXER_FFMPEG_PREFIX OR
     NOT IS_DIRECTORY "${IMMERSIVE_AUDIO_MUXER_FFMPEG_PREFIX}/bin")
    message(FATAL_ERROR
      "The Windows GUI requires the patched FFmpeg runtime; set IMMERSIVE_AUDIO_MUXER_FFMPEG_PREFIX to its installation prefix")
  endif()

  set(_immersive_audio_muxer_ffmpeg_runtime_dlls)
  foreach(_ffmpeg_library IN ITEMS avformat avcodec avutil swresample)
    file(GLOB _ffmpeg_runtime_dlls CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
      "${IMMERSIVE_AUDIO_MUXER_FFMPEG_PREFIX}/bin/${_ffmpeg_library}-*.dll")
    if(NOT _ffmpeg_runtime_dlls)
      message(FATAL_ERROR
        "The FFmpeg runtime DLL for ${_ffmpeg_library} was not found under ${IMMERSIVE_AUDIO_MUXER_FFMPEG_PREFIX}/bin")
    endif()
    list(APPEND _immersive_audio_muxer_ffmpeg_runtime_dlls ${_ffmpeg_runtime_dlls})
  endforeach()

  add_custom_command(TARGET immersive_audio_muxer_gui POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      ${_immersive_audio_muxer_ffmpeg_runtime_dlls}
      "$<TARGET_FILE_DIR:immersive_audio_muxer_gui>"
    COMMAND_EXPAND_LISTS
    VERBATIM)
endif()

if(TARGET iamf_tools_encoder_inprocess AND TARGET immersive_audio_muxer_gui)
  add_dependencies(immersive_audio_muxer_gui official_iamf_tools)
  if(WIN32)
    add_custom_command(TARGET immersive_audio_muxer_gui POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_iamf_tools_encoder_runtime}"
        "$<TARGET_FILE_DIR:immersive_audio_muxer_gui>"
      VERBATIM)
  endif()
endif()

install(TARGETS immersive_audio_muxer_core immersive_audio_muxer_app
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
if(TARGET iamf_tools_encoder_inprocess)
  install(FILES "${_iamf_tools_encoder_runtime}"
    DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
if(IMMERSIVE_AUDIO_MUXER_BUILD_GUI)
  if(APPLE)
    install(TARGETS immersive_audio_muxer_gui
      BUNDLE DESTINATION ".")
  else()
    install(TARGETS immersive_audio_muxer_gui
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endif()
endif()
if(IMMERSIVE_AUDIO_MUXER_BUILD_RELEASE_BUNDLE)
  set(IMMERSIVE_AUDIO_MUXER_BUILD_INFO_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/iamf_muxer_build_info.txt")
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/release_build_info.txt.in"
    "${IMMERSIVE_AUDIO_MUXER_BUILD_INFO_FILE}"
    @ONLY)
  install(FILES "${IMMERSIVE_AUDIO_MUXER_BUILD_INFO_FILE}"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/iamf_muxer")
  if(IMMERSIVE_AUDIO_MUXER_FFMPEG_LICENSE_FILE AND
     EXISTS "${IMMERSIVE_AUDIO_MUXER_FFMPEG_LICENSE_FILE}")
    install(FILES "${IMMERSIVE_AUDIO_MUXER_FFMPEG_LICENSE_FILE}"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/iamf_muxer/licenses"
      RENAME "FFmpeg-LICENSE.txt")
  endif()
  if(IMMERSIVE_AUDIO_MUXER_IAMF_TOOLS_FETCH_OFFICIAL AND
     DEFINED _iamf_tools_source_dir AND
     EXISTS "${_iamf_tools_source_dir}/LICENSE")
    install(FILES "${_iamf_tools_source_dir}/LICENSE"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/iamf_muxer/licenses"
      RENAME "iamf-tools-LICENSE.txt")
  endif()
  if(EXISTS "${spdlog_SOURCE_DIR}/LICENSE")
    install(FILES "${spdlog_SOURCE_DIR}/LICENSE"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/iamf_muxer/licenses"
      RENAME "spdlog-LICENSE.txt")
  endif()
  if(EXISTS "${spdlog_SOURCE_DIR}/include/spdlog/fmt/bundled/fmt.license.rst")
    install(FILES "${spdlog_SOURCE_DIR}/include/spdlog/fmt/bundled/fmt.license.rst"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/iamf_muxer/licenses"
      RENAME "fmt-LICENSE.txt")
  endif()
endif()

if(IMMERSIVE_AUDIO_MUXER_BUILD_TESTS)
  enable_testing()
  add_executable(immersive_audio_muxer_box_writer_test
    tests/box_writer_self_test.cpp)
  set_target_properties(immersive_audio_muxer_box_writer_test PROPERTIES
    OUTPUT_NAME "iamf_muxer_box_writer_test")
  target_link_libraries(immersive_audio_muxer_box_writer_test PRIVATE
    immersive_audio_muxer_core)
  target_compile_features(immersive_audio_muxer_box_writer_test PRIVATE
    cxx_std_17)
  add_test(NAME immersive_audio_muxer_box_writer_test
           COMMAND immersive_audio_muxer_box_writer_test)
  add_executable(immersive_audio_muxer_project_model_test
    tests/app_project_model_self_test.cpp)
  set_target_properties(immersive_audio_muxer_project_model_test PROPERTIES
    OUTPUT_NAME "iamf_muxer_project_model_test")
  target_link_libraries(immersive_audio_muxer_project_model_test PRIVATE
    immersive_audio_muxer_app)
  target_compile_features(immersive_audio_muxer_project_model_test PRIVATE cxx_std_17)
  add_test(NAME immersive_audio_muxer_project_model_test
           COMMAND immersive_audio_muxer_project_model_test)
  add_executable(immersive_audio_muxer_atomic_output_transaction_test
    tests/atomic_output_transaction_self_test.cpp)
  set_target_properties(immersive_audio_muxer_atomic_output_transaction_test PROPERTIES
    OUTPUT_NAME "iamf_muxer_atomic_output_transaction_test")
  target_link_libraries(immersive_audio_muxer_atomic_output_transaction_test PRIVATE
    immersive_audio_muxer_core)
  target_compile_features(immersive_audio_muxer_atomic_output_transaction_test PRIVATE cxx_std_17)
  add_test(NAME immersive_audio_muxer_atomic_output_transaction_test
           COMMAND immersive_audio_muxer_atomic_output_transaction_test)
  add_executable(immersive_audio_muxer_container_normalization_test
    tests/container_normalization_self_test.cpp)
  set_target_properties(immersive_audio_muxer_container_normalization_test PROPERTIES
    OUTPUT_NAME "iamf_muxer_container_normalization_test")
  target_include_directories(immersive_audio_muxer_container_normalization_test PRIVATE
    "${IMMERSIVE_AUDIO_MUXER_FFMPEG_INCLUDE_DIR}")
  target_link_libraries(immersive_audio_muxer_container_normalization_test PRIVATE
    immersive_audio_muxer_core)
  target_compile_features(immersive_audio_muxer_container_normalization_test PRIVATE cxx_std_17)
  add_test(NAME immersive_audio_muxer_container_normalization_test
           COMMAND immersive_audio_muxer_container_normalization_test
             "${CMAKE_SOURCE_DIR}/tests/data/container_normalization_subrip.mkv")

  add_executable(immersive_audio_muxer_container_normalization_video_test
    tests/container_normalization_video_self_test.cpp)
  set_target_properties(immersive_audio_muxer_container_normalization_video_test PROPERTIES
    OUTPUT_NAME "iamf_muxer_container_normalization_video_test")
  target_include_directories(immersive_audio_muxer_container_normalization_video_test PRIVATE
    "${IMMERSIVE_AUDIO_MUXER_FFMPEG_INCLUDE_DIR}")
  target_link_libraries(immersive_audio_muxer_container_normalization_video_test PRIVATE
    immersive_audio_muxer_core)
  target_compile_features(immersive_audio_muxer_container_normalization_video_test PRIVATE cxx_std_17)
  add_test(NAME immersive_audio_muxer_container_normalization_video_test
           COMMAND immersive_audio_muxer_container_normalization_video_test
             "${CMAKE_SOURCE_DIR}/tests/data/container_normalization_subrip.mkv")

  add_executable(immersive_audio_muxer_ffmpeg_component_availability_test
    tests/ffmpeg_component_availability_self_test.cpp)
  set_target_properties(immersive_audio_muxer_ffmpeg_component_availability_test PROPERTIES
    OUTPUT_NAME "iamf_muxer_ffmpeg_component_availability_test")
  target_include_directories(immersive_audio_muxer_ffmpeg_component_availability_test PRIVATE
    "${IMMERSIVE_AUDIO_MUXER_FFMPEG_INCLUDE_DIR}")
  target_link_libraries(immersive_audio_muxer_ffmpeg_component_availability_test PRIVATE
    immersive_audio_muxer_core)
  target_compile_features(immersive_audio_muxer_ffmpeg_component_availability_test PRIVATE
    cxx_std_17)
  add_test(NAME immersive_audio_muxer_ffmpeg_component_availability_test
    COMMAND immersive_audio_muxer_ffmpeg_component_availability_test)

  add_executable(immersive_audio_muxer_runtime_logging_test
    tests/runtime_logging_self_test.cpp)
  set_target_properties(immersive_audio_muxer_runtime_logging_test PROPERTIES
    OUTPUT_NAME "iamf_muxer_runtime_logging_test")
  target_include_directories(immersive_audio_muxer_runtime_logging_test PRIVATE
    "${IMMERSIVE_AUDIO_MUXER_FFMPEG_INCLUDE_DIR}")
  target_link_libraries(immersive_audio_muxer_runtime_logging_test PRIVATE
    immersive_audio_muxer_core)
  target_compile_features(immersive_audio_muxer_runtime_logging_test PRIVATE
    cxx_std_17)
  add_test(NAME immersive_audio_muxer_runtime_logging_test
           COMMAND immersive_audio_muxer_runtime_logging_test)

  set(_keep_source_audio_iamf_fixture "")
  if(DEFINED _iamf_tools_source_dir)
    set(_keep_source_audio_iamf_fixture
        "${_iamf_tools_source_dir}/gh_pages/web_demo/data/7_1_4_Opus.iamf")
  endif()
  if(EXISTS "${_keep_source_audio_iamf_fixture}")
    add_executable(immersive_audio_muxer_keep_source_audio_codec_test
      tests/keep_source_audio_codec_self_test.cpp)
    set_target_properties(immersive_audio_muxer_keep_source_audio_codec_test PROPERTIES
      OUTPUT_NAME "iamf_muxer_keep_source_audio_codec_test")
    target_include_directories(immersive_audio_muxer_keep_source_audio_codec_test PRIVATE
      "${IMMERSIVE_AUDIO_MUXER_FFMPEG_INCLUDE_DIR}")
    target_link_libraries(immersive_audio_muxer_keep_source_audio_codec_test PRIVATE
      immersive_audio_muxer_core)
    target_compile_features(immersive_audio_muxer_keep_source_audio_codec_test PRIVATE cxx_std_17)
    add_test(NAME immersive_audio_muxer_keep_source_audio_codec_test
      COMMAND immersive_audio_muxer_keep_source_audio_codec_test
        "${CMAKE_SOURCE_DIR}/tests/data/keep_source_audio_flac_pcm.mp4"
        "${_keep_source_audio_iamf_fixture}"
        "${CMAKE_CURRENT_BINARY_DIR}/keep_source_audio_flac_pcm_keep_test.mp4"
        "${CMAKE_CURRENT_BINARY_DIR}/keep_source_audio_flac_pcm_drop_test.mp4")
  else()
    message(STATUS
      "Skipping Keep Source Audio FLAC/PCM integration test: pinned iamf-tools test vector is unavailable")
  endif()
endif()
