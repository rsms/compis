# makes sure that the cc implementation properly detects "c++",
# even when the path is absolute or using the alternate "clang++" alias.

"$COPATH/c++" -c hello.cc

ln -s "$COEXE" clang++
"$PWD/clang++" -c hello.cc
