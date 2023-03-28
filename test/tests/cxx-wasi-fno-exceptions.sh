# wasi doesn't support exceptions, so -fno-exceptions should have no effect

c++ --target=wasm32-wasi hello.cc -o a.wasm
c++ --target=wasm32-wasi hello.cc -o b.wasm -fno-exceptions

[ "$(_sha256 a.wasm)" = "$(_sha256 b.wasm)" ] ||
  _err "-fno-exceptions caused different build results"
