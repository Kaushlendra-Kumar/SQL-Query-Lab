# Browser build of the engine (Emscripten only). Produces querylab.js + querylab.wasm,
# an ES module factory loaded by the frontend's Web Worker.
add_executable(querylab_wasm src/querylab_wasm.cpp)
target_link_libraries(querylab_wasm PRIVATE querylab_engine querylab_warnings)
set_target_properties(querylab_wasm PROPERTIES OUTPUT_NAME querylab SUFFIX ".js")
target_link_options(querylab_wasm PRIVATE
  -lembind
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createQueryLab
  -sENVIRONMENT=worker,node)
