# wasi doesn't support exceptions, so -fexceptions should cause an error
echo "expecting: 'c++: error: wasi target does not support exceptions [-fexceptions]'"
if c++ --target=wasm32-wasi hello.cc -fexceptions 2>&1; then
  _err "-fexceptions should have caused an error"
fi
