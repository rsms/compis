# compile simple C program directly from source
# also, use "co cc" instead of "cc" to cover testing non-multicall invocation
co cc hello.c -o hello.exe
./hello.exe
