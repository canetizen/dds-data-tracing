cmake_minimum_required(VERSION 3.10)
project(command_center C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

find_package(CURL REQUIRED)
find_package(Protobuf REQUIRED)
find_package(opentelemetry-cpp REQUIRED)

add_executable(app
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/CombatMessages.c
)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(app
    ddsc
    pthread
    ${CURL_LIBRARIES}
    ${Protobuf_LIBRARIES}
    opentelemetry-cpp::trace
    opentelemetry-cpp::otlp_http_exporter
)
