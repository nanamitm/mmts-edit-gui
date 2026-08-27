cmake_minimum_required(VERSION 3.21)

project(mmts_edit_gui VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
option(MMTS_ENABLE_TSDUCK "Enable MPEG-2 TS export support via TSDuck" ${WIN32})

# ---------------------------------------------------------------------------
# External projects
# ---------------------------------------------------------------------------
set(DANTTO4K_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../dantto4k" CACHE PATH "Path to dantto4k checkout")
set(FFMPEG_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/ffmpeg" CACHE PATH "Path to prebuilt ffmpeg (include/lib/bin)")

if(NOT EXISTS "${DANTTO4K_DIR}/src/mmtTlvDemuxer.h")
    message(FATAL_ERROR "dantto4k sources not found at ${DANTTO4K_DIR}. Set -DDANTTO4K_DIR=...")
endif()
if(NOT EXISTS "${FFMPEG_DIR}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "ffmpeg dev libraries not found at ${FFMPEG_DIR}. Set -DFFMPEG_DIR=...")
endif()

# dantto4k core sources (mirror of mmts-dsfilter's subset; excludes TSDuck/CAS/BonDriver).
set(DANTTO4K_SOURCES
    accessControlDescriptor additionalAribSubtitleInfo adtsConverter aes
    aribCharsetEncoder aribCharsetTables aribTextEncoder b24Color
    b24SubtitleConverter caMessage compressedIPPacket config
    contentCopyControlDescriptor damt dataTransmissionMessage dataUnit dcct ddmt
    ecm emt eventPackageDescriptor extensionHeaderScrambling fragmentAssembler
    indexItem ipv6 m2SectionMessage m2ShortSectionMessage mhAit
    mhApplicationBoundaryAndPermissionDescriptor mhApplicationDescriptor
    mhAudioComponentDescriptor mhAutostartPriorityDescriptor mhBit
    mhBroadcasterNameDescriptor mhCacheControlInfoDescriptor mhCaContractInformation
    mhCdt mhContentDescriptor mhDataComponentDescriptor mhEit mhEventGroupDescriptor
    mhExtendedEventDescriptor mhExternalApplicationControlDescriptor mhLinkageDescriptor
    mhLogoTransmissionDescriptor mhParentalRatingDescriptor mhRandomizedLatencyDescriptor
    mhSdt mhSeriesDescriptor mhServiceDescriptor mhServiceListDescriptor
    mhShortEventDescriptor mhSimpleApplicationLocationDescriptor mhSiParameterDescriptor
    mhStreamIdentificationDescriptor
    mhTot mhTransportProtocolDescriptor mmtDescriptorFactory mmtDescriptors
    mmtGeneralLocationInfo mmtp mmtSplice mmtsMapWriter mmtStream mmtTableFactory mmtTlvDemuxer mpt mpu
    mpuApplicationProcessor mpuAudioProcessor mpuExtendedTimestampDescriptor
    mpuProcessorFactory mpuSubtitleProcessor mpuTimestampDescriptor mpuVideoProcessor
    multimediaServiceInformationDescriptor networkNameDescriptor nit ntp
    ntpTimestamp paMessage
    pesPacket plt pugixml relatedBroadcasterDescriptor remoteControlKeyDescriptor
    serviceListDescriptor signalingMessage stream systemManagementDescriptor timebase
    timeUtil tlv tlvDescriptorFactory tlvDescriptors tlvTableFactory
    transmissionControlSignal videoComponentDescriptor
    # dantto4k's TTML layer lives in its own subdirectory since the upstream
    # rewrite; the loop below joins these onto ${DANTTO4K_DIR}/src/.
    ttml/b24_converter ttml/drcs ttml/parser ttml/resolver ttml/style)

set(DANTTO4K_SRC_FILES "")
foreach(name ${DANTTO4K_SOURCES})
    list(APPEND DANTTO4K_SRC_FILES "${DANTTO4K_DIR}/src/${name}.cpp")
endforeach()

# dantto4k logs subtitle diagnostics through subtitleDebugLog(), which
# remuxerHandler.cpp defines and both b24SubtitleConverter.cpp and
# ttml/drcs.cpp reference.  The remuxer needs TSDuck, and several targets do
# not link it even when TSDuck is on, so every target gets exactly one
# definition: the remuxer where it is wanted, this no-op everywhere else.
set(SUBTITLE_LOG_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/SubtitleDebugLogStub.cpp")

# dantto4k is older, less strict C++ -- relax conformance & noisy warnings for it only.
if(MSVC)
    set_source_files_properties(${DANTTO4K_SRC_FILES} PROPERTIES
        COMPILE_OPTIONS "/permissive;/wd4996;/wd4244;/wd4267;/wd4018;/wd4800")
endif()

# ---------------------------------------------------------------------------
# Include directories
#   APP_INCLUDE keeps plain "#include \"Foo.h\"" working across src subfolders.
# ---------------------------------------------------------------------------
set(APP_INCLUDE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/model"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/decode"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
set(EXT_INCLUDE
    "${DANTTO4K_DIR}/src"
    "${DANTTO4K_DIR}/thirdparty/asio/asio/include"
    "${FFMPEG_DIR}/include")

# ---------------------------------------------------------------------------
# tsduck (MMT -> MPEG-2 TS remux, used by the .ts exporter). Reuses the static
# libs already built under dantto4k/thirdparty/tsduck.
# ---------------------------------------------------------------------------
if(MMTS_ENABLE_TSDUCK)
    set(TSDUCK_ROOT "${DANTTO4K_DIR}/thirdparty/tsduck")
    if(NOT EXISTS "${TSDUCK_ROOT}/bin/include/tsduck.h")
        message(FATAL_ERROR "tsduck not found/built at ${TSDUCK_ROOT} (need bin/include + bin/<Config>-x64 libs).")
    endif()
    file(GLOB_RECURSE _ts_hdrs "${TSDUCK_ROOT}/src/libtscore/*.h" "${TSDUCK_ROOT}/src/libtsduck/*.h")
    set(TSDUCK_INCLUDE "")
    foreach(_h ${_ts_hdrs})
        if(NOT _h MATCHES "/(unix|linux|mac|bsd|private|__pycache__)/")
            get_filename_component(_d "${_h}" DIRECTORY)
            list(APPEND TSDUCK_INCLUDE "${_d}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES TSDUCK_INCLUDE)
    list(APPEND TSDUCK_INCLUDE "${TSDUCK_ROOT}/bin/include")
    set(TSDUCK_LIBDIR "${TSDUCK_ROOT}/bin/$<IF:$<CONFIG:Debug>,Debug,Release>-x64")

    # dantto4k's TS remuxer (depends on tsduck) -- relaxed warnings like the rest.
    # It also carries subtitleDebugLog(), so targets taking REMUX_SRC must not
    # add SUBTITLE_LOG_STUB as well.
    set(REMUX_SRC "${DANTTO4K_DIR}/src/remuxerHandler.cpp")
    if(MSVC)
        set_source_files_properties(${REMUX_SRC} PROPERTIES
            COMPILE_OPTIONS "/permissive;/wd4996;/wd4244;/wd4267;/wd4018;/wd4800;/wd4819")
    endif()

    # Adds tsduck include/defs/libs (with the required /WHOLEARCHIVE) to a target.
    function(enable_tsduck tgt)
        target_compile_definitions(${tgt} PRIVATE TSDUCK_STATIC_LIBRARY TSCORE_STATIC_LIBRARY)
        target_include_directories(${tgt} PRIVATE ${TSDUCK_INCLUDE})
        target_link_directories(${tgt} PRIVATE "${TSDUCK_LIBDIR}")
        target_link_libraries(${tgt} PRIVATE tsducklib tscorelib)
        target_link_options(${tgt} PRIVATE "/WHOLEARCHIVE:tsducklib.lib" "/WHOLEARCHIVE:tscorelib.lib")
        if(MSVC)
            target_compile_options(${tgt} PRIVATE /wd4819 /Wv:17)
        endif()
    endfunction()
endif()

set(SMART_RENDER_SRC
    src/decode/MmtsSmartExporter.cpp src/decode/MmtsSmartExporter.h
    src/decode/HevcReencoder.cpp src/decode/HevcReencoder.h)

set(TS_SRC "")
if(MMTS_ENABLE_TSDUCK)
    list(APPEND TS_SRC
        src/decode/MmtsTsExporter.cpp src/decode/MmtsTsExporter.h
        ${REMUX_SRC})
else()
    # Without the remuxer these targets have no subtitleDebugLog() definition.
    list(APPEND TS_SRC "${SUBTITLE_LOG_STUB}")
endif()

# Project source groups.
set(MODEL_SRC
    src/model/MmtsMap.cpp  src/model/MmtsMap.h
    src/model/MmtsEdit.cpp src/model/MmtsEdit.h
    src/model/MmtsCutPlan.cpp src/model/MmtsCutPlan.h
    src/model/MmtsExporter.cpp src/model/MmtsExporter.h)
set(DECODE_SRC
    src/decode/MmtsVideoDecoder.cpp    src/decode/MmtsVideoDecoder.h
    src/decode/MmtsDecodeController.cpp src/decode/MmtsDecodeController.h
    src/decode/MmtsMapBuilder.cpp      src/decode/MmtsMapBuilder.h)
set(UI_SRC
    src/ui/MainWindow.cpp    src/ui/MainWindow.h
    src/ui/PreviewWidget.cpp src/ui/PreviewWidget.h
    src/ui/TimelineWidget.cpp src/ui/TimelineWidget.h)

function(enable_windows_runtime tgt)
    if(WIN32)
        target_compile_definitions(${tgt} PRIVATE UNICODE _UNICODE)
        target_link_libraries(${tgt} PRIVATE Winscard ws2_32)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Main application
# ---------------------------------------------------------------------------
set(MMTS_EDIT_GUI_SOURCES
    src/main.cpp
    resources/icons.qrc
    ${MODEL_SRC} ${DECODE_SRC} ${UI_SRC} ${SMART_RENDER_SRC} ${TS_SRC}
    ${DANTTO4K_SRC_FILES})
if(WIN32)
    list(APPEND MMTS_EDIT_GUI_SOURCES resources/app.rc)
    add_executable(mmts-edit-gui WIN32 ${MMTS_EDIT_GUI_SOURCES})
else()
    add_executable(mmts-edit-gui ${MMTS_EDIT_GUI_SOURCES})
endif()
target_compile_definitions(mmts-edit-gui PRIVATE MMTS_ENABLE_TSDUCK=$<BOOL:${MMTS_ENABLE_TSDUCK}>)
target_include_directories(mmts-edit-gui PRIVATE ${APP_INCLUDE} ${EXT_INCLUDE})
target_link_directories(mmts-edit-gui PRIVATE "${FFMPEG_DIR}/lib")
target_link_libraries(mmts-edit-gui PRIVATE Qt6::Widgets avcodec avutil swscale)
enable_windows_runtime(mmts-edit-gui)
if(MMTS_ENABLE_TSDUCK)
    enable_tsduck(mmts-edit-gui)
endif()

# ---------------------------------------------------------------------------
# Headless tests
# ---------------------------------------------------------------------------
# Decoder smoke test.
add_executable(mmts-decode-test
    tests/decode_test.cpp
    src/model/MmtsMap.cpp src/model/MmtsMap.h
    ${DECODE_SRC}
    ${DANTTO4K_SRC_FILES} ${SUBTITLE_LOG_STUB})
target_include_directories(mmts-decode-test PRIVATE ${APP_INCLUDE} ${EXT_INCLUDE})
target_link_directories(mmts-decode-test PRIVATE "${FFMPEG_DIR}/lib")
target_link_libraries(mmts-decode-test PRIVATE Qt6::Gui avcodec avutil swscale)
enable_windows_runtime(mmts-decode-test)

# Threaded controller + cache smoke test.
add_executable(mmts-controller-test
    tests/controller_test.cpp
    src/model/MmtsMap.cpp src/model/MmtsMap.h
    ${DECODE_SRC}
    ${DANTTO4K_SRC_FILES} ${SUBTITLE_LOG_STUB})
target_include_directories(mmts-controller-test PRIVATE ${APP_INCLUDE} ${EXT_INCLUDE})
target_link_directories(mmts-controller-test PRIVATE "${FFMPEG_DIR}/lib")
target_link_libraries(mmts-controller-test PRIVATE Qt6::Gui avcodec avutil swscale)
enable_windows_runtime(mmts-controller-test)

# EDL model round-trip test (no GUI / ffmpeg).
add_executable(mmts-edit-test
    tests/edit_test.cpp
    src/model/MmtsEdit.cpp src/model/MmtsEdit.h)
target_include_directories(mmts-edit-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/model")
target_link_libraries(mmts-edit-test PRIVATE Qt6::Core)

if(MMTS_ENABLE_TSDUCK)
    # TS export test (uses dantto4k + tsduck + ffmpeg/avutil).
    add_executable(mmts-ts-export-test
        tests/ts_export_test.cpp
        src/model/MmtsMap.cpp src/model/MmtsMap.h
        src/model/MmtsEdit.cpp src/model/MmtsEdit.h
        src/model/MmtsCutPlan.cpp src/model/MmtsCutPlan.h
        src/decode/MmtsTsExporter.cpp src/decode/MmtsTsExporter.h
        src/decode/HevcReencoder.cpp src/decode/HevcReencoder.h
        ${REMUX_SRC}
        ${DANTTO4K_SRC_FILES})
    target_include_directories(mmts-ts-export-test PRIVATE ${APP_INCLUDE} ${EXT_INCLUDE})
    target_link_directories(mmts-ts-export-test PRIVATE "${FFMPEG_DIR}/lib")
    target_link_libraries(mmts-ts-export-test PRIVATE Qt6::Core avcodec avutil swscale)
    enable_windows_runtime(mmts-ts-export-test)
    enable_tsduck(mmts-ts-export-test)
endif()

# HEVC re-encode test (smart-render building block; uses dantto4k + ffmpeg).
add_executable(mmts-reencode-test
    tests/reencode_test.cpp
    src/model/MmtsMap.cpp src/model/MmtsMap.h
    src/decode/HevcReencoder.cpp src/decode/HevcReencoder.h
    ${DANTTO4K_SRC_FILES} ${SUBTITLE_LOG_STUB})
target_include_directories(mmts-reencode-test PRIVATE ${APP_INCLUDE} ${EXT_INCLUDE})
target_link_directories(mmts-reencode-test PRIVATE "${FFMPEG_DIR}/lib")
target_link_libraries(mmts-reencode-test PRIVATE Qt6::Core avcodec avutil swscale)
enable_windows_runtime(mmts-reencode-test)

# .mmtsmap generation test (uses dantto4k; no ffmpeg).
add_executable(mmts-mapbuild-test
    tests/mapbuild_test.cpp
    src/model/MmtsMap.cpp src/model/MmtsMap.h
    src/decode/MmtsMapBuilder.cpp src/decode/MmtsMapBuilder.h
    ${DANTTO4K_SRC_FILES} ${SUBTITLE_LOG_STUB})
target_include_directories(mmts-mapbuild-test PRIVATE ${APP_INCLUDE} ${EXT_INCLUDE})
target_link_libraries(mmts-mapbuild-test PRIVATE Qt6::Core)
enable_windows_runtime(mmts-mapbuild-test)

# Smart .mmts export test (frame-accurate; uses dantto4k + ffmpeg, no tsduck).
add_executable(mmts-smart-export-test
    tests/smart_export_test.cpp
    src/model/MmtsMap.cpp src/model/MmtsMap.h
    src/model/MmtsEdit.cpp src/model/MmtsEdit.h
    src/model/MmtsCutPlan.cpp src/model/MmtsCutPlan.h
    src/decode/MmtsSmartExporter.cpp src/decode/MmtsSmartExporter.h
    src/decode/HevcReencoder.cpp src/decode/HevcReencoder.h
    ${DANTTO4K_SRC_FILES} ${SUBTITLE_LOG_STUB})
target_include_directories(mmts-smart-export-test PRIVATE ${APP_INCLUDE} ${EXT_INCLUDE})
target_link_directories(mmts-smart-export-test PRIVATE "${FFMPEG_DIR}/lib")
target_link_libraries(mmts-smart-export-test PRIVATE Qt6::Core avcodec avutil swscale)
enable_windows_runtime(mmts-smart-export-test)

# .mmts export (RAP-aligned copy) test (no ffmpeg).
add_executable(mmts-export-test
    tests/export_test.cpp
    src/model/MmtsMap.cpp src/model/MmtsMap.h
    src/model/MmtsEdit.cpp src/model/MmtsEdit.h
    src/model/MmtsExporter.cpp src/model/MmtsExporter.h)
target_include_directories(mmts-export-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/model")
target_link_libraries(mmts-export-test PRIVATE Qt6::Core)

# ---------------------------------------------------------------------------
# mmts-edit-cli: headless CLI counterpart to mmts-edit-gui (export an existing
# .mmtsedit, or ad-hoc --cut ranges, to .ts/.mmts without opening the GUI).
# ---------------------------------------------------------------------------
add_executable(mmts-edit-cli
    tests/mmts_edit_cli.cpp
    src/model/MmtsMap.cpp     src/model/MmtsMap.h
    src/model/MmtsEdit.cpp    src/model/MmtsEdit.h
    src/model/MmtsCutPlan.cpp src/model/MmtsCutPlan.h
    src/decode/MmtsMapBuilder.cpp   src/decode/MmtsMapBuilder.h
    src/decode/HevcReencoder.cpp    src/decode/HevcReencoder.h
    src/decode/MmtsSmartExporter.cpp src/decode/MmtsSmartExporter.h
    ${TS_SRC}
    ${DANTTO4K_SRC_FILES})
target_compile_definitions(mmts-edit-cli PRIVATE MMTS_ENABLE_TSDUCK=$<BOOL:${MMTS_ENABLE_TSDUCK}>)
target_include_directories(mmts-edit-cli PRIVATE ${APP_INCLUDE} ${EXT_INCLUDE})
target_link_directories(mmts-edit-cli PRIVATE "${FFMPEG_DIR}/lib")
target_link_libraries(mmts-edit-cli PRIVATE Qt6::Core avcodec avutil swscale)
enable_windows_runtime(mmts-edit-cli)
if(MMTS_ENABLE_TSDUCK)
    enable_tsduck(mmts-edit-cli)
endif()

# ---------------------------------------------------------------------------
# Copy required ffmpeg runtime DLLs next to the executables.
# ---------------------------------------------------------------------------
if(WIN32)
    set(FFMPEG_DLLS avcodec-62 avutil-60 swscale-9 swresample-6)
    foreach(tgt mmts-edit-gui mmts-decode-test mmts-controller-test mmts-edit-cli)
        foreach(dll ${FFMPEG_DLLS})
            if(EXISTS "${FFMPEG_DIR}/bin/${dll}.dll")
                add_custom_command(TARGET ${tgt} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${FFMPEG_DIR}/bin/${dll}.dll" "$<TARGET_FILE_DIR:${tgt}>")
            endif()
        endforeach()
    endforeach()
endif()
