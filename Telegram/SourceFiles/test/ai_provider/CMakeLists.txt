cmake_minimum_required(VERSION 3.21)
project(ayu_ai_provider_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)
get_filename_component(source_files "../.." ABSOLUTE)

add_executable(ayu_ai_provider_tests
    ai_client_tests.cpp
    ${source_files}/ayu/features/ai/ai_client.cpp
)
target_include_directories(ayu_ai_provider_tests PRIVATE
    ${source_files}
    ${source_files}/../lib_base
    ${source_files}/../ThirdParty/GSL/include
)
target_link_libraries(ayu_ai_provider_tests PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Network
)
if(MSVC)
    target_compile_options(ayu_ai_provider_tests PRIVATE /utf-8 /permissive-)
endif()
enable_testing()
add_test(NAME ai_provider COMMAND ayu_ai_provider_tests)
add_test(NAME ai_provider_timeout COMMAND ayu_ai_provider_tests --timeout)
set_tests_properties(ai_provider_timeout PROPERTIES TIMEOUT 70)
