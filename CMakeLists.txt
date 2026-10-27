cmake_minimum_required(VERSION 3.16)

project(data_upload_scan VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

add_executable(data_upload_scan
    src/main.c
    src/tf_scan.c
)

target_include_directories(data_upload_scan PRIVATE include)

target_compile_definitions(data_upload_scan PRIVATE
    _FILE_OFFSET_BITS=64
    PROGRAM_VERSION="${PROJECT_VERSION}"
)

target_compile_options(data_upload_scan PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wformat=2
    -Werror=return-type
)

install(TARGETS data_upload_scan RUNTIME DESTINATION bin)

add_executable(data_upload_demo
    src/upload_main.c
    src/tf_scan.c
    src/oss_config.c
    src/oss_upload.c
)

target_include_directories(data_upload_demo PRIVATE include)

target_compile_definitions(data_upload_demo PRIVATE
    _FILE_OFFSET_BITS=64
    PROGRAM_VERSION="${PROJECT_VERSION}"
)

target_compile_options(data_upload_demo PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wformat=2
    -Werror=return-type
)

install(TARGETS data_upload_demo RUNTIME DESTINATION bin)
