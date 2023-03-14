# create object archive, then link executable
co cc -c hello.c -o hello.o
co ar rcs libhello.a hello.o
co cc libhello.a -o hello.exe
./hello.exe
