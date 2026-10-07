add_executable(cachelite_base_test
    base/BuildInfoTest.cpp
)

target_link_libraries(cachelite_base_test
    PRIVATE
        cachelite_core
)

cachelite_set_warnings(cachelite_base_test)
cachelite_enable_sanitizers(cachelite_base_test)

add_test(
    NAME cachelite.base.build_info
    COMMAND cachelite_base_test
)

add_executable(cachelite_protocol_test
    protocol/ProtocolTest.cpp
)

target_link_libraries(cachelite_protocol_test
    PRIVATE
        cachelite_protocol
)

cachelite_set_warnings(cachelite_protocol_test)
cachelite_enable_sanitizers(cachelite_protocol_test)

add_test(
    NAME cachelite.protocol
    COMMAND cachelite_protocol_test
)

add_executable(cachelite_storage_test
    storage/MemoryStoreTest.cpp
)

target_link_libraries(cachelite_storage_test
    PRIVATE
        cachelite_storage
)

cachelite_set_warnings(cachelite_storage_test)
cachelite_enable_sanitizers(cachelite_storage_test)

add_test(
    NAME cachelite.storage.memory_store
    COMMAND cachelite_storage_test
)

add_executable(cachelite_database_test
    database/DatabaseTest.cpp
)

target_link_libraries(cachelite_database_test
    PRIVATE
        cachelite_database
)

cachelite_set_warnings(cachelite_database_test)
cachelite_enable_sanitizers(cachelite_database_test)

add_test(
    NAME cachelite.database
    COMMAND cachelite_database_test
)

if(CACHELITE_ENABLE_MYSQL)
    add_executable(cachelite_mysql_integration_test
        database/MySQLIntegrationTest.cpp
    )

    target_link_libraries(cachelite_mysql_integration_test
        PRIVATE
            cachelite_cache
            Threads::Threads
    )

    cachelite_set_warnings(cachelite_mysql_integration_test)
    cachelite_enable_sanitizers(cachelite_mysql_integration_test)

    add_test(
        NAME cachelite.database.mysql_live
        COMMAND cachelite_mysql_integration_test
    )
    set_tests_properties(
        cachelite.database.mysql_live
        PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 30
    )
endif()

add_executable(cachelite_command_test
    command/CommandExecutorTest.cpp
)

target_link_libraries(cachelite_command_test
    PRIVATE
        cachelite_command
)

cachelite_set_warnings(cachelite_command_test)
cachelite_enable_sanitizers(cachelite_command_test)

add_test(
    NAME cachelite.command_executor
    COMMAND cachelite_command_test
)

add_executable(cachelite_persistence_test
    persistence/AofLogTest.cpp
)

target_link_libraries(cachelite_persistence_test
    PRIVATE
        cachelite_persistence
)

cachelite_set_warnings(cachelite_persistence_test)
cachelite_enable_sanitizers(cachelite_persistence_test)

add_test(
    NAME cachelite.persistence.aof
    COMMAND cachelite_persistence_test
)

add_executable(cachelite_cache_concurrency_test
    cache/CacheServiceConcurrencyTest.cpp
)

target_link_libraries(cachelite_cache_concurrency_test
    PRIVATE
        cachelite_cache
)

cachelite_set_warnings(cachelite_cache_concurrency_test)
cachelite_enable_sanitizers(cachelite_cache_concurrency_test)

add_test(
    NAME cachelite.cache.concurrency
    COMMAND cachelite_cache_concurrency_test
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(cachelite_server_integration_test
        server/RespServerIntegrationTest.cpp
    )

    target_link_libraries(cachelite_server_integration_test
        PRIVATE
            Threads::Threads
    )

    cachelite_set_warnings(cachelite_server_integration_test)
    cachelite_enable_sanitizers(cachelite_server_integration_test)

    add_test(
        NAME cachelite.server.concurrent_clients
        COMMAND cachelite_server_integration_test
            $<TARGET_FILE:cachelite_resp_epoll>
    )
    set_tests_properties(
        cachelite.server.concurrent_clients
        PROPERTIES
            TIMEOUT 30
            RESOURCE_LOCK cachelite_resp_server_port
    )

    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME cachelite.server.benchmark_smoke
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/server/BenchmarkSmokeTest.py
                $<TARGET_FILE:cachelite_resp_epoll>
                ${PROJECT_SOURCE_DIR}/scripts/resp_benchmark.py
        )
        set_tests_properties(
            cachelite.server.benchmark_smoke
            PROPERTIES TIMEOUT 30
        )
    endif()
endif()

