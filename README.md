Compis is a contemporary systems programming language in the spirit of C.

The compiler supports writing programs in mixed Compis and C;
you can mix .c and .co source files. It accomplishes this by bundling some LLVM tools, like clang and lld into one executable. The LLVM dependency might go away someday.

"Compis" is a variation on the Swedish word "kompis", meaning "buddy" or "comrade." It's also [a nod to one of the first computers I used as a kid.](https://en.wikipedia.org/wiki/Compis)

> **Note:** Compis is under development

Usage:

```shell
$ co build -o foo main.co
$ ./foo
```

## Features

- Memory safety via ownership `let x *Thing = thing // move, not copy`
- Simple: primitive values, arrays, structs and functions.
- Convention over configuration: Sensible defaults. One way to do something.
- Compiler is a statically-linked, single executable.
- Hackable pragmatic compiler (parse → type-check → static analysis → codegen → link)




# Tour of the language



## Memory safety

There are three kinds of pointer types:

- `*T` — owns value of T
- `&T`, `mut&T` — reference to immutable T and mutable T, respectively
- `raw_ptr` — user-managed pointer to memory (unsafe contexts only; not implemented)

### Memory ownership

```
type Thing {
  x i32
}
fun example(thing *Thing) i32 {
  return thing.x
} // thing loses ownership here, it is "dropped"
```

When the scope of an owning value ends that value is "dropped":

1. If the type is optional and empty, do nothing, else
2. If there's a "drop" type function defined for the type of value, that function is called to do any custom cleanup like closing a file descriptor.
3. If the value has subvalues that are owners, like a struct field, those are dropped.
4. If the value is heap-allocated, its memory is freed.

### Optional types

None of these can be `null`, instead Compis has "optional" types, `?T`

```co
fun example(x ?i32) {
  // let y i32 = x  // error
  if x {
    // type of x is "i32" here, not "?i32"
    let y i32 = x // ok
  }
}
```

### No uninitialized memory

Memory in Compis is always initialized. When no initializer or initial value is provided, memory is zeroed. Therefore, all types in Compis are valid when their memory is all zero.

```co
type Vec3 { x, y, z f32 }
var v Vec3 // initialized to {0.0, 0.0, 0.0}
```


## Variables

There are variables `var` and one-time bindings `let`. `let` is not variable, it can not be reassigned once assigned to. The type can be omitted if a value is provided (the type is inferred from the value.) The value can be omitted for `var` if type is defined.

```co
var b i32 = 1 // a 32-bit signed integer
var a = 1     // type inferred as "int"
a = 2         // update value of a to 2
var c u8      // zero initialized
let d i64 = 1 // a 64-bit signed integer
let e = 1     // type inferred as "int"
e = 2         // error: cannot assign to binding
let f i8      // error: missing value
```


> FUTURE: introduce a `const` type for immutable compile-time constants

> FUTURE: support deferred binding, e.g. `let x i8; x = 8`




# Building

First time setup:

    ./init.sh

Build & test:

    ./build.sh -debug
    out/debug/co build -o out/hello examples/hello.c examples/foo.co
    out/hello

Build & run in continuous mode:

    ./build.sh -wf=examples/foo.co \
      -run='out/debug/co build examples/hello.c examples/foo.co && ./a.out'


## LLVM

By default llvm & clang is built in release mode with assertions.
There's a small but noticeable perf hit introduced by assertions.
You can build llvm without them like this:

    etc/llvm/build-llvm.sh -force -no-assertions

You can also customize llvm build mode.
Available modes: Debug, Release, RelWithDebInfo and MinSizeRel (default)

    etc/llvm/build-llvm.sh -force -mode RelWithDebInfo

Note: These flags can also be passed to `./init.sh`.
