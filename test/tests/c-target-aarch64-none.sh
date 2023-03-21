# compile code without targeting any specific operating system
target=${0:9:$(( ${#0} - 9 - 3 ))}  # len("c-target-") - len(".sh")
echo "building for $target"

if [[ $target == arm-* ]]; then
  cc -Os --target=$target -mfloat-abi=soft -c add_ints.c -o add_ints.o
else
  cc -Os --target=$target -c add_ints.c -o add_ints.o
fi
