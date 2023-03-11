Example of WebAssembly program for the [WASI platform](https://wasi.dev)

    compis cc --target=wasm32-wasi hello.c -o hello.wasm

If you have a wasi runtime installed,
like [wasmtime](https://github.com/bytecodealliance/wasmtime),
you can run it like this:

    wasmtime hello.wasm

You can also run it in a web browser:

    python3 -m http.server -b localhost 8123
