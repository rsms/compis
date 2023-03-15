# wasi doesn't support exceptions, so -fexceptions should cause an error
if c++ --target=wasm32-wasi hello.cc -fexceptions 2>&1; then
  _err "-fexceptions should have caused an error"
fi
