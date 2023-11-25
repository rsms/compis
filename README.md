Compis is a contemporary systems programming language in the spirit of C.

The compiler supports writing programs in mixed Compis and C;
you can mix .c and .co source files. It accomplishes this by bundling some LLVM tools, like clang and lld into one executable. The LLVM dependency might go away someday.

"Compis" is a variation on the Swedish word "kompis", meaning "buddy" or "comrade." It's also [a nod to one of the first computers I used as a kid.](https://en.wikipedia.org/wiki/Compis)

> **Note:** Compis is under development

Usage:

```shell
$ compis build -o foo main.co
$ ./foo
```

Since Compis bundles clang & lld, you can use it as a C and C++ compiler
with cross-compilation capabilities:

```shell
$ compis cc hello.c -o hello
$ ./hello
Hello world
$ compis cc --target=aarch64-macos hello.c -o hello-mac
$ file hello-mac
hello-mac: Mach-O 64-bit executable arm64
$ compis c++ --target=wasm32-wasi hello.cc -o hello.wasm
$ wasmtime hello.wasm
Hello world
```


## Features

- Memory safety via ownership `let x *Thing = thing // move, not copy`
- Simple: primitive values, arrays, structs and functions.
- Convention over configuration: Sensible defaults. One way to do something.
- Compiler is a statically-linked, single executable.
- Hackable pragmatic compiler (parse → type-check → static analysis → codegen → link)



# Tour of the language


## Clarity

Compis attempts to make local reasoning about code as easy as possible.
There are no implicit type conversion, no side effects from language constructs
and the lifetime constraints of a value is explicitly encoded in its type.


## Memory safety

Compis manages the lifetime of values which are considered "owners."
In other words, memory allocation and deallocation is automatically managed.
There's an escape hatch ("unsafe" blocks & functions; unimplemented) for when you need precise
control.

A type is either considered "copyable" or "owning":

- Copyable types can be trivially copied without side effects.
  For example, all primitive types, like `int` and `bool`, are copyable.
  The lifetime of a copyable value is simply the lifetime of its owning variable,
  parameter or expression.

- Owning types have managed, linear lifetimes. They can only be
  copied—as in diverge, or "fork" if you will—by explicit user code.
  When a value of owning type ceases to exist it is "dropped";
  any `drop()` function defined for the type is called and if the type
  is stored in heap memory, it is freed.

Assigning a copyable value to variable (or using it as an rvalue expression in any situation, like passing it as an argument to a call) creates a distinct "deep" copy:

    type Vec2 { x, y int }
    type Line { start, end Vec2 }
    var Line a
    var b = a  // 'a' is copied
    b.start.x = 2
    assert(a.start.x == 0)

Assigning an owning value to a variable _moves_ the value; its previous owner becomes inaccessible and any attempt to use the old owner
causes a compile-time error.

    type Vec2 { x, y int }
    type Line { start, end Vec2 }
    // implementing "drop" for Vec2 makes is an "owning" type
    fun Vec2.drop(mut this) {}
    var Line a
    var b = a // 'a's value moves to 'b'
    b.start.x = 2
    a.start.x // error: 'a' has moved


### References

References are used for "lending" a value somewhere, without a change in storage or ownership.
Reference types are defined with a leading ampersand `&T`
and created with the `&` prefix operation: `&expr`.

    type Vec2 { x, y int }
    fun rightmost(a, b &Vec2) int {
      // 'a' and 'b' are read-only references here
      if a.x >= b.x { a.x } else { b.x }
    }
    fun main() {
      var a = Vec2(1, 1)
      var b = Vec2(2, 2)
      rightmost(&a, &b) // => 2
    }

Mutable reference types are denoted with the keyword "mut": `mut&T`.
Mutable references are useful when you want to allow a function to modify a value without copying the value or transferring its ownership back and forth.

    type Vec2 { x, y int }
    fun translate_x(v mut&Vec2, int delta) {
      v.x += delta
    }
    fun main() {
      var a = Vec2(1, 1)
      translate_x(&a, 2)  // lends a mutable reference to callee
      assert(a.x == 3)
    }

Compis does _not_ enforce exclusive mutable borrowing, like for example Rust does.
This makes Compis a little more forgiving and flexible at the expense of aliasing;
it is possible to have multiple pointers to a value which may change at any time:

    type Vec2 { x, y int }
    type Line { start, end mut&Vec2 }
    fun main() {
      var a = Vec2(1, 1)
      var line = Line(&a, &a)
      a.x = 2
      assert(line.start.x == 2)
      assert(line.end.x == 2)   // same value
    }


_This may change_ and Compis may introduce "borrow checking" or some version of it,
that enforces that no aliasing can occur when dealing with references. Mutable Value Semantics is another interesting idea on this topic.

References are semantically more similar to values than pointers: a reference used as an rvalue does not need to be "dereferenced" (but pointers do.)

    fun add(x int, y &int) int {
      let result = x + y  // no need to deref '*y' here
      assert(result == 2)
      return result
    }

The only situations where a reference needs to be "dereferenced" is when replacing a mutable reference to a copyable value with a new value:

    var a = 1
    var b mut&int = &a
    *b = 2   // must explicitly deref mutable refs in assignment
    assert(a == 2)


### Pointers

A pointer is an address to a value stored in long-term memory, "on the heap".
It's written as `*T`.
Pointers are "owned" types and have the same semantics as regular "owned" values, meaning their lifetime is managed by the compiler.
Pointers can never be "null". A pointer value which may or may not hold a value is made [optional](#optional), i.e. `?*T`.

> Planned feature: unmanaged "raw" pointer `rawptr T` which can only be created
> or dereferenced inside "unsafe" blocks.

```
type Vec2 { x, y int }
fun translate_x(v mut&Vec2, int delta) {
  v.x += delta
}
fun example(v *Vec2) {
  v.x = 1            // pointer types are mutable
  translate_x(&a, 2) // lends a mutable reference to callee
  assert(v.x == 1)
  // v's memory is freed here
}
```


### Ownership

In this example, `Thing` is considered an "owning" type since it has a "drop" function defined. Compis will, at compile time, make sure that there's exactly one owner of a "Thing" (that it is not copied.)

```
type Thing {
  x i32
}
fun Thing.drop(mut this) {
  print("Thing dropped")
}
fun example(thing Thing) i32 {
  return thing.x
} // "Thing dropped"
```

When the scope of an owning value ends that value is "dropped":

1. If the type is optional and empty, do nothing, else
2. If there's a "drop" type function defined for the type of value, that function is called to do any custom cleanup like closing a file descriptor.
3. If the value has subvalues that are owners, like a struct field, those are dropped.
4. If the value is heap-allocated, its memory is freed.


### Optional

Compis has _optional types_ `?T` rather than nullable types.

```co
fun example(x ?i32)
  // let y i32 = x  // error
  if x
    // type of x is "i32" here, not "?i32"
    let y i32 = x // ok
```

The compiler is clever enough to know when an optional value is
definitely available or not, based on prior tests:

```co
fun example(name ?str)
  if name && name.len() > 0
    print(name)

fun example2(a, b ?int) int
  if a && b { a * b } else 0
fun example3(a, b ?int) int
  if !(a && b) 0 else { a * b }
fun example4(a, b ?int) int
  if a && b { a * b } else
    a * 2  // error: a may be valid, but must be checked first
fun example5(a ?int) int
  if !a { a * 2 } // error: a is definitely empty
```

You can use variable definitions with `if` expressions, which is useful for taking some action on the result of a function call, without spamming your scope with temporary variables:

```co
fun try_read_file(path str) ?str

if let contents = try_read_file("/etc/passwd")
  // in here, the type of contents is str, not ?str
  print(contents)
```

In the above example, `contents` is only defined inside the "then" branch.
Accessing `contents` in an "else" branch or outside the "then" branch either
leads to an "undefined identifier" error or referencing some other variable in
the outer scope.


### No uninitialized memory

Memory in Compis is always initialized. When no initializer or initial value is provided, memory is zeroed. Therefore, all types in Compis are valid when their memory is all zero.

```co
type Vec3 { x, y, z f32 }
var v Vec3 // initialized to {0.0, 0.0, 0.0}
```


## Variables

Compis has variables `var` and one-time bindings `let`. `let` bindings are not variable, they can not be reassigned once defined to. The type can be omitted if a value is provided (the type is inferred from the value.) The value can be omitted for `var` if type is defined.

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



## Functions

Amazingly, Compis has functions!
Here are a few examples:

```co
fun mul_or_div(x, y int, mul bool) int
  if mul x * y
  else   x / y

fun no_args() int { return 42 }
fun no_args_implicit_return() int { 42 }
fun no_result(message str) { print(message) }
fun inferred_result(x, y f32) = x * y
let f = fun(x, y int) = x * y
```


### Type functions

Compis doesn't have classes, but it has something called "type functions" that allows
you to clearly associate functions with data:

```co
type Account
  id   uint
  name str

fun Account.isRoot(this) bool
  this.id == 0

fun Account.setName(mut this, newname str) str
  let prevname = .name
  .name = newname
  prevname

fun example()
  let account Account
  account.setName("anne")
```


### Using C ABI functions

To access a C ABI function you need to declare it in the compis source file
that accesses it using `pub "C" fun ...`:

`example/a.c`:

```c
long mul_or_div(long x, long y, _Bool mul) {
  return mul ? x*y : x/y;
}
```

`example/b.co`:

```co
pub "C" fun mul_or_div(x, y int, mul bool) int
fun example() int
  mul_or_div(1, 2, true)
```




## Arrays

An array in Compis is a representation of a region of linear memory,
interpreted as zero or more values of a uniform type.
Arrays have ownership of their values.

There are two kinds of arrays with very similar syntax:

- Fixed-size arrays `[type size]` (e.g. `[int 3]` for three ints)
- Dynamic arrays `[type]` which can grow at runtime

```co
var three_ints [int 3]
var some_bytes [u8]
three_ints[3] // compile error: out of bounds
some_bytes[3] // runtime panic: out of bounds
```

Fixed-size arrays are concretely represented simply by a pointer to the
underlying memory while dynamic arrays are represented by a triple
`(capacity, length, data_pointer)`.



## Slices

Slices are references to arrays. Like regular references, they can be mutable.

```co
var three_ints [int 3]
var a &[int]    = &three_ints
var b mut&[int] = &three_ints
```

Slices are concretely represented by a tuple `(length, data_pointer)`.

Note that you can make references to arrays while retaining compile-time size
information:




### Slices vs array references

References to fixed-size arrays are concretely represented by a pointer to the array's
underlying memory, rather than a slice construct:

```co
var four_bytes [u8 4]      // fixed-size array
var some_bytes [u8]        // dynamically-sized array
var a = &four_bytes        // mut&[u8 3] -- reference to array
var b = &some_bytes        // mut&[u8] -- slice of array
var c &[u8] = &four_bytes  // slice of array, from explicit type
```

When you don't specify the type, as in the case for `a` and `b` above,
Compis will chose the highest-fidelity type. For example, referencing a fixed-size array
yields a mutable array reference with the length encoded in the type.
This leads the most efficient machine code (just a pointer.)
However, you can specify a more narrow type or a compatible type of lower fidelity, as
in the case with `c` above.

To understand the practical impact, consider the two following functions,
one which takes a reference to an array and one which takes a slice:

```co
fun decode_u32le_array_ref(bytes &[u8 4]) u32
  u32(bytes[0]) |
  u32(bytes[1]) << 8 |
  u32(bytes[2]) << 16 |
  u32(bytes[3]) << 24

fun decode_u32le_slice(bytes &[u8]) u32
  u32(bytes[0]) |
  u32(bytes[1]) << 8 |
  u32(bytes[2]) << 16 |
  u32(bytes[3]) << 24
```

The equivalent generated C code looks like this:

```c
u32 decode_u32le_array_ref(const u8* bytes) {
  return (u32)bytes[0] |
         (u32)bytes[1] << 8u |
         (u32)bytes[2] << 16u |
         (u32)bytes[3] << 24u;
}

u32 decode_u32le_slice(struct { uint len; const u8* ptr; } bytes) {
  if (bytes.len <= 3) __co_panic_out_of_bounds(); // bounds check
  return (u32)bytes.ptr[0] |
         (u32)bytes.ptr[1] << 8u |
         (u32)bytes.ptr[2] << 16u |
         (u32)bytes.ptr[3] << 24u;
}
```

Notice how for the "slice" version, two machine words are passed as arguments
(length and address) instead of just one (address) as is the case with the
"array_ref" version. Additionally, a bounds check is performed at runtime in the
"slice" version.

> Note: Currently the actual generated code is a little less elegant for the "slice" version as there are bounds checks for every access, not just one for the largest offset. In the future the Compis code generator will be more clever and perform a minimum amount of runtime checks.



## Structures

Structures are used for organizing data.
A structure have named fields and can be nested.
Here's an example of declaring a named structure type:

```co
type Thing
  x, y f32
  size uint
  metadata          // anonymous struct
    debugname str
    is_hidden bool
```

They can be composed, and the order they are defined in does not matter:

```co
type Thing
  x, y f32
  size uint
  metadata Metadata

type OtherThing
  name     str
  metadata Metadata

type Metadata
  debugname str
  is_hidden bool
```



## Templates

Template types (generic types) are useful when describing shapes of data with
incomplete details.
They are also useful for reusing the same structures for many other types.
Here's an example where the same type `Vector` is used to implement two functions
that accept different types of primitive values (`int` and `f32`):

```co
type Vector<T>
  x, y T

fun example1(point Vector<int>) int
  point.x * point.y

fun example2(point Vector<f32>) f32
  point.x * point.y
```

Template parameters can define default values:

```co
type Foo<T,Offs=uint>
  value T
  offs  Offs

var a Foo<int>  // is really Foo<int,uint>
```

Templates can be partially expanded:

```co
type Foo<T,Offs=uint>
  value T
  offs  Offs

type Bar<T>
  small_foo Foo<T,u8>  // Foo partially expanded

var a Bar<int>  // Bar and Foo fully expanded
```


Currently only types can be templates, not yet functions, but I'll add that.
It is also not yet possible to define type functions for template types
(i.e. `fun Foo<T>.bar(this)` does not compile.)



## Packages

A package in Compis is a directory of files and identified by its file path.
Declarations in any file is visible in other files of the same package. For example:

```
$ cat foo/a.co
fun a(x int) int
  x * 2

$ cat foo/b.co
fun b(x int) int
  a(x) // the "a" function is visible here

$ compis build ./foo
[foo 6/6] build/opt-x86_64-linux/bin/foo
```

The only exception to this rule is imported symbols,
which are only visible within the same source file:

```
$ cat foo/a.co
import "std/runtime" { print }
// here, "print" is only available in this source file
fun say_hello()
  print("Hello")

$ cat foo/b.co
pub fun main()
  say_hello()    // ok; say_hello is available
  print("Hello") // error; print is not available here

$ compis build ./foo
foo/b.co:3:3: error: unknown identifier "print"
3 → │   print("Hello") // error; print is not available here
    │   ~~~~~
```

As a convenience, Compis can build "ad-hoc packages" out of arbitrary source files:

```shell
$ compis build somedir/myprog.co
[somedir/myprog 4/4] build/opt-x86_64-macos.10/bin/myprog
```


### Importing packages

```co
import "foo/bar"            // package available as "bar"
import "foo/bar" as lol     // package available as "lol"
import "./mysubpackage"     // relative to source file's directory
import "cat" { x, y }       // import only members x and y
import "cat" { x, y as y2 } // change name of some members
import "cat" as c { x, y }  // import both package and some members
import "html" { * }         // import all members
import "html" { *, a as A } // import all members, change name of some
```

List of members is a block which contains lists (comma separated names),
which allows us to write it in different ways depending on the amount of members
we are importing. This can help readability and improved version control diffs.
The following three are equivalent:

```co
import "fruit" { apple, banana, citrus, peach as not_a_plum }

import "fruit"
  apple
  banana
  citrus
  peach as not_a_plum

import "fruit" { apple,
  banana, citrus,
  peach as not_a_plum
}
```

Similarly—as a convenience—imports themselves can be grouped in a block:

```co
import
  "foo/bar"
  "foo/bar" as lol
  "./mysubpackage"
  "cat" { x, y as y2 }
  "html" { * }
  "fruit"
    apple
    banana
    citrus
    peach as not_a_plum

// equivalent to:
import "foo/bar"
import "foo/bar" as lol
import "./mysubpackage"
import "cat" { x, y as y2 }
import "html" { * }
import "fruit" { apple, banana, citrus, peach as not_a_plum }
```

When a package's namespace is imported and no explicit name is given with `as name`,
the package's identifier is inferred from its last path component:

```co
import "foo/abc"   // imported as identifier "abc"
import "foo/a b c" // imported as identifier "c"
import "foo/a-b-c" // imported as identifier "c"
```

If the last path component is not a valid identifier `as name` is required:

```co
import "foo/abc!"        // error: cannot infer package identifier
import "foo/abc!" as abc // imported as identifier "abc"
```

To import a package only for its side effects, without introducing any identifiers
into the source file scope, name it `_`:

```co
"test/check-that-random-works" as _
```

Examples of invalid import paths:

```co
import ""   // error: invalid import path (empty path)
import "/"  // error: invalid import path (absolute path)
import "."  // error: invalid import path (cannot import itself)
import " "  // error: invalid import path (leading whitespace)
import "a:" // error: invalid import path (invalid character)
```

Imports syntax specification:

```abnf
importstmt  = "import" (importgroup | importspec) ";"
importgroup = "{" (importspec ";")+ "}"
importspec  = posixpath ("as" id)? membergroup?
membergroup = "{" (memberlist ";")+ "}"
memberlist  = member ("," member)*
member      = "*" | id ("as" id)?
```


## Primitive types

Compis has the following primitive types:

```
TYPE   VALUES

void   nothing (can only be used as function-result type)
bool   false or true
i8     -128 … 127
i16    -32768 … 32767
i32    -2147483648 … 2147483647
i64    -9223372036854775808 … 9223372036854775807
u8     0 … 255
u16    0 … 65535
u32    0 … 4294967295
u64    0 … 18446744073709551615
int    Target-specific address size (i.e. i8, i16, i32 or i64)
uint   Target-specific address size (i.e. u8, u16, u32 or u64)
f32    -16777216 … 16777216 (integer precision)
f64    −9007199254740992 … 9007199254740992 (-2e53 … 2e53; integer precision)
```

Integers are represented as [two's complement](https://en.wikipedia.org/wiki/Two%27s_complement).
Floating-point numbers are represented as [IEEE 754](https://en.wikipedia.org/wiki/IEEE_754).
Overflow of signed integers causes a runtime panic.
Overflow of unsigned integers wrap.



## Integer literals

Compis features "ideally typed" integer literals.
The actual type is inferred on use and the literal is verified to be within range:

```
fun foo(v i32) void
fun bar(v u16) void
fun cat(v i8) void
fun example()
  let x = 200
  foo(x) // materialized as i32(320)
  bar(x) // materialized as u16(320)
  //cat(x) // error: 200 overflows i8
```



## Character literals

There's no special type for representing text runes in Compis.
Instead you deal with either `u32` for Unicode codepoints,
or `u8` for UTF-8 encoded sequences.

However Compis has syntax for character literals.
Character literals must be valid Unicode codepoints.

```
fun example()
  let _ = 'a'      // 0x61
  let _ = '\n'     // 0x0A
  let _ = '©'      // 0xA9 (encoded as UTF-8 C2 A9)
  let _ = '\u00A9' // 0xA9
  //let _ = '\xA9' // error: invalid UTF-8 sequence
  //let _ = ''     // error: empty character literal
```

If you feel the need to specify character values which are not Unicode codepoints,
use regular integers:

```
fun example()
  let _ = 0xA9
  let _ = 0xFF
  let _ = 0xFFFFFFFF
```



## Strings

Strings represent UTF-8 text. The type `str` is an alias of `&[u8]` (slice of bytes.)
String literals are high-fidelity types, embedding the size in the type (e.g. `&[u8 5]`).

```
fun foo(s str)
fun bar(s &[u8])
fun cat(s &[u8 5])
fun mak(s &[u8 6])
fun example()
  let s = "hello" // &[u8 5]
  foo(s)
  bar(s)
  cat(s)
  mak(s) // error: passing value of type &[u8 5] to parameter of type &[u8 6]
```



# Building

Build an optimized product:

    ./build.sh

Build a debug product:

    ./build.sh -debug

Run co:

    out/debug/co build -o out/hello examples/hello.c examples/foo.co
    out/hello

Build & run debug product in continuous mode:

    ./build.sh -debug -wf=examples/foo.co \
      -run='out/debug/co build examples/hello.c examples/foo.co && build/debug/main'

