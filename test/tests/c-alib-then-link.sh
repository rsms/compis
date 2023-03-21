# create object archive, then link executable
cc -c hello.c -o hello.o
ar rcs libhello.a hello.o
cc libhello.a -o hello.exe
./hello.exe
