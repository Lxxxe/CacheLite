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
option(CACHELITE_ENABLE_MYSQL "Enable MySQL cache read-through" OFF)

include(cmake/CacheLiteWarnings.cmake)
include(cmake/CacheLiteSanitizers.cmake)

find_package(Threads REQUIRED)

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

add_library(cachelite_buffer STATIC
    src/net/Buffer.cpp
)

target_include_directories(cachelite_buffer
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

cachelite_set_warnings(cachelite_buffer)
cachelite_enable_sanitizers(cachelite_buffer)

add_library(cachelite_protocol STATIC
    src/protocol/RespEncoder.cpp
    src/protocol/RespParser.cpp
    src/protocol/RespValue.cpp
)

target_include_directories(cachelite_protocol
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(cachelite_protocol
    PUBLIC
        cachelite_buffer
)

cachelite_set_warnings(cachelite_protocol)
cachelite_enable_sanitizers(cachelite_protocol)

add_library(cachelite_storage STATIC
    src/storage/MemoryStore.cpp
)

target_include_directories(cachelite_storage
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

cachelite_set_warnings(cachelite_storage)
cachelite_enable_sanitizers(cachelite_storage)

add_library(cachelite_database STATIC
    src/database/MySQLConnectionPool.cpp
    src/database/MySQLRepository.cpp
)

target_include_directories(cachelite_database
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

if(CACHELITE_ENABLE_MYSQL)
    find_path(CACHELITE_MYSQL_INCLUDE_DIR
        NAMES mysql/mysql.h
    )
    find_library(CACHELITE_MYSQL_LIBRARY
        NAMES mysqlclient mariadb
    )

    if(NOT CACHELITE_MYSQL_INCLUDE_DIR OR
       NOT CACHELITE_MYSQL_LIBRARY)
        message(FATAL_ERROR
            "CACHELITE_ENABLE_MYSQL is ON, but MySQL client development files were not found"
        )
    endif()

    target_compile_definitions(cachelite_database
        PUBLIC
            CACHELITE_ENABLE_MYSQL=1
    )
    target_include_directories(cachelite_database
        PUBLIC
            ${CACHELITE_MYSQL_INCLUDE_DIR}
    )
    target_link_libraries(cachelite_database
        PUBLIC
            ${CACHELITE_MYSQL_LIBRARY}
    )
endif()

cachelite_set_warnings(cachelite_database)
cachelite_enable_sanitizers(cachelite_database)

add_library(cachelite_cache STATIC
    src/cache/CacheService.cpp
)

target_include_directories(cachelite_cache
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(cachelite_cache
    PUBLIC
        cachelite_database
        cachelite_storage
        Threads::Threads
)

cachelite_set_warnings(cachelite_cache)
cachelite_enable_sanitizers(cachelite_cache)

add_library(cachelite_command STATIC
    src/command/CommandExecutor.cpp
)

target_include_directories(cachelite_command
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(cachelite_command
    PUBLIC
        cachelite_cache
        cachelite_protocol
)

cachelite_set_warnings(cachelite_command)
cachelite_enable_sanitizers(cachelite_command)

add_library(cachelite_persistence STATIC
    src/persistence/AofLog.cpp
)

target_include_directories(cachelite_persistence
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(cachelite_persistence
    PUBLIC
        cachelite_command
)

cachelite_set_warnings(cachelite_persistence)
cachelite_enable_sanitizers(cachelite_persistence)

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
        src/net/EpollPoller.cpp
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
            cachelite_buffer
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

    add_executable(cachelite_epoll_echo
        src/server/epoll_echo_main.cpp
    )

    target_link_libraries(cachelite_epoll_echo
        PRIVATE
            cachelite_net
    )

    cachelite_set_warnings(cachelite_epoll_echo)
    cachelite_enable_sanitizers(cachelite_epoll_echo)

    add_executable(cachelite_resp_epoll
        src/server/resp_epoll_main.cpp
    )

    target_link_libraries(cachelite_resp_epoll
        PRIVATE
            cachelite_net
            cachelite_command
            cachelite_persistence
    )

    cachelite_set_warnings(cachelite_resp_epoll)
    cachelite_enable_sanitizers(cachelite_resp_epoll)
endif()

if(CACHELITE_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()

