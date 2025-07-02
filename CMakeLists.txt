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

add_test(NAME vim_lineedit_tests COMMAND vim_lineedit_tests)
