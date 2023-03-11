Example of freestanding WebAssembly program running in a web browser

    compis cc --target=wasm32-none hello.c -o hello.wasm
    python3 -m http.server -b localhost 8123
