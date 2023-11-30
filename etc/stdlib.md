# Compis Standard Library


## built-in functions

#### `fun Seq.len(this) uint`

Returns the current number of entries of a sequence.

Available for all slice and array types,
i.e. `[T]` `[T N]` `&[T N]` `&[T]` `mut&[T]` `mut&[T N]`.

The compiler transforms calls to `Seq.len` into either a constant
(for types of fixed size) or a simple memory load.


#### `fun Seq.cap(this) uint`

Returns the current number of entries of a sequence.

Available for all slice and array types,
i.e. `[T]` `[T N]` `&[T N]` `&[T]` `mut&[T]` `mut&[T N]`

The compiler transforms calls to `Seq.cap` into either a constant
(for types of fixed size) or a simple memory load.

Note that for types without a concept of dynamic capacity, like slices,
this function always returns the same value as `Seq.len`.


#### `fun [T].reserve(mut this, cap uint) bool`

Requests that the array has capacity for at least `cap` entries.
Returns `false` if memory could not be allocated.

In the case the array's memory need to grow, it will grow to a size as precise as possible to `cap`, subject to allocator alignment (`sizeof(uint)`.) I.e. this function will not try to be "clever" and allocate extra space beyond what is requested. If you expect to need more space soon, implement your own logic for reserving extra capacity up front.

```
fun example(a [int])
  if a.reserve(5)
    assert(a.cap >= 5)
```


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
