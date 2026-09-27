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

