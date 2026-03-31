# Fuzz & stress test client for octaneServGrpc
# Builds a standalone gRPC client — no Octane SDK dependency.
#
# Build:
#   cmake -B build -DBUILD_FUZZ_TESTS=ON
#   cmake --build build --config Release --target octane_fuzz_client

cmake_minimum_required(VERSION 3.16)

add_executable(octane_fuzz_client
    fuzz_client.cpp
    util/test_framework.cpp
    util/connection.cpp
    categories/handle_fuzz.cpp
    categories/type_confusion.cpp
    categories/enum_fuzz.cpp
    categories/numeric_boundary.cpp
    categories/float_fuzz.cpp
    categories/string_fuzz.cpp
    categories/buffer_fuzz.cpp
    categories/proto_malform.cpp
    categories/rapid_fire.cpp
    categories/concurrent_stress.cpp
    categories/connection_abuse.cpp
    categories/stream_abuse.cpp
    categories/stale_handle.cpp
    categories/state_machine.cpp
    categories/resource_exhaust.cpp
    categories/sequence_fuzz.cpp
)

target_include_directories(octane_fuzz_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROTO_GEN_DIR}
)

target_link_libraries(octane_fuzz_client PRIVATE
    serv_protos
    grpc++
)

target_compile_features(octane_fuzz_client PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(octane_fuzz_client PRIVATE /W3 /EHsc)
    # Match parent CRT
    set_property(TARGET octane_fuzz_client PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()
