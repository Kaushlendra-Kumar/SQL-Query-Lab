file(GLOB_RECURSE QUERYLAB_ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(querylab_engine STATIC ${QUERYLAB_ENGINE_SOURCES})
target_include_directories(querylab_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(querylab_engine PUBLIC querylab_thirdparty Threads::Threads PRIVATE querylab_warnings)

# Standalone command-line shell (no frontend / server needed).
if(NOT EMSCRIPTEN)
  add_executable(querylab_cli tools/querylab_cli.cpp)
  target_link_libraries(querylab_cli PRIVATE querylab_engine querylab_warnings)
endif()

if(QUERYLAB_BUILD_TESTS)
  enable_testing()
  file(GLOB QUERYLAB_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  add_executable(querylab_tests ${QUERYLAB_TEST_SOURCES})
  target_link_libraries(querylab_tests PRIVATE querylab_engine querylab_warnings)
  add_test(NAME querylab_tests COMMAND querylab_tests)
endif()

# Differential tests against SQLite (optional: built only when SQLite is found).
if(QUERYLAB_BUILD_TESTS)
  find_package(SQLite3 QUIET)
  if(SQLite3_FOUND)
    add_executable(querylab_difftest tests/differential/difftest_sqlite.cpp)
    target_link_libraries(querylab_difftest PRIVATE querylab_engine querylab_warnings SQLite::SQLite3)
    add_test(NAME querylab_difftest COMMAND querylab_difftest)
    message(STATUS "QueryLab: SQLite found, differential tests enabled")
  else()
    message(STATUS "QueryLab: SQLite not found, differential tests skipped")
  endif()
endif()
