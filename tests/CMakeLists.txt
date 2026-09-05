# SPDX-License-Identifier: Apache-2.0

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  FIND_PACKAGE_ARGS NAMES GTest
)
FetchContent_MakeAvailable(googletest)

add_library(libbtop_test)
target_include_directories(libbtop_test PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(libbtop_test libbtop GTest::gtest_main)

add_executable(btop_test cpu_names.cpp net_interfaces.cpp tools.cpp)
target_link_libraries(btop_test libbtop_test)

include(GoogleTest)
gtest_discover_tests(btop_test)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import pyte" RESULT_VARIABLE PYTE_MISSING ERROR_QUIET)
  if(NOT PYTE_MISSING)
    add_test(NAME net_pty_acceptance COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/pty_acceptance.py" $<TARGET_FILE:btop>)
    set_tests_properties(net_pty_acceptance PROPERTIES TIMEOUT 120)
  endif()
endif()
