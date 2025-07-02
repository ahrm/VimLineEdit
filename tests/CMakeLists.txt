add_executable(vim_lineedit_tests
  VimTestRunner.cpp
  ../VimLineEdit.cpp
)

target_link_libraries(vim_lineedit_tests Qt6::Widgets)
