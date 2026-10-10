add_executable(vim_lineedit_tests
  VimTestRunner.cpp
  ../VimLineEdit.cpp
)

target_link_libraries(vim_lineedit_tests Qt6::Widgets)

add_executable(emacs_lineedit_tests EmacsTestRunner.cpp ../VimLineEdit.cpp)
target_link_libraries(emacs_lineedit_tests Qt6::Widgets)

add_executable(emacs_widget_tests EmacsWidgetTests.cpp ../VimLineEdit.cpp)
target_link_libraries(emacs_widget_tests Qt6::Widgets)
add_test(NAME emacs_widget_tests COMMAND emacs_widget_tests)
set_tests_properties(emacs_widget_tests PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "emacs" TIMEOUT 30)

find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(EMACS_EXECUTABLE NAMES emacs)
if(Python3_Interpreter_FOUND)
  set(EMACS_GENERATOR "${CMAKE_SOURCE_DIR}/test_generator/emacs_test_generator.py")
  file(GLOB EMACS_CASES CONFIGURE_DEPENDS
       "${CMAKE_SOURCE_DIR}/test_generator/emacs_test_cases/emacs_test_case_*.keystrokes.txt")
  # Fixtures whose setup and edits fit in a single-line widget.
  set(EMACS_LINE_CASES 0 2 3 4 6 8 9 10 11 12 13 14 15 17 18 19 21 22 23 24 25 26 27 28 29 30)
  foreach(fixture IN LISTS EMACS_CASES)
    get_filename_component(case_name "${fixture}" NAME_WE)
    add_test(NAME "${case_name}"
      COMMAND "${Python3_EXECUTABLE}" "${EMACS_GENERATOR}" compare "${fixture}"
              --runner $<TARGET_FILE:emacs_lineedit_tests>)
    set_tests_properties("${case_name}" PROPERTIES
      LABELS "emacs;compatibility" TIMEOUT 30)
    string(REPLACE "emacs_test_case_" "" case_index "${case_name}")
    if(case_index IN_LIST EMACS_LINE_CASES)
      add_test(NAME "${case_name}_line"
        COMMAND "${Python3_EXECUTABLE}" "${EMACS_GENERATOR}" compare "${fixture}"
                --runner $<TARGET_FILE:emacs_lineedit_tests> --widget line)
      set_tests_properties("${case_name}_line" PROPERTIES LABELS "emacs;compatibility" TIMEOUT 30)
    endif()
    if(case_name MATCHES "^emacs_test_case_[012]$")
      add_test(NAME "emacs_infrastructure_${case_name}"
        COMMAND "${Python3_EXECUTABLE}" "${EMACS_GENERATOR}" compare "${fixture}"
                --runner $<TARGET_FILE:emacs_lineedit_tests> --mode standard)
      set_tests_properties("emacs_infrastructure_${case_name}" PROPERTIES
        LABELS "emacs;infrastructure" TIMEOUT 30)
    endif()
  endforeach()
  add_test(NAME emacs_infrastructure_line_widget
    COMMAND "${Python3_EXECUTABLE}" "${EMACS_GENERATOR}" compare
            "${CMAKE_SOURCE_DIR}/test_generator/emacs_test_cases/emacs_test_case_0.keystrokes.txt"
            --runner $<TARGET_FILE:emacs_lineedit_tests> --mode standard --widget line)
  set_tests_properties(emacs_infrastructure_line_widget PROPERTIES
    LABELS "emacs;infrastructure" TIMEOUT 30)
  if(EMACS_EXECUTABLE)
    add_test(NAME emacs_reference_verify
      COMMAND "${Python3_EXECUTABLE}" "${EMACS_GENERATOR}" verify --emacs "${EMACS_EXECUTABLE}")
    set_tests_properties(emacs_reference_verify PROPERTIES LABELS "emacs;reference" TIMEOUT 120)
    add_test(NAME emacs_harness_checks
      COMMAND "${CMAKE_COMMAND}" -E env
              "REFERENCE_EMACS=${EMACS_EXECUTABLE}"
              "REFERENCE_QT_RUNNER=$<TARGET_FILE:emacs_lineedit_tests>"
              "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_emacs_infrastructure.py")
    set_tests_properties(emacs_harness_checks PROPERTIES LABELS "emacs;infrastructure" TIMEOUT 60)
  endif()
else()
  message(STATUS "Python3 not found: Emacs fixture tests are not registered")
endif()
