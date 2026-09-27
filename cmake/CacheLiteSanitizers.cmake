function(cachelite_enable_sanitizers target_name)
    if(NOT CACHELITE_ENABLE_SANITIZERS)
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${target_name} PRIVATE
            -fsanitize=address,undefined
        )
    else()
        message(WARNING "Sanitizers are enabled only for GCC and Clang in this project")
    endif()
endfunction()

