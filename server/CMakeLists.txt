add_executable(querylab_server src/main.cpp)
target_link_libraries(querylab_server PRIVATE querylab_engine querylab_warnings Threads::Threads)
if(WIN32)
  target_link_libraries(querylab_server PRIVATE ws2_32)
  target_compile_definitions(querylab_server PRIVATE _WIN32_WINNT=0x0A00)
endif()

if(QUERYLAB_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME querylab_api_tests
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/api_test.py $<TARGET_FILE:querylab_server>)
  endif()
endif()
