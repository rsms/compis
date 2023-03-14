# compile simple C program to object, then link executable
co cc -c hello.c -o hello.o
co cc hello.o -o hello.exe
./hello.exe
