cmake_minimum_required(VERSION 3.5)

project(VimLineEdit LANGUAGES CXX)

set(CMAKE_BUILD_TYPE Debug)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_PREFIX_PATH "/Volumes/my_external_ssd/qt69/")
find_package(Qt6 COMPONENTS Widgets REQUIRED)

add_executable(vim_lineedit
  main.cpp
  VimLineEdit.cpp
  VimLineEdit.h
)

target_link_libraries(vim_lineedit Qt6::Widgets)

# build the subproject in ./tests/CMakeLists.txt
add_subdirectory(tests)

# Enable testing
enable_testing()

file(GLOB_RECURSE ALL_DATA_FILES
     CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/test_generator/test_cases/*.keystrokes.txt"
)

# foreach (data_file IN LISTS ALL_DATA_FILES)
# # print the data file path
#   message(STATUS "Data file: ${data_file}")
# endforeach()


# add_test(NAME vim_lineedit_tests COMMAND vim_lineedit_tests)
# foreach(idx RANGE 0 8)

foreach(test_path IN LISTS ALL_DATA_FILES)
  # Extract filename from full path
  get_filename_component(filename ${test_path} NAME_WE)
  
  # Extract index from filename (test_case_{index})
  string(REGEX REPLACE "test_case_([0-9]+)" "\\1" test_index ${filename})
  # message(STATUS "Adding test for index: ${test_index}")
  
  add_test(
    NAME    my_test_idx_${test_index}
    COMMAND vim_lineedit_tests ${test_index}
  )
endforeach()

