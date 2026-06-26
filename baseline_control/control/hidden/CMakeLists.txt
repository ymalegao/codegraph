cmake_minimum_required(VERSION 3.16)
project(request_scheduler_hidden_tests LANGUAGES CXX)

if(NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "FIXTURE_DIR is required")
endif()

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${FIXTURE_DIR}" fixture-build)

set(CMAKE_CXX_STANDARD 17)
foreach(test_name
        invariant_matrix_hidden_test
        batch_limit_hidden_test
        unlimited_batch_hidden_test
        background_deferral_hidden_test
        regression_hidden_test)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE request_scheduler)
    target_include_directories(${test_name} PRIVATE "${FIXTURE_DIR}/tests")
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
enable_testing()
