c++ --target=wasm32-wasi hello.cc -o hello.wasm
wasi-run hello.wasm
