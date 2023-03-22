cc --target=wasm32-wasi hello.c -o hello.wasm
wasi-run hello.wasm
