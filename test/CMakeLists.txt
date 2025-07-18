# SPDX-License-Identifier: MIT
find_package(cmocka 1.1.5 REQUIRED)
find_package(cmocka_extensions REQUIRED)

function(create_unit_test)
  cmake_parse_arguments(PARSED_ARGS "" "NAME" "SOURCES;INCLUDES;LIBRARIES;DEFINITIONS;WRAPS" ${ARGN})
  get_filename_component(UNIT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
  get_filename_component(UNIT_NAME ${UNIT_DIR} NAME)

  message(STATUS "Create unit test ${PARSED_ARGS_NAME} for ${UNIT_NAME}")
  add_executable(${PARSED_ARGS_NAME} ${PARSED_ARGS_SOURCES})

  target_link_libraries(
    ${PARSED_ARGS_NAME}
    PRIVATE
    ${PARSED_ARGS_WRAPS}
    ${PARSED_ARGS_LIBRARIES}
    ${CMOCKA_LIBRARIES}
  )

  target_include_directories(
    ${PARSED_ARGS_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/test/
    ${PROJECT_SOURCE_DIR}/inc/
    ${PARSED_ARGS_INCLUDES}
    ${CMOCKA_INCLUDE_DIR}
  )

  if(PARSED_ARGS_DEFINITIONS)
    target_compile_definitions(${PARSED_ARGS_NAME} PRIVATE ${PARSED_ARGS_DEFINITIONS})
  endif()

  add_test(NAME ${PARSED_ARGS_NAME} COMMAND ${UNIT_TEST_INTERPRETER} ${PARSED_ARGS_NAME})

  install(
    TARGETS ${PARSED_ARGS_NAME} DESTINATION
    ${CMAKE_INSTALL_LIBDIR}/test/${PROJECT_NAME}/${UNIT_NAME}
  )
endfunction()

add_subdirectory(mocks)
file(GLOB utests RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} utest-*)

foreach(utest ${utests})
  add_subdirectory(${utest})
endforeach()
