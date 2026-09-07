option(AYUGRAM_ENABLE_COMPILATION_CACHE "Cache C/C++ compilation with sccache." OFF)

if (NOT AYUGRAM_ENABLE_COMPILATION_CACHE)
    return()
endif()

find_program(AYUGRAM_SCCACHE_EXECUTABLE NAMES sccache REQUIRED)

if (CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
    set(CMAKE_C_COMPILER_LAUNCHER "${AYUGRAM_SCCACHE_EXECUTABLE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${AYUGRAM_SCCACHE_EXECUTABLE}")
    set(CMAKE_OBJC_COMPILER_LAUNCHER "${AYUGRAM_SCCACHE_EXECUTABLE}")
    set(CMAKE_OBJCXX_COMPILER_LAUNCHER "${AYUGRAM_SCCACHE_EXECUTABLE}")
elseif (NOT CMAKE_GENERATOR STREQUAL "Xcode")
    message(FATAL_ERROR "Compilation caching requires Ninja, Makefiles, or Xcode.")
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang" OR CMAKE_GENERATOR STREQUAL "Xcode")
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:SHELL:-Xclang -fno-pch-timestamp>"
    )
endif()

function(ayugram_cache_targets directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach (target IN LISTS targets)
        get_target_property(target_type ${target} TYPE)
        if (target_type STREQUAL "INTERFACE_LIBRARY" OR target_type STREQUAL "UTILITY")
            continue()
        endif()

        if (CMAKE_GENERATOR STREQUAL "Xcode")
            set_target_properties(${target} PROPERTIES
                XCODE_ATTRIBUTE_C_COMPILER_LAUNCHER "${AYUGRAM_SCCACHE_EXECUTABLE}"
                XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES NO
                XCODE_ATTRIBUTE_COMPILER_INDEX_STORE_ENABLE NO
                XCODE_ATTRIBUTE_CLANG_USE_RESPONSE_FILE NO
                XCODE_ATTRIBUTE_OTHER_CFLAGS "$(inherited) -Xclang -fno-pch-timestamp"
                XCODE_ATTRIBUTE_OTHER_CPLUSPLUSFLAGS "$(inherited) -Xclang -fno-pch-timestamp"
            )
        elseif (MSVC)
            set_property(TARGET ${target} PROPERTY MSVC_DEBUG_INFORMATION_FORMAT Embedded)
            get_target_property(headers ${target} PRECOMPILE_HEADERS)
            if (headers)
                # sccache cannot cache MSVC PCH use; retain the headers as forced includes.
                set_property(TARGET ${target} PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
                foreach (header IN LISTS headers)
                    target_compile_options(${target} PRIVATE "$<$<BOOL:${header}>:/FI${header}>")
                endforeach()
            endif()
        endif()
    endforeach()

    get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach (subdirectory IN LISTS subdirectories)
        ayugram_cache_targets("${subdirectory}")
    endforeach()
endfunction()

if (MSVC OR CMAKE_GENERATOR STREQUAL "Xcode")
    cmake_language(DEFER CALL ayugram_cache_targets "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
