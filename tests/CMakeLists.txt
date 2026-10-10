add_executable(vim_lineedit_tests
  VimTestRunner.cpp
  ../VimLineEdit.cpp
)

target_link_libraries(vim_lineedit_tests Qt6::Widgets)

add_executable(emacs_lineedit_tests EmacsTestRunner.cpp ../VimLineEdit.cpp)
target_link_libraries(emacs_lineedit_tests Qt6::Widgets)

find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(EMACS_EXECUTABLE NAMES emacs)
if(Python3_Interpreter_FOUND)
  set(EMACS_GENERATOR "${CMAKE_SOURCE_DIR}/test_generator/emacs_test_generator.py")
  file(GLOB EMACS_CASES CONFIGURE_DEPENDS
       "${CMAKE_SOURCE_DIR}/test_generator/emacs_test_cases/emacs_test_case_*.keystrokes.txt")
  foreach(fixture IN LISTS EMACS_CASES)
    get_filename_component(case_name "${fixture}" NAME_WE)
    add_test(NAME "${case_name}"
      COMMAND "${Python3_EXECUTABLE}" "${EMACS_GENERATOR}" compare "${fixture}"
              --runner $<TARGET_FILE:emacs_lineedit_tests>)
    set_tests_properties("${case_name}" PROPERTIES
      SKIP_RETURN_CODE 77 LABELS "emacs;compatibility" TIMEOUT 30)
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
