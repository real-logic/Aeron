#
# Copyright 2014-2021 Real Logic Limited.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

find_package(Java REQUIRED)

set(CODEC_SCHEMA ${ARCHIVE_CODEC_SCHEMA_DIR}/aeron-archive-codecs.xml)
set(GENERATED_CODECS
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/BoundedReplayRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/ControlResponse.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/ConnectRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/CloseSessionRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/StartRecordingRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/StartRecordingRequest2.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/StopRecordingRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/StopRecordingByIdentityRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/ReplayRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/StopReplayRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/StopAllReplaysRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/ListRecordingsRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/ListRecordingsForUriRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/ListRecordingRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/ExtendRecordingRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/ExtendRecordingRequest2.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/RecordingPositionRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/TruncateRecordingRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/PurgeRecordingRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/StopRecordingSubscriptionRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/StartPositionRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/StopPositionRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/FindLastMatchingRecordingRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/ListRecordingSubscriptionsRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/ReplicateRequest2.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/DetachSegmentsRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/DeleteDetachedSegmentsRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/PurgeSegmentsRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/AttachSegmentsRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/MigrateSegmentsRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/KeepAliveRequest.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/CatalogHeader.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/RecordingDescriptorHeader.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/RecordingDescriptor.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/RecordingSubscriptionDescriptor.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/RecordingStarted.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/RecordingProgress.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/RecordingStopped.h
    ${ARCHIVE_CODEC_TARGET_DIR}/aeron_archive_client/RecordingSignalEvent.h)

add_custom_command(OUTPUT ${GENERATED_CODECS}
    COMMAND ${CMAKE_COMMAND} -E env JAVA_HOME=$ENV{JAVA_HOME} BUILD_JAVA_HOME=$ENV{BUILD_JAVA_HOME} BUILD_JAVA_VERSION=$ENV{BUILD_JAVA_VERSION} ${GRADLE_WRAPPER} -Dcodec.target.dir=${ARCHIVE_CODEC_TARGET_DIR} :aeron-archive:generateCppCodecs --no-daemon --console=plain -q
    DEPENDS ${CODEC_SCHEMA} aeron-all-jar
    WORKING_DIRECTORY ${ARCHIVE_CODEC_WORKING_DIR}
    COMMENT "Generating C++ Archive codecs")

add_custom_target(codecs DEPENDS ${GENERATED_CODECS})

SET(SOURCE
    client/ArchiveProxy.cpp
    client/ControlResponsePoller.cpp
    client/ControlResponseAdapter.cpp
    client/RecordingDescriptorPoller.cpp
    client/RecordingSubscriptionDescriptorPoller.cpp
    client/RecordingEventsPoller.cpp
    client/RecordingEventsAdapter.cpp
    client/RecordingSignalAdapter.cpp
    client/AeronArchive.cpp
    client/ReplayMerge.cpp)

SET(HEADERS
    client/ArchiveException.h
    client/ArchiveConfiguration.h
    client/ArchiveProxy.h
    client/ControlResponsePoller.h
    client/ControlResponseAdapter.h
    client/RecordingDescriptorPoller.h
    client/RecordingSubscriptionDescriptorPoller.h
    client/RecordingEventsPoller.h
    client/RecordingEventsAdapter.h
    client/RecordingSignalAdapter.h
    client/RecordingPos.h
    client/AeronArchive.h
    client/ReplayMerge.h)

# static library
add_library(aeron_archive_client STATIC ${SOURCE} ${HEADERS})
add_library(aeron_archive_client_shared SHARED ${SOURCE} ${HEADERS})
add_library(aeron_archive_client_wrapper STATIC ${SOURCE} ${HEADERS})

add_dependencies(aeron_archive_client codecs)
add_dependencies(aeron_archive_client_shared codecs)
add_dependencies(aeron_archive_client_wrapper codecs)

target_include_directories(aeron_archive_client
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${ARCHIVE_CODEC_TARGET_DIR})

target_include_directories(aeron_archive_client_wrapper
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${ARCHIVE_CODEC_TARGET_DIR})

target_include_directories(aeron_archive_client_shared
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${ARCHIVE_CODEC_TARGET_DIR})

if (NOT WIN32)
    set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
endif ()

target_link_libraries(aeron_archive_client
    PRIVATE aeron_client
    INTERFACE ${CMAKE_THREAD_LIBS_INIT})

target_link_libraries(aeron_archive_client_shared
    PRIVATE aeron_client_shared
    INTERFACE ${CMAKE_THREAD_LIBS_INIT})

target_link_libraries(aeron_archive_client_wrapper
    PRIVATE aeron_client_wrapper aeron
    INTERFACE ${CMAKE_THREAD_LIBS_INIT})

if (AERON_INSTALL_TARGETS)
    install(TARGETS aeron_archive_client_shared aeron_archive_client 
        LIBRARY DESTINATION lib 
        ARCHIVE DESTINATION lib)
    install(DIRECTORY . DESTINATION include FILES_MATCHING PATTERN "*.h")
endif ()
