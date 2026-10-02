cmake_minimum_required(VERSION 3.16)
project(esphome_x2d_checks LANGUAGES CXX)
set(X2D_CORE_DIR "" CACHE PATH "Checkout of the component's pinned x2d-core commit")
if(NOT EXISTS "${X2D_CORE_DIR}/src/controller.h" OR NOT EXISTS "${X2D_CORE_DIR}/tests/check_controller.cpp")
  message(FATAL_ERROR "Set X2D_CORE_DIR to the pinned x2d-core checkout")
endif()
enable_testing()
# Reuse the core's checks against the component's pinned commit.
foreach(source IN ITEMS "${X2D_CORE_DIR}/tests/check_controller.cpp"
                        "${X2D_CORE_DIR}/tests/check_radio_runtime.cpp"
                        "${X2D_CORE_DIR}/tests/check_cc1101.cpp"
                        tests/check_radio_encoder.cpp
                        tests/check_storage.cpp)
  get_filename_component(check "${source}" NAME_WE)
  add_executable(${check} "${source}")
  target_compile_features(${check} PRIVATE cxx_std_17)
  target_include_directories(${check} PRIVATE components/x2d "${X2D_CORE_DIR}/src")
  target_compile_options(${check} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  if(check STREQUAL "check_storage")
    target_include_directories(${check} PRIVATE tests/idf_stubs)
    target_compile_definitions(${check} PRIVATE X2D_JOURNAL_ADDRESS=0x3E0000)
  endif()
  add_test(NAME ${check} COMMAND ${check})
endforeach()
