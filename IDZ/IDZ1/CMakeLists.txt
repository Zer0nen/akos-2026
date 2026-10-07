cmake_minimum_required(VERSION 3.27)
project(idz1 LANGUAGES C)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Open this project in Ubuntu/WSL.")
endif()

add_executable(gardeners src/main.c src/garden.c src/gardener.c)
set_target_properties(gardeners PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_definitions(gardeners PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(gardeners PRIVATE -Wall -Wextra -Wpedantic)

add_custom_target(run COMMAND "$<TARGET_FILE:gardeners>" data/demo.txt
    DEPENDS gardeners WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" USES_TERMINAL)
include(CTest)
if(BUILD_TESTING)
    foreach(scenario demo rows blocked isolated two_cells invalid interrupt)
        add_test(NAME garden.${scenario} COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh"
            "$<TARGET_FILE:gardeners>" ${scenario})
        set_tests_properties(garden.${scenario} PROPERTIES TIMEOUT 15)
    endforeach()
endif()
