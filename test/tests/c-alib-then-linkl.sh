# create object archive, then link executable with -L & -l
co cc -c hello.c -o hello.o
mkdir -p mylib
co ar rcs mylib/libhello.a hello.o
co cc -Lmylib -lhello -o hello.exe
./hello.exe
