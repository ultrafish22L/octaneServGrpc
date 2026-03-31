# octaneServGrpc CMakeLists.txt
#
# Builds a Windows tray application that:
#   1. Links the Octane Render SDK (octane.dll/octane.lib)
#   2. Hosts a gRPC server serving 96 Octane proto services
#   3. Maps gRPC calls to SDK C++ API calls
#
# gRPC is auto-downloaded via FetchContent on first configure (~2-5 min).
# Subsequent builds use the cached download.

cmake_minimum_required(VERSION 3.16)

# CMP0091: use CMAKE_MSVC_RUNTIME_LIBRARY instead of per-target flags
cmake_policy(SET CMP0091 NEW)

project(octaneServGrpc VERSION 1.0.0 LANGUAGES CXX)

# Use DLL CRT globally — required for gRPC's BoringSSL and abseil to link cleanly.
# Octane SDK (octane.dll) is a DLL so DLL CRT is compatible.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================================================================
# Options
# =============================================================================
set(GRPC_VERSION "v1.62.1" CACHE STRING "gRPC version to fetch")

# =============================================================================
# Octane Render SDK paths
# =============================================================================
set(OCTANE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../OctaneRenderSDK_Studio+_2026_2_win"
    CACHE PATH "Path to Octane Render SDK")

if(NOT EXISTS "${OCTANE_SDK_DIR}/lib/octane.lib")
    message(FATAL_ERROR "Octane SDK not found at ${OCTANE_SDK_DIR}. "
        "Set OCTANE_SDK_DIR to the SDK location.")
endif()

set(OCTANE_INCLUDE_DIR "${OCTANE_SDK_DIR}/include")
set(OCTANE_LIB "${OCTANE_SDK_DIR}/lib/octane.lib")
set(OCTANE_BIN_DIR "${OCTANE_SDK_DIR}/bin")

# Build as Studio+ tier 2
add_definitions(-DOCTANE_STUDIO_VERSION)
# NOTE: Do NOT define OCTANE_STANDALONE — it hides apiMode_Shared_start/exit/activate
# which we need for the plugin SDK lifecycle.

# =============================================================================
# gRPC via FetchContent (auto-download)
#
# First configure downloads gRPC + abseil + protobuf + re2 + cares (~2-5 min).
# Cached in build/_deps/ for subsequent builds.
# Matches the add_subdirectory pattern used by octaneservermodule.
# =============================================================================
include(FetchContent)

# Prevent gRPC/protobuf from installing into system dirs
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
# Use bundled dependencies (don't look for system installs)
set(gRPC_ABSL_PROVIDER "module" CACHE STRING "" FORCE)
set(gRPC_CARES_PROVIDER "module" CACHE STRING "" FORCE)
set(gRPC_PROTOBUF_PROVIDER "module" CACHE STRING "" FORCE)
set(gRPC_RE2_PROVIDER "module" CACHE STRING "" FORCE)
set(gRPC_SSL_PROVIDER "module" CACHE STRING "" FORCE)
set(gRPC_ZLIB_PROVIDER "module" CACHE STRING "" FORCE)
# Suppress protobuf deprecation warnings
set(protobuf_ABSL_PROVIDER "module" CACHE STRING "" FORCE)

message(STATUS "Fetching gRPC ${GRPC_VERSION} (first run downloads ~200MB, takes 2-5 min)...")

FetchContent_Declare(
    grpc
    GIT_REPOSITORY https://github.com/grpc/grpc.git
    GIT_TAG        ${GRPC_VERSION}
    GIT_SHALLOW    TRUE           # Don't fetch full history
    GIT_SUBMODULES_RECURSE TRUE   # Pull abseil, protobuf, etc.
)

# EXCLUDE_FROM_ALL = don't build gRPC tests/examples
FetchContent_GetProperties(grpc)
if(NOT grpc_POPULATED)
    FetchContent_Populate(grpc)
    add_subdirectory(${grpc_SOURCE_DIR} ${grpc_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

# After add_subdirectory, these targets are available:
#   grpc++              - gRPC C++ library
#   libprotobuf         - Protobuf runtime
#   protobuf::protoc    - protoc compiler (target)
#   grpc_cpp_plugin     - gRPC codegen plugin (target)

# Resolve tool paths from targets (works for both build-tree and installed)
set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
set(_GRPC_CPP_PLUGIN $<TARGET_FILE:grpc_cpp_plugin>)

message(STATUS "gRPC configured successfully.")

# =============================================================================
# Proto compilation
# =============================================================================
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
file(GLOB PROTO_FILES "${PROTO_DIR}/*.proto")

# Exclude duplicate proto files:
# - apinodesystem.proto is a monolith (11K lines) containing all the split files
#   (apinodesystem_1 through _8). The split files are authoritative.
# - apinodearray.proto duplicates apinodesystem_5.proto (ApiNodeArray)
# - apiitemarray.proto duplicates apinodesystem_1.proto (ApiItemArray)
list(FILTER PROTO_FILES EXCLUDE REGEX "apinodesystem\\.proto$")
list(FILTER PROTO_FILES EXCLUDE REGEX "apinodearray\\.proto$")
list(FILTER PROTO_FILES EXCLUDE REGEX "apiitemarray\\.proto$")

# Proto output directory
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

# Well-known types (google/protobuf/empty.proto etc.) live inside the fetched protobuf source
set(PROTOBUF_IMPORT_DIR "${grpc_SOURCE_DIR}/third_party/protobuf/src")

# Generate C++ sources from each proto file
set(PROTO_SRCS)
set(GRPC_SRCS)

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)

    set(PB_CC "${PROTO_GEN_DIR}/${PROTO_NAME}.pb.cc")
    set(PB_H  "${PROTO_GEN_DIR}/${PROTO_NAME}.pb.h")
    set(GRPC_CC "${PROTO_GEN_DIR}/${PROTO_NAME}.grpc.pb.cc")
    set(GRPC_H  "${PROTO_GEN_DIR}/${PROTO_NAME}.grpc.pb.h")

    list(APPEND PROTO_SRCS ${PB_CC})
    list(APPEND GRPC_SRCS ${GRPC_CC})

    add_custom_command(
        OUTPUT ${PB_CC} ${PB_H} ${GRPC_CC} ${GRPC_H}
        COMMAND ${_PROTOBUF_PROTOC}
            --proto_path=${PROTO_DIR}
            --proto_path=${PROTOBUF_IMPORT_DIR}
            --cpp_out=${PROTO_GEN_DIR}
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN}
            ${PROTO_FILE}
        DEPENDS ${PROTO_FILE} protobuf::protoc grpc_cpp_plugin
        COMMENT "Compiling ${PROTO_NAME}.proto"
    )
endforeach()

# Static library of all compiled protos
add_library(serv_protos STATIC ${PROTO_SRCS} ${GRPC_SRCS})
target_include_directories(serv_protos PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(serv_protos PUBLIC grpc++ libprotobuf)
if(MSVC)
    # Suppress warnings in generated code
    target_compile_options(serv_protos PRIVATE /W0)
endif()

# =============================================================================
# Shared sources for both server executables
# =============================================================================
set(SERV_SOURCES
    src/main.cpp
    src/sdk_engine.cpp
    src/grpc_server.cpp
    src/SERVKEY.cpp
    src/util/handle_registry.cpp
    src/util/server_log.cpp
)

# Helper: configure a server target with SDK, gRPC, and platform settings.
# Shared between octaneServGrpc (tray) and octaneGrpcSE (headless console).
function(configure_serv_target TARGET_NAME)
    target_include_directories(${TARGET_NAME} PRIVATE
        ${OCTANE_INCLUDE_DIR}
        ${OCTANE_INCLUDE_DIR}/octanegui
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(${TARGET_NAME} PRIVATE
        ${OCTANE_LIB}
        serv_protos
        grpc++
    )
    target_include_directories(${TARGET_NAME} PRIVATE ${PROTO_GEN_DIR})
    if(WIN32)
        target_compile_options(${TARGET_NAME} PRIVATE /EHa)
        target_link_libraries(${TARGET_NAME} PRIVATE bcrypt ws2_32 crypt32)
        set_property(TARGET ${TARGET_NAME} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endif()
endfunction()

# Octane runtime files to copy next to each exe
set(OCTANE_RUNTIME_FILES
    octane.dll
    octane.dat
    libEGL.dll
    libGLESv2.dll
    libcef.dll
    chrome_elf.dll
    d3dcompiler_47.dll
    vk_swiftshader.dll
    vulkan-1.dll
    icudtl.dat
    resources.pak
    chrome_100_percent.pak
    chrome_200_percent.pak
    snapshot_blob.bin
    v8_context_snapshot.bin
    vk_swiftshader_icd.json
    en-US.pak
    cef-helper.exe
)

# Helper: add post-build copy commands for Octane runtime DLLs
function(copy_octane_runtime TARGET_NAME)
    if(WIN32)
        foreach(RUNTIME_FILE ${OCTANE_RUNTIME_FILES})
            if(EXISTS "${OCTANE_BIN_DIR}/${RUNTIME_FILE}")
                add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${OCTANE_BIN_DIR}/${RUNTIME_FILE}"
                        "$<TARGET_FILE_DIR:${TARGET_NAME}>/${RUNTIME_FILE}"
                    COMMENT "Copying ${RUNTIME_FILE} → ${TARGET_NAME}"
                )
            endif()
        endforeach()
    endif()
endfunction()

# DLL CRT for serv_protos (shared by both targets)
if(WIN32)
    set_property(TARGET serv_protos PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

# =============================================================================
# Target 1: octaneServGrpc — Windows tray app (original)
# =============================================================================
if(WIN32)
    add_executable(octaneServGrpc WIN32
        ${SERV_SOURCES}
        src/OctaneServGrpc.rc
    )
    target_compile_definitions(octaneServGrpc PRIVATE SERV_WIN32_APP)
    configure_serv_target(octaneServGrpc)
    copy_octane_runtime(octaneServGrpc)
else()
    add_executable(octaneServGrpc ${SERV_SOURCES})
    configure_serv_target(octaneServGrpc)
endif()

# =============================================================================
# Target 2: octaneGrpcSE — headless console exe for Electron integration
# =============================================================================
if(WIN32)
    add_executable(octaneGrpcSE
        ${SERV_SOURCES}
        src/OctaneServGrpc.rc
    )
else()
    add_executable(octaneGrpcSE ${SERV_SOURCES})
endif()
configure_serv_target(octaneGrpcSE)
copy_octane_runtime(octaneGrpcSE)

# =============================================================================
# Post-build: copy both exes + runtime to dev/bin/ for easy access
# =============================================================================
set(DEV_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bin")

# Helper: copy an exe and all Octane runtime files to dev/bin/
function(copy_to_dev_bin TARGET_NAME)
    # Copy the exe itself
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DEV_BIN_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${TARGET_NAME}>"
            "${DEV_BIN_DIR}/$<TARGET_FILE_NAME:${TARGET_NAME}>"
        COMMENT "Copying ${TARGET_NAME} → dev/bin/"
    )
    # Copy runtime dependencies
    if(WIN32)
        foreach(RUNTIME_FILE ${OCTANE_RUNTIME_FILES})
            if(EXISTS "${OCTANE_BIN_DIR}/${RUNTIME_FILE}")
                add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${OCTANE_BIN_DIR}/${RUNTIME_FILE}"
                        "${DEV_BIN_DIR}/${RUNTIME_FILE}"
                    COMMENT "Copying ${RUNTIME_FILE} → dev/bin/"
                )
            endif()
        endforeach()
    endif()
endfunction()

copy_to_dev_bin(octaneServGrpc)
copy_to_dev_bin(octaneGrpcSE)

# =============================================================================
# Summary
# =============================================================================
message(STATUS "")
message(STATUS "=== octaneServGrpc ===")
message(STATUS "  Octane SDK:   ${OCTANE_SDK_DIR}")
message(STATUS "  gRPC version: ${GRPC_VERSION}")
message(STATUS "  Proto dir:    ${PROTO_DIR}")
list(LENGTH PROTO_FILES _PROTO_COUNT)
message(STATUS "  Proto count:  ${_PROTO_COUNT} files")
message(STATUS "  Targets:      octaneServGrpc.exe (tray app), octaneGrpcSE.exe (headless console)")
message(STATUS "")
