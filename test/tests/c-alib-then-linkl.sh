# create object archive, then link executable with -L & -l
cc -c hello.c -o hello.o
mkdir -p mylib
ar rcs mylib/libhello.a hello.o
cc -Lmylib -lhello -o hello.exe
./hello.exe
