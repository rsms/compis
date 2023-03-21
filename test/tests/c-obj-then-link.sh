# compile simple C program to object, then link executable
cc -c hello.c -o hello.o
cc hello.o -o hello.exe
./hello.exe
