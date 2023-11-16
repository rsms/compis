# Compis Standard Library

## std/builtin

This is a pseudo package


## std/runtime

- Source: `lib/std/runtime` (distribution: `$COROOT/std/runtime`)
- Automatically linked into all executables unless `--no-stdruntime` is set
- All top-level `pub` members of `std/runtime/runtime.co` are automatically in scope
  in all compis source files, unless `--no-stdruntime` is set.

It is totally valid to "override" ("shadow") runtime members:

```co
fun print(msg str)
  // your own implementation

pub fun main()
  // calls your print, not runtime.print
  print("hello world")
```

However, members explicitly imported from `std/runtime` causes a compile-time error
if overridden, just like with any other import:

```co
import "std/runtime" { print }

fun print(msg str)
  // your own implementation

pub fun main()
  print("hello world")
```

Output:

```
example.co:3:1: error: redefinition of "print"
3 → │ fun print(msg str)
    │ ~~~~~~~~~
example.co:1:24: help: "print" previously defined here
1 → │ import "std/runtime" { print }
    │                        ~~~~~
```
