cmake_minimum_required(VERSION 3.14)
project(pqfl_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native -O3 -fopenmp-simd")
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../proto)
set(PROTO_FILES
    ${PROTO_DIR}/pqfl.proto
    ${PROTO_DIR}/health.proto
    ${PROTO_DIR}/admin.proto
    ${PROTO_DIR}/worker.proto
)

find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(OpenSSL REQUIRED)

# Find the gRPC C++ plugin executable
find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
if(NOT GRPC_CPP_PLUGIN_EXECUTABLE)
    message(FATAL_ERROR "grpc_cpp_plugin not found!")
endif()

# -- Generate protobuf sources
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

# -- Generate gRPC sources
set(GRPC_SRCS)
set(GRPC_HDRS)
foreach(PROTO ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO} NAME_WE)
    list(APPEND GRPC_SRCS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.cc")
    list(APPEND GRPC_HDRS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.h")
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.cc"
               "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.h"
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
        ARGS --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
             --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
             -I${PROTO_DIR}
             ${PROTO}
        DEPENDS ${PROTO}
    )
endforeach()

add_executable(pqfl_server
    main.cpp
    admin_service.cpp
    crypto_engine.cpp
    federated_service.cpp
    identity_utils.cpp
    logger.cpp
    config.cpp
    state_store.cpp
    worker_service.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

target_include_directories(pqfl_server
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PROTO_DIR}
        ${OpenSSL_INCLUDE_DIR}
)

target_link_directories(pqfl_server
    PRIVATE
        /usr/local/lib
)

target_link_libraries(pqfl_server
    PRIVATE
    gRPC::grpc++
    gRPC::grpc++_reflection
    protobuf::libprotobuf
    OpenSSL::SSL
    OpenSSL::Crypto
)

add_executable(pqfl_test_client
    test_client.cpp
    crypto_engine.cpp
    logger.cpp
    state_store.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)
target_include_directories(pqfl_test_client
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PROTO_DIR}
        ${OpenSSL_INCLUDE_DIR}
)
target_link_directories(pqfl_test_client
    PRIVATE
        /usr/local/lib
)
target_link_libraries(pqfl_test_client
    PRIVATE
    gRPC::grpc++
    protobuf::libprotobuf
    OpenSSL::SSL
    OpenSSL::Crypto
)

add_executable(pqfl_admin_demo
    admin_demo_client.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)
target_include_directories(pqfl_admin_demo
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PROTO_DIR}
        ${OpenSSL_INCLUDE_DIR}
)
target_link_directories(pqfl_admin_demo
    PRIVATE
        /usr/local/lib
)
target_link_libraries(pqfl_admin_demo
    PRIVATE
    gRPC::grpc++
    protobuf::libprotobuf
    OpenSSL::SSL
    OpenSSL::Crypto
)

add_executable(pqfl_admin_cli
    admin_cli.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)
target_include_directories(pqfl_admin_cli
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PROTO_DIR}
        ${OpenSSL_INCLUDE_DIR}
)
target_link_directories(pqfl_admin_cli
    PRIVATE
        /usr/local/lib
)
target_link_libraries(pqfl_admin_cli
    PRIVATE
    gRPC::grpc++
    protobuf::libprotobuf
    OpenSSL::SSL
    OpenSSL::Crypto
)

add_executable(pqfl_worker_demo
    worker_demo_client.cpp
    crypto_engine.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)
target_include_directories(pqfl_worker_demo
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PROTO_DIR}
        ${OpenSSL_INCLUDE_DIR}
)
target_link_directories(pqfl_worker_demo
    PRIVATE
        /usr/local/lib
)
target_link_libraries(pqfl_worker_demo
    PRIVATE
    gRPC::grpc++
    protobuf::libprotobuf
    OpenSSL::SSL
    OpenSSL::Crypto
)

add_executable(pqfl_self_test
    pqfl_self_test.cpp
    crypto_engine.cpp
    logger.cpp
    state_store.cpp
)
target_include_directories(pqfl_self_test
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PROTO_DIR}
        ${OpenSSL_INCLUDE_DIR}
)
target_link_libraries(pqfl_self_test
    PRIVATE
    protobuf::libprotobuf
    OpenSSL::SSL
    OpenSSL::Crypto
)

install(TARGETS
    pqfl_server
    pqfl_test_client
    pqfl_self_test
    pqfl_admin_demo
    pqfl_admin_cli
    pqfl_worker_demo
    DESTINATION bin)
