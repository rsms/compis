target=${0:9:$(( ${#0} - 9 - 3 ))}  # len("c-target-") - len(".sh")
echo "building for $target"
cc --target=$target hello.c -o hello
