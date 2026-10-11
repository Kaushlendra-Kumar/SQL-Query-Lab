file(GLOB_RECURSE QUERYLAB_ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(querylab_engine STATIC ${QUERYLAB_ENGINE_SOURCES})
target_include_directories(querylab_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(querylab_engine PUBLIC querylab_thirdparty Threads::Threads PRIVATE querylab_warnings)

# Standalone command-line shell (no frontend / server needed).
add_executable(querylab_cli tools/querylab_cli.cpp)
target_link_libraries(querylab_cli PRIVATE querylab_engine querylab_warnings)

if(QUERYLAB_BUILD_TESTS)
  enable_testing()
  file(GLOB QUERYLAB_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  add_executable(querylab_tests ${QUERYLAB_TEST_SOURCES})
  target_link_libraries(querylab_tests PRIVATE querylab_engine querylab_warnings)
  add_test(NAME querylab_tests COMMAND querylab_tests)
endif()
