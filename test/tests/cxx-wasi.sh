co c++ --target=wasm32-wasi hello.cc -o hello.wasm

if command -v wasmtime >/dev/null; then
  wasmtime hello.wasm
else
  _warn "not running build product (wasmtime not found in PATH)"
fi
