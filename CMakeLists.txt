cmake_minimum_required(VERSION 3.20)

project(
    CacheLite
    VERSION 0.1.0
    DESCRIPTION "A high-concurrency in-memory key-value server with MySQL read-through caching"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CACHELITE_BUILD_TESTS "Build CacheLite tests" ON)
option(CACHELITE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

include(cmake/CacheLiteWarnings.cmake)
include(cmake/CacheLiteSanitizers.cmake)

add_library(cachelite_core STATIC
    src/base/BuildInfo.cpp
)

target_include_directories(cachelite_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_definitions(cachelite_core
    PRIVATE
        CACHELITE_VERSION="${PROJECT_VERSION}"
)

cachelite_set_warnings(cachelite_core)
cachelite_enable_sanitizers(cachelite_core)

add_executable(cachelite_server
    src/server/main.cpp
)

target_link_libraries(cachelite_server
    PRIVATE
        cachelite_core
)

cachelite_set_warnings(cachelite_server)
cachelite_enable_sanitizers(cachelite_server)

# The networking layer currently targets Linux/WSL because it uses
# POSIX sockets. Keep the base target buildable on other hosts.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(cachelite_net STATIC
        src/net/InetAddress.cpp
        src/net/Socket.cpp
    )

    target_include_directories(cachelite_net
        PUBLIC
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(cachelite_net
        PUBLIC
            cachelite_core
    )

    cachelite_set_warnings(cachelite_net)
    cachelite_enable_sanitizers(cachelite_net)

    add_executable(cachelite_blocking_echo
        src/server/blocking_echo_main.cpp
    )

    target_link_libraries(cachelite_blocking_echo
        PRIVATE
            cachelite_net
    )

    cachelite_set_warnings(cachelite_blocking_echo)
    cachelite_enable_sanitizers(cachelite_blocking_echo)
endif()

if(CACHELITE_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()

