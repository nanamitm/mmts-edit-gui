cmake_minimum_required(VERSION 3.21)

project(mmts_edit_gui VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

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
    accessControlDescriptor adtsConverter aes aribEncoder aribUtil b24Color
    b24SubtitleConvertor caMessage compressedIPPacket config
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
    mmtGeneralLocationInfo mmtp mmtsMapWriter mmtStream mmtTableFactory mmtTlvDemuxer mpt mpu
    mpuApplicationProcessor mpuAudioProcessor mpuExtendedTimestampDescriptor
    mpuProcessorFactory mpuSubtitleProcessor mpuTimestampDescriptor mpuVideoProcessor
    multimediaServiceInformationDescriptor networkNameDescriptor nit ntp paMessage
    pesPacket plt pugixml relatedBroadcasterDescriptor remoteControlKeyDescriptor
    serviceListDescriptor signalingMessage stream systemManagementDescriptor timebase
    timeUtil tlv tlvDescriptorFactory tlvDescriptors tlvTableFactory
    transmissionControlSignal ttml videoComponentDescriptor)

set(DANTTO4K_SRC_FILES "")
foreach(name ${DANTTO4K_SOURCES})
    list(APPEND DANTTO4K_SRC_FILES "${DANTTO4K_DIR}/src/${name}.cpp")
endforeach()

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

# Project source groups.
set(MODEL_SRC
    src/model/MmtsMap.cpp  src/model/MmtsMap.h
    src/model/MmtsEdit.cpp src/model/MmtsEdit.h
    src/model/MmtsExporter.cpp src/model/MmtsExporter.h)
set(DECODE_SRC
    src/decode/MmtsVideoDecoder.cpp    src/decode/MmtsVideoDecoder.h
    src/decode/MmtsDecodeController.cpp src/decode/MmtsDecodeController.h
    src/decode/MmtsMapBuilder.cpp      src/decode/MmtsMapBuilder.h)
set(UI_SRC
    src/ui/MainWindow.cpp    src/ui/MainWindow.h
    src/ui/PreviewWidget.cpp src/ui/PreviewWidget.h
    src/ui/TimelineWidget.cpp src/ui/TimelineWidget.h)

# ---------------------------------------------------------------------------
# Main application
# ---------------------------------------------------------------------------
add_executable(mmts-edit-gui WIN32
    src/main.cpp
    ${MODEL_SRC} ${DECODE_SRC} ${UI_SRC}
    ${DANTTO4K_SRC_FILES})
target_compile_definitions(mmts-edit-gui PRIVATE WIN32 _WINDOWS UNICODE _UNICODE)
target_include_directories(mmts-edit-gui PRIVATE ${APP_INCLUDE} ${EXT_INCLUDE})
target_link_directories(mmts-edit-gui PRIVATE "${FFMPEG_DIR}/lib")
target_link_libraries(mmts-edit-gui PRIVATE Qt6::Widgets avcodec avutil swscale)
if(WIN32)
    target_link_libraries(mmts-edit-gui PRIVATE Winscard ws2_32)
endif()

# ---------------------------------------------------------------------------
# Headless tests
# ---------------------------------------------------------------------------
# Decoder smoke test.
add_executable(mmts-decode-test
    tests/decode_test.cpp
    src/model/MmtsMap.cpp src/model/MmtsMap.h
    ${DECODE_SRC}
    ${DANTTO4K_SRC_FILES})
target_compile_definitions(mmts-decode-test PRIVATE WIN32 _WINDOWS UNICODE _UNICODE)
target_include_directories(mmts-decode-test PRIVATE ${APP_INCLUDE} ${EXT_INCLUDE})
target_link_directories(mmts-decode-test PRIVATE "${FFMPEG_DIR}/lib")
target_link_libraries(mmts-decode-test PRIVATE Qt6::Gui avcodec avutil swscale)
if(WIN32)
    target_link_libraries(mmts-decode-test PRIVATE Winscard ws2_32)
endif()

# Threaded controller + cache smoke test.
add_executable(mmts-controller-test
    tests/controller_test.cpp
    src/model/MmtsMap.cpp src/model/MmtsMap.h
    ${DECODE_SRC}
    ${DANTTO4K_SRC_FILES})
target_compile_definitions(mmts-controller-test PRIVATE WIN32 _WINDOWS UNICODE _UNICODE)
target_include_directories(mmts-controller-test PRIVATE ${APP_INCLUDE} ${EXT_INCLUDE})
target_link_directories(mmts-controller-test PRIVATE "${FFMPEG_DIR}/lib")
target_link_libraries(mmts-controller-test PRIVATE Qt6::Gui avcodec avutil swscale)
if(WIN32)
    target_link_libraries(mmts-controller-test PRIVATE Winscard ws2_32)
endif()

# EDL model round-trip test (no GUI / ffmpeg).
add_executable(mmts-edit-test
    tests/edit_test.cpp
    src/model/MmtsEdit.cpp src/model/MmtsEdit.h)
target_include_directories(mmts-edit-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/model")
target_link_libraries(mmts-edit-test PRIVATE Qt6::Core)

# .mmtsmap generation test (uses dantto4k; no ffmpeg).
add_executable(mmts-mapbuild-test
    tests/mapbuild_test.cpp
    src/model/MmtsMap.cpp src/model/MmtsMap.h
    src/decode/MmtsMapBuilder.cpp src/decode/MmtsMapBuilder.h
    ${DANTTO4K_SRC_FILES})
target_compile_definitions(mmts-mapbuild-test PRIVATE WIN32 _WINDOWS UNICODE _UNICODE)
target_include_directories(mmts-mapbuild-test PRIVATE ${APP_INCLUDE} ${EXT_INCLUDE})
target_link_libraries(mmts-mapbuild-test PRIVATE Qt6::Core)
if(WIN32)
    target_link_libraries(mmts-mapbuild-test PRIVATE Winscard ws2_32)
endif()

# .mmts export (RAP-aligned copy) test (no ffmpeg).
add_executable(mmts-export-test
    tests/export_test.cpp
    src/model/MmtsMap.cpp src/model/MmtsMap.h
    src/model/MmtsEdit.cpp src/model/MmtsEdit.h
    src/model/MmtsExporter.cpp src/model/MmtsExporter.h)
target_include_directories(mmts-export-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/model")
target_link_libraries(mmts-export-test PRIVATE Qt6::Core)

# ---------------------------------------------------------------------------
# Copy required ffmpeg runtime DLLs next to the executables.
# ---------------------------------------------------------------------------
set(FFMPEG_DLLS avcodec-62 avutil-60 swscale-9 swresample-6)
foreach(tgt mmts-edit-gui mmts-decode-test mmts-controller-test)
    foreach(dll ${FFMPEG_DLLS})
        if(EXISTS "${FFMPEG_DIR}/bin/${dll}.dll")
            add_custom_command(TARGET ${tgt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${FFMPEG_DIR}/bin/${dll}.dll" "$<TARGET_FILE_DIR:${tgt}>")
        endif()
    endforeach()
endforeach()
