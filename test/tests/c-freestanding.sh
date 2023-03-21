# compile code without targeting any specific operating system.

for target in $(_co_targets); do
  IFS=, read -r arch sys ign <<< "$target"
  if [ "$sys" = none ]; then
    echo "building for $arch-$sys"
    CFLAGS=
    [ $arch = arm ] && CFLAGS="$CFLAGS -mfloat-abi=soft"
    cc -Os --target=$arch-$sys $CFLAGS -c add_ints.c -o add_ints.o
  fi
done
