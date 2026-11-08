# The Sosu Programming Language

A complete guide, for Sosu 0.4.

## Preface

Sosu is a native, low-level language built around one idea: **you should know what a function does by reading its signature.** Its effects (`uses io`), its failures (`T!E`) and its allocations (an `Arena*` parameter) are written there, and the compiler checks them. Its compiler, `sosuc`, is written in C99, writes C, and calls the C compiler of the system, so a Sosu program runs wherever C runs.

This guide covers the whole language and its tools, chapter by chapter. If you want to write a first program tonight, read the [quick guide](QUICK_GUIDE.md) first, and come back here when you need the details. The [specification](SPEC.md) remains the reference for the edge cases, the [library reference](REFERENCE.md) lists every public function of `core`, `std` and the packages, and [errors.md](errors.md) lists every error code.

### Who this guide is for

Programmers who know C, C++, C# or Java. It does not teach programming. It explains what Sosu does, why, and what it costs, with examples you can run.

### How to read it

| Part | Chapters | Read it when |
|---|---|---|
| I. The Language | 1 to 7 | first, in order |
| II. Data | 8 to 11 | before writing your own types |
| III. Memory | 12 to 15 | before writing a program that keeps data around |
| IV. Abstraction | 16 to 18 | when you write libraries |
| V. Programs | 19 to 23 | when a program grows past one file |
| VI. Platforms | 24 to 26 | for windows, C libraries and releases |
| Appendices | A to C | as a reference |

Every program in this guide is compiled and run by the tests of the repository: a block followed by an output block prints exactly that output, and a block said to be refused is refused with the code given. If an example here does not work for you, it is a bug: please report it.

### Conventions

Code is in `monospace`. Boxes mark three kinds of remarks:

> **Tip:** a shorter or better way to do something.

> **Note:** a detail worth knowing, which you can skip at first.

> **Warning:** a place where a program can go wrong without the compiler saying so.

### The principles behind the language

Seven choices explain most of what follows:

1. **Reading comes before writing.** A longer signature is accepted if it saves opening the body.
2. **Nothing is hidden.** No implicit conversion, no overloading, no macros, no exceptions, no hidden allocation, no hidden control flow.
3. **Failure and absence are types.** No null pointer and no exception: `T?` and `T!E`.
4. **Memory is explicit.** No garbage collector, no reference counting, no global allocator: arenas.
5. **Effects are checked.** A function that prints, reads the clock or starts a thread says so.
6. **Simple signatures come before complete guarantees.** There are no lifetimes in the types, so the compiler catches many dangling pointers, not all of them.
7. **It runs where C runs.** The cost is a C compiler on the machine that builds.

---

# Part I. The Language

## 1. Getting Started

### 1.1 Installing

The [releases](https://github.com/Clement-Bonet/Sosu/releases) have `sosuc` for Windows (x64), Linux (x64 and ARM64) and macOS (universal), with the standard library (`lib/`) and the packages (`packages/`). Unpack the archive and add its `bin/` folder to your `PATH`.

`sosuc` needs a C compiler to produce programs:

| System | C compiler |
|---|---|
| Windows | MSVC (Visual Studio or its Build Tools); `sosuc` finds it by itself |
| Linux | `cc`: gcc or clang |
| macOS | clang, from the Xcode Command Line Tools |

```bash
sosuc --version
```

`sosuc` finds `core` and `std` in `$SOSU_HOME/lib`, or else in the `lib/` folder next to its `bin/`.

### 1.2 A package

Every program is a *package*: a folder with a `sosu.toml` manifest and a `src/` folder.

```text
hello/
  sosu.toml
  src/
    main.sosu
```

```toml
[package]
name = "hello"
version = "0.1.0"
```

`src/main.sosu` makes it a program; `src/lib.sosu` would make it a library.

<!-- run -->
```csharp
import std.io;

i32 main() uses io {
    io.print("Hello, {}!", "world");
    return 0;
}
```

```text
Hello, world!
```

From the `hello` folder:

```bash
sosuc run
```

`main` returns the exit code of the program. `uses io` declares that it writes to the console: remove it, and the program is refused.

### 1.3 The commands

| Command | Effect |
|---|---|
| `sosuc check [folder]` | checks the package without compiling it |
| `sosuc run [folder] [-- args]` | compiles and runs it, with arguments after `--` |
| `sosuc build [folder]` | compiles it into `build/` |
| `sosuc build --lib` / `--lib=shared` | compiles a library for other languages (chapter 25) |
| `sosuc test [folder]` | runs its `test` blocks; `--bench` measures its benchmarks |
| `sosuc fmt [files or folders]` | formats in place; `--check` only reports |
| `sosuc package [folder]` | makes a release folder and archive; `--target android` or `ios-sim` |
| `sosuc bindgen header.h -o file.sosu` | writes Sosu declarations from a C header |
| `sosuc i18n extract` | writes the template of the translations of a package |
| `sosuc lsp` | runs the language server, for editors |

Options of `build`, `run` and `test`:

| Option | Effect |
|---|---|
| `--release` | optimized C; no overflow checks, no contracts |
| `--checks=on\|off` | index checks; on by default, in release too |
| `--contracts=check\|off` | `requires` and `ensures` |
| `--target os-arch` | another system or architecture (chapter 26) |
| `--format=json` | diagnostics as JSON, for tools |

### 1.4 What happens when you build

`sosuc` reads the package and its dependencies, checks them, writes one C file to `build/`, and calls the C compiler. The C it writes keeps `#line` directives, so a debugger and the C compiler point to your Sosu lines. You never edit the C, and its form is not part of the compatibility promises.

### 1.5 Editors

`sosuc lsp` speaks the Language Server Protocol: errors as you type, hover, go to definition and completion. It reads what is on the disk and fetches nothing: after cloning a project with git dependencies, run `sosuc check` once in a terminal, or the editor reports them as not fetched (E0405).

## 2. Lexical Structure

### 2.1 Source files

Source files are UTF-8, and may start with a byte order mark. Names and symbols are ASCII; other characters appear only in strings, characters and comments.

```csharp
// a line comment
/* a block comment, which does not nest */
```

### 2.2 Names

A name is `[A-Za-z_][A-Za-z0-9_]*`. The convention, which `sosuc fmt` does not enforce:

| Kind | Style | Example |
|---|---|---|
| types, enum variants, interfaces | PascalCase | `TextBuffer`, `NotFound` |
| functions, methods, variables, modules | snake_case | `read_file`, `line_count` |
| constants | UPPER_SNAKE_CASE | `MAX_PLAYERS` |

### 2.3 Keywords

```text
as auto base break class continue defer else enum ensures extern false for if import in
interface match mut override pub requires return self struct test true try unsafe uses virtual while
```

`comptime` is reserved. `some`, `none`, `ok` and `err` are predefined, not reserved. `fn(` starts a function type, and `keeps` is a word only before a parameter or after the parameters of a method. `window`, `component`, `style` and `theme` are words only in `.ui.sosu` files.

### 2.4 Literals

| Literal | Examples | Notes |
|---|---|---|
| decimal integer | `42`, `1000000` | cannot start with `0`: `007` is an error, Sosu has no octal |
| hexadecimal | `0xFF`, `0x4000004F` | lowercase prefix |
| binary | `0b1010` | |
| float | `3.14`, `1e5`, `2.5e-3` | a digit after the dot, so `0..n` is a range |
| character | `'a'`, `'\n'`, `'\u{e9}'` | one code point |
| string | `"text"` | one line, valid UTF-8 |
| boolean | `true`, `false` | |

A literal has no type suffix: `300ui8` is an error, write `300 as ui8`.

Escapes, in strings and characters: `\n \t \r \0 \\ \' \"`, `\xNN` (ASCII only, `\x00` to `\x7F`, so that a string stays valid UTF-8), and `\u{N}` (one to six hexadecimal digits).

<!-- run -->
```csharp
import std.io;

i32 main() uses io {
    io.print("tab:\t| quote: \" | e acute: \u{e9}");
    io.print("{} {} {}", 0xFF, 0b1010, 2.5e2);
    return 0;
}
```

```text
tab:	| quote: " | e acute: é
255 10 250.0
```

## 3. Types and Values

### 3.1 The types at a glance

| Category | Types |
|---|---|
| integers | `i8 i16 i32 i64`, `ui8 ui16 ui32 ui64`, `isize usize` |
| floats | `f32 f64`, and `uf32 uf64`, never negative |
| others | `bool`, `char` (a Unicode code point, 32 bits), `void` |
| text | `str`: valid UTF-8, a pointer and a length, not owned |
| pointer | `T*`, never null; `T*?` if it may be absent |
| option | `T?`: `some(x)` or `none` |
| result | `T!E`: `ok(x)` or `err(e)` |
| array | `T[N]`, fixed size, stored in place |
| view | `T[]`: a pointer and a length |
| function | `fn(i32) -> i32 uses io` |
| your own | `struct`, `enum`, `class`, `interface`, generic or not |
| memory | `Arena` |

### 3.2 Variables

The type comes before the name. A variable gets its value where it is declared: `i32 x;` is an error. It cannot change unless it is `mut`.

<!-- run -->
```csharp
import std.io;

i32 main() uses io {
    i32 width = 80;
    mut i32 height = 24;
    height += 1;
    auto area = width * height;  // auto: the type of the value, i32 here
    io.print("{}", area);
    return 0;
}
```

```text
2000
```

There are no global variables. A module may hold constants, whose value is known when compiling:

<!-- check -->
```csharp
pub i32 MAX_PLAYERS = 4;
pub ui32 FLAGS = 0x20;
str NAME = "board";
i32 CELLS = MAX_PLAYERS * 16;

struct Board {
    i32[CELLS] cells;  // a constant gives the size of an array
}
```

### 3.3 Integers

| Type | Size | Range |
|---|---|---|
| `i8`, `ui8` | 8 bits | −128 to 127, 0 to 255 |
| `i16`, `ui16` | 16 bits | −32 768 to 32 767, 0 to 65 535 |
| `i32`, `ui32` | 32 bits | about ±2.1 billion, 0 to about 4.3 billion |
| `i64`, `ui64` | 64 bits | about ±9.2 × 10¹⁸, 0 to about 1.8 × 10¹⁹ |
| `isize`, `usize` | the size of a pointer | sizes, lengths and indexes are `usize` |

A literal takes the type that is expected: in `i64 x = 3;`, `3` is an `i64`; in `a + 1` with an `a` of type `ui8`, `1` is a `ui8`. Without an expected type, an integer literal is an `i32`. A literal that does not fit is an error.

**Overflow is never undefined.** In a debug build, an overflow stops the program with its position and the exit code 101. In a release build, the arithmetic wraps around. When wrapping is what you want, say so:

<!-- run -->
```csharp
import std.io;

i32 main() uses io {
    ui8 x = 250;
    io.print("{}", wrapping_add(x, 10));
    io.print("{}", wrapping_mul(16 as ui8, 17));
    return 0;
}
```

```text
4
16
```

Division by zero always stops the program, in release too. `%` works on integers only. `>>` is arithmetic on signed integers and logical on unsigned ones.

### 3.4 Floats

`f32` and `f64` are IEEE floats. A float literal is an `f64` unless a type is expected, and an integer literal is never a float: write `3.0`. A float prints with a dot: `25.0`.

`uf32` and `uf64` are floats that are never negative: a size, a distance, a duration. A negative result is checked like an integer overflow. `-x` on an unsigned float is refused, and `as` converts to and from the signed ones.

### 3.5 Conversions

Nothing converts by itself. Both operands of an operator have the same type, and `as` converts:

<!-- run -->
```csharp
import std.io;

i32 main() uses io {
    i32 count = 7;
    i64 total = 1000;
    f64 mean = total as f64 / count as f64;
    io.print("{}", total + count as i64);
    io.print("{}", mean > 142.8 && mean < 142.9);
    io.print("{}", 'A' as i32);
    io.print("{}", 66 as ui8 as char);
    io.print("{}", true as i32);
    return 0;
}
```

```text
1007
true
65
B
1
```

What `as` does:

- between integers, it keeps the low bits, as C does; a literal that does not fit is an error;
- from a float to an integer, it stops the program if the value does not fit;
- `char` and `bool` convert to integers, and `ui8` to `char`;
- a class pointer converts to a parent or an interface (chapter 11);
- reinterpreting a pointer (`p as ui8*`) needs `unsafe` (chapter 15).

`as` binds tighter than the arithmetic: `-x as f32` is `(-x) as f32`, and `x as i32 * 2` multiplies the result.

### 3.6 Booleans and characters

`bool` is `true` or `false`; `&&`, `||` and `!` need booleans, and conditions too: `if (count)` is an error, write `if (count != 0)`.

`char` is a Unicode code point. Compare characters with `==` and `<`.

### 3.7 Text

A `str` is valid UTF-8 text, without a terminating NUL: a pointer to bytes and a length in bytes. It does not own its bytes: they are in the program, in an arena, or in a buffer.

<!-- run -->
```csharp
import std.io;

i32 main() uses io {
    str word = "naïve";
    io.print("{} bytes", word.len);
    mut i32 n = 0;
    for (char c in word.chars()) {
        n += 1;
    }
    io.print("{} characters, the first byte is {}", n, word.bytes[0]);
    io.print("{}", "abc" < "abd");
    return 0;
}
```

```text
6 bytes
5 characters, the first byte is 110
true
```

- `s.len` is the length in bytes, `s.bytes[i]` a byte, `s.chars()` the characters.
- `==` compares the contents, `<` compares byte by byte, which is the order of the code points.
- Building text allocates: `std.text` does it in an arena (chapter 22).
- Bytes from outside are checked: `str.from_bytes(bytes)` gives a `str!Utf8Error`.

### 3.8 Arrays and views

`T[N]` is an array of `N` elements, stored in place and copied with its owner. `T[]` is a *view*: a pointer and a length, for any number of elements. `x[a..b]` is the view of the elements `a` to `b - 1` of an array or of a view.

<!-- run -->
```csharp
import std.io;

i32 total(i32[] values) {
    mut i32 sum = 0;
    for (i32 v in values) {
        sum += v;
    }
    return sum;
}

i32 main() uses io {
    mut i32[5] a = [5, 1, 4, 2, 3];
    a[0] = 6;
    i32[] middle = a[1..4];
    io.print("{} {} {}", total(a[0..5]), total(middle), middle.len);
    i32[4] zeros = [0; 4];
    io.print("{}", zeros[3]);
    return 0;
}
```

```text
16 7 3
0
```

Every index is checked, in release builds too (`--checks=off` removes it). A view's elements can always be assigned, so taking a view of a local array needs a `mut` variable, as taking its address does.

`[a, b, c]` is an array literal, and `[value; count]` repeats a value. Their size must match the type.

### 3.9 Pointers

`T*` is a pointer, and it is **never null**. `&x` takes the address of a `mut` variable, `*p` reads or writes through a pointer, and `.` reaches a field or a method through a pointer as through a value: there is no `->`.

<!-- run -->
```csharp
import std.io;

void swap(i32* a, i32* b) {
    i32 t = *a;
    *a = *b;
    *b = t;
}

i32 main() uses io {
    mut i32 x = 1;
    mut i32 y = 2;
    swap(&x, &y);
    io.print("{} {}", x, y);
    return 0;
}
```

```text
2 1
```

A pointer that may be absent is `T*?`, an option (chapter 10). There is no pointer arithmetic outside `unsafe`.

> **Note:** a pointer to a local variable cannot be returned, nor stored where it outlives the function: the compiler refuses it (E0371, E0374). Part III explains the rules.

## 4. Expressions and Operators

### 4.1 Operators

| Kind | Operators |
|---|---|
| arithmetic | `+ - * / %` |
| comparison | `== != < <= > >=`, not chainable |
| logical | `&& \|\| !` |
| bitwise | `& \| ^ ~ << >>` |
| address | `&x`, `*p` |
| access | `.`, `[]`, `()` |
| conversion | `as`, `as?` (to a subclass) |
| range | `a..b` (exclusive), `a..=b` (inclusive) |
| conditional | `c ? a : b` |

### 4.2 Precedence

From the tightest:

1. postfix: `()` `[]` `.`
2. unary: `! ~ - & *`, and `as`
3. `* / %`
4. `+ -`
5. `<< >>`
6. `&`
7. `^`
8. `|`
9. `== != < <= > >=`
10. `&&`
11. `||`
12. `..` `..=`
13. `?:`

Unlike C, bitwise operators bind tighter than comparisons: `a & mask == 0` means `(a & mask) == 0`. `a < b < c` is an error.

### 4.3 What Sosu removed

| In C | In Sosu |
|---|---|
| `x++`, `x--` | `x += 1`, `x -= 1` |
| `if (x = 1)` | an assignment is a statement, not an expression |
| `a, b` (comma operator) | gone |
| `->` | `.` |
| `::` | `.` |
| implicit conversions | `as` |
| `switch` with fall-through | `match`, exhaustive (chapter 9) |

Evaluation is left to right, always.

### 4.4 Assignments

`= += -= *= /= %= &= |= ^= <<= >>=` are statements. An expression on its own line must be a call or a `match`: `x == 1;` is refused, since it does nothing.

## 5. Statements and Control Flow

### 5.1 Blocks and `if`

Braces are mandatory; parentheses around conditions too. `else if` chains as usual.

<!-- run -->
```csharp
import std.io;

str size(i32 n) {
    if (n < 10) {
        return "small";
    } else if (n < 100) {
        return "medium";
    } else {
        return "large";
    }
}

i32 main() uses io {
    io.print("{} {} {}", size(3), size(42), size(1000));
    return 0;
}
```

```text
small medium large
```

### 5.2 Loops

<!-- run -->
```csharp
import std.io;

i32 main() uses io {
    for (i32 i in 0..3) {
        io.print("range {}", i);
    }
    for (i32 i in 1..=2) {
        io.print("inclusive {}", i);
    }
    mut i32[3] items = [10, 20, 30];
    for (i32 x in items) {
        io.print("item {}", x);
    }
    for (mut i32 i = 8; i > 2; i /= 2) {
        io.print("c-style {}", i);
    }
    mut i32 n = 0;
    while (true) {
        n += 1;
        if (n == 2) {
            continue;
        }
        if (n > 3) {
            break;
        }
        io.print("while {}", n);
    }
    return 0;
}
```

```text
range 0
range 1
range 2
inclusive 1
inclusive 2
item 10
item 20
item 30
c-style 8
c-style 4
while 1
while 3
```

`for (T x in e)` walks a range, an array, a view, or anything with an iterator (`s.chars()`).

### 5.3 `defer`

`defer` runs a statement when the block ends, whatever the way out: the end of the block, `return`, `break`. Several run in reverse order. It is how memory and other resources are released.

<!-- run -->
```csharp
import std.io;

void work() uses io {
    defer io.print("third");
    defer io.print("second");
    io.print("first");
}

i32 main() uses io {
    work();
    return 0;
}
```

```text
first
second
third
```

`return` is not allowed inside a `defer`.

### 5.4 Names and scopes

A block opens a scope. A name cannot be declared twice in a function, even in an inner block: there is no shadowing of locals, so a name means one thing in a function. In a method, the fields and methods of `self` are reached without `self.`.

## 6. Functions

### 6.1 Declaring and calling

<!-- run -->
```csharp
import std.io;

i32 gcd(i32 a, i32 b) {
    if (b == 0) {
        return a;
    }
    return gcd(b, a % b);
}

void count_down(mut i32 n) uses io {
    while (n > 0) {
        io.print("{}", n);
        n -= 1;
    }
}

i32 main() uses io {
    io.print("{}", gcd(84, 36));
    count_down(2);
    return 0;
}
```

```text
12
2
1
```

- The return type comes first; `void` returns nothing.
- Parameters are passed by value. A parameter changes only if it is `mut`, and only in the function.
- A function that returns a value ends every path with `return`.
- There is no overloading and no default argument: two functions of a module have two names.
- Functions can be declared in any order.

### 6.2 Functions as values

A function is a value of a function type, and can be passed, stored and returned:

<!-- run -->
```csharp
import std.io;

i32 double(i32 x) {
    return x * 2;
}

i32 apply(fn(i32) -> i32 f, i32 x) {
    return f(x);
}

i32 main() uses io {
    fn(i32) -> i32 g = double;
    io.print("{} {}", apply(double, 5), g(7));
    return 0;
}
```

```text
10 14
```

Chapter 17 covers lambdas, which make such values on the spot.

### 6.3 Contracts

`requires` states what a function expects, `ensures` what it guarantees; `result` names the returned value there. They are checked in debug builds, and stop the program with a message when they fail.

<!-- run -->
```csharp
import std.io;

i32 clamp(i32 x, i32 lo, i32 hi)
    requires lo <= hi
    ensures result >= lo && result <= hi
{
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

i32 main() uses io {
    io.print("{}", clamp(15, 0, 10));
    return 0;
}
```

```text
10
```

`assert(condition)` checks a condition in every build. When it compares two values, a failure shows both sides.

## 7. Effects

### 7.1 Why

In most languages, nothing in `void save(Doc d)` says whether it writes a file, asks for the time, or starts a thread. In Sosu, the signature says it, and the compiler checks it:

| Effect | Covers |
|---|---|
| `io` | console, files, network, windows and display |
| `time` | reading the clock |
| `random` | random generators seeded by the system |
| `threads` | threads, channels, locks |

A function calls only what its effects allow. Effects are written, never inferred, so the signature is enough to know them.

<!-- error E0340 -->
```csharp
import std.io;

void log(str message) {
    io.print("{}", message);
}
```

The fix is to declare it, which makes every caller declare it too:

<!-- run -->
```csharp
import std.io;
import std.time;

void log(str message) uses io {
    io.print("{}", message);
}

i64 seconds() uses time {
    return time.unix_seconds();
}

i32 main() uses io, time {
    log("started");
    io.print("{}", seconds() > 0);
    return 0;
}
```

```text
started
true
```

### 7.2 The rules

- A function without `uses` has no effect: it computes from its arguments and its memory only. Such a function is easy to test and safe to call anywhere.
- `main` declares what the program does.
- `test` blocks may use every effect.
- Effects are part of function types: `fn(i32) -> void uses io`. A function with fewer effects fits where more are allowed.
- An overriding method cannot add an effect to the method it overrides.
- C functions declare their effects by hand; `sosuc bindgen` writes `uses io` on every C function.
- A higher-order function takes the effects of its callbacks with an effect parameter, `uses E` (chapter 17).

> **Tip:** when a deep function starts to need `io`, the compiler lists every caller to change. That is the cost of effects, and also their point: the change is visible in review.

---

# Part II. Data

## 8. Structs

### 8.1 Values with fields

A `struct` is a value: copied when assigned, passed or returned. Its layout is the C layout of the same fields, in the same order.

<!-- run -->
```csharp
import std.io;

struct Vec2 {
    f64 x;
    f64 y;

    f64 dot(Vec2 o) {
        return x * o.x + y * o.y;
    }

    Vec2 plus(Vec2 o) {
        return Vec2{ x: x + o.x, y: y + o.y };
    }
}

i32 main() uses io {
    Vec2 a = Vec2{ x: 1.0, y: 2.0 };
    Vec2 b = a.plus(Vec2{ x: 2.0, y: 0.5 });
    io.print("{} {} {}", b.x, b.y, a.dot(b));
    return 0;
}
```

```text
3.0 2.5 8.0
```

- A literal names every field: `Vec2{ x: 1.0, y: 2.0 }`. A field may have a default value in the declaration, and can then be left out.
- Methods are functions declared inside; `self` is a pointer to the value, and its fields are reached without `self.`.
- There is no inheritance for structs: use a class (chapter 11) when you need it.

### 8.2 Changing a struct

A field changes only if it is declared `mut`, and only through something that may change: a `mut` variable, or a pointer.

<!-- run -->
```csharp
import std.io;

struct Counter {
    str name;
    mut i32 value;

    void bump() {
        value += 1;
    }
}

i32 main() uses io {
    mut Counter c = Counter{ name: "clicks", value: 0 };
    c.bump();
    c.bump();
    Counter copy = c;  // a copy
    c.bump();
    io.print("{} {} {}", c.name, c.value, copy.value);
    return 0;
}
```

```text
clicks 3 2
```

`name` cannot change after the literal: only `mut` fields can.

## 9. Enums and `match`

### 9.1 Sum types

An `enum` is a value that is one of several variants, each with its own data or none.

<!-- run -->
```csharp
import std.io;

enum Shape {
    Circle(f64 radius),
    Rect(f64 width, f64 height),
    Empty,
}

f64 area(Shape s) {
    return match (s) {
        Circle(r) => 3.0 * r * r,
        Rect(w, h) => w * h,
        Empty => 0.0,
    };
}

i32 main() uses io {
    Shape[3] shapes = [Circle(1.0), Rect(2.0, 3.0), Shape.Empty];
    for (Shape s in shapes) {
        io.print("{}", area(s));
    }
    return 0;
}
```

```text
3.0
6.0
0.0
```

A variant is written `Shape.Circle(1.0)`, or `Circle(1.0)` where the type is known. An enum without data is a classic enumeration, and compares with `==`.

### 9.2 `match`

`match` is an expression, and it must cover every variant: a forgotten case is a compile error (E0312), which is why adding a variant shows every place to update. `_` is the default case, and must be written.

<!-- run -->
```csharp
import std.io;

enum Light {
    Red,
    Amber,
    Green,
}

str advice(Light l) {
    return match (l) {
        Green => "go",
        _ => "stop",
    };
}

void show(Light l) uses io {
    match (l) {
        Red => io.print("red"),
        Amber => {
            io.print("amber");
            io.print("careful");
        },
        Green => io.print("green"),
    }
}

i32 main() uses io {
    show(Light.Amber);
    io.print("{} {}", advice(Light.Green), advice(Light.Red));
    return 0;
}
```

```text
amber
careful
go stop
```

- An arm is an expression or a block; arms are separated by commas.
- Patterns are `_`, `Variant`, and `Variant(a, b)`, which names the data.
- Used as a statement, a `match` needs no semicolon.
- Enums with data are compared with `match`, not `==`.

## 10. Absence and Failure

### 10.1 `T?`: maybe a value

`T?` holds `some(x)` or `none`. There is no null: a pointer that may be absent is `T*?`, and you have to look before you use it.

<!-- run -->
```csharp
import std.io;

i32? index_of(str[] names, str wanted) {
    for (i32 i in 0..names.len as i32) {
        if (names[i as usize] == wanted) {
            return some(i);
        }
    }
    return none;
}

i32 main() uses io {
    mut str[3] names = ["Ada", "Grace", "Alan"];
    match (index_of(names[0..3], "Grace")) {
        some(i) => io.print("found at {}", i),
        none => io.print("not found"),
    }
    i32? missing = index_of(names[0..3], "Bob");
    io.print("{}", match (missing) {
        some(i) => i,
        none => -1,
    });
    return 0;
}
```

```text
found at 1
-1
```

`none` needs an expected type: `i32? x = none;`. `some(x)` takes the type of `x`.

### 10.2 `T!E`: a value or an error

`T!E` holds `ok(x)` or `err(e)`, where `E` is usually an enum of the ways it fails. The result of a call that can fail must be used: you cannot ignore an error by accident.

`try` gives the value, or returns the error to the caller. The early return is written where it happens.

<!-- run -->
```csharp
import std.io;
import std.text;

enum ConfigError {
    Missing,
    NotANumber,
    OutOfRange,
}

i32!ConfigError port_of(str value) {
    if (value.len == 0) {
        return err(ConfigError.Missing);
    }
    i64 n = match (text.parse_i64(value)) {
        ok(v) => v,
        err(e) => {
            return err(ConfigError.NotANumber);
        },
    };
    if (n < 1 || n > 65535) {
        return err(ConfigError.OutOfRange);
    }
    return ok(n as i32);
}

i32!ConfigError next_port(str value) {
    i32 port = try port_of(value);
    return ok(port + 1);
}

str describe(ConfigError e) {
    return match (e) {
        Missing => "missing",
        NotANumber => "not a number",
        OutOfRange => "out of range",
    };
}

i32 main() uses io {
    str[4] inputs = ["8080", "", "eighty", "99999"];
    for (str s in inputs) {
        match (next_port(s)) {
            ok(p) => io.print("ok {}", p),
            err(e) => io.print("error: {}", describe(e)),
        }
    }
    return 0;
}
```

```text
ok 8081
error: missing
error: not a number
error: out of range
```

- `try` works in a function that returns a result with the same error type.
- A function that returns nothing but can fail returns `void!E`, and ends with `return ok();`.
- `ok(x)` and `err(e)` need an expected type, given by the return type or the variable.

### 10.3 Designing errors

- Make the error type an enum of what the caller can act on, not of what went wrong inside.
- Convert errors at module boundaries with `match`: a parser's `ParseError` becomes a configuration's `ConfigError`.
- Keep `assert` and contracts for bugs, and `T!E` for what can happen with correct code: a missing file, bad input.

## 11. Classes and Interfaces

### 11.1 Classes

A `class` is a type allocated in an arena and handled through a pointer, never copied. It is what you use for objects with identity: a window, a parser, a node of a tree.

<!-- run -->
```csharp
import std.io;

class Account {
    str owner;
    mut i64 balance;

    void init(keeps str owner, i64 opening) {
        self.owner = owner;
        balance = opening;
    }

    bool withdraw(i64 amount) {
        if (amount > balance) {
            return false;
        }
        balance -= amount;
        return true;
    }
}

i32 main() uses io {
    mut Arena arena = Arena.new(1 * KB);
    defer arena.free();
    Account* a = arena.new(Account, "Ada", 100);
    io.print("{}", a.withdraw(30));
    io.print("{}", a.withdraw(500));
    io.print("{} {}", a.owner, a.balance);
    return 0;
}
```

```text
true
false
Ada 70
```

- `arena.new(Type, arguments)` allocates the object and calls its `init`.
- `init` keeps `owner` in the object, beyond the call: it says so with `keeps` (chapter 14), or the compiler refuses it.
- A field is assigned in `init`; elsewhere, only if it is `mut`.
- There is no destructor: the memory goes with the arena, and other resources are released with `defer`.

### 11.2 Inheritance and virtual methods

A class has at most one parent class, written first, then any number of interfaces. Methods are **not** virtual unless declared `virtual`, and `override` is mandatory to redefine one: the cost of a virtual call is visible in the code.

<!-- run -->
```csharp
import std.io;

class Shape {
    str name;

    void init(keeps str name) {
        self.name = name;
    }

    virtual f64 area() {
        return 0.0;
    }

    void describe() uses io {
        io.print("{}: {}", name, area());
    }
}

class Square : Shape {
    f64 side;

    void init(f64 side) {
        base.init("square");
        self.side = side;
    }

    override f64 area() {
        return side * side;
    }
}

i32 main() uses io {
    mut Arena arena = Arena.new(1 * KB);
    defer arena.free();
    Square* sq = arena.new(Square, 3.0);
    Shape* s = sq as Shape*;
    s.describe();
    match (s as? Square*) {
        some(back) => io.print("a square of side {}", back.side),
        none => io.print("not a square"),
    }
    return 0;
}
```

```text
square: 9.0
a square of side 3.0
```

- `base.init(...)` starts the `init` of a derived class; `base` is `self` seen as the parent.
- `as` converts to a parent or an interface; `as?` converts to a subclass, checked when the program runs, and gives `T*?`.
- `override` on a method that redefines nothing is an error.

### 11.3 Interfaces

An interface lists method signatures; only classes implement them, with `override`.

<!-- run -->
```csharp
import std.io;

interface Speaker {
    str speak();
}

class Dog : Speaker {
    void init() { }

    override str speak() {
        return "woof";
    }
}

class Robot : Speaker {
    i32 serial;

    void init(i32 serial) {
        self.serial = serial;
    }

    override str speak() {
        return "beep";
    }
}

void talk(Speaker*[] speakers) uses io {
    for (Speaker* s in speakers) {
        io.print("{}", s.speak());
    }
}

i32 main() uses io {
    mut Arena arena = Arena.new(1 * KB);
    defer arena.free();
    mut Speaker*[2] all = [arena.new(Dog) as Speaker*, arena.new(Robot, 7) as Speaker*];
    talk(all[0..2]);
    return 0;
}
```

```text
woof
beep
```

An interface can also be a constraint of a generic (chapter 16): `<T: Speaker>` calls `t.speak()` directly, without a virtual call.

---

# Part III. Memory

Sosu has no garbage collector, no reference counting and no global allocator. This part explains what it has instead, what the compiler checks, and what it leaves to you.

## 12. Arenas

### 12.1 The idea

An arena is a region of memory that grows by blocks. You allocate in it, and you free it all at once. Freeing objects one by one does not exist: you choose how long a *group* of objects lives, which is usually how programs think anyway (a request, a frame, a document, a level).

<!-- run -->
```csharp
import std.io;

struct Point {
    mut i32 x;
    mut i32 y;
}

i32 main() uses io {
    mut Arena arena = Arena.new(4 * KB);
    defer arena.free();

    Point* p = arena.alloc(Point);
    p.x = 3;
    i32[] squares = arena.alloc_slice(i32, 10);
    for (usize i in 0..squares.len) {
        squares[i] = (i * i) as i32;
    }
    io.print("{} {} {}", p.x, p.y, squares[9]);
    return 0;
}
```

```text
3 0 81
```

### 12.2 What an arena gives

| Call | Gives |
|---|---|
| `Arena.new(size)` | an arena whose blocks are `size` bytes; a larger allocation gets its own block |
| `a.alloc(T)` | a zeroed `T*`; stops the program when memory runs out |
| `a.try_alloc(T)` | a zeroed `T*?`: `none` when memory runs out |
| `a.alloc_slice(T, n)` | a zeroed `T[]` of `n` elements |
| `a.new(C, args)` | a class, through its `init` |
| `a.closure(lambda)` | a closure that lives in the arena (chapter 17) |
| `a.free()` | frees every block; the arena cannot be used after |
| `size_of(T)` | the size of a type, in bytes |

`KB`, `MB` and `GB` are `usize` constants. Since `alloc` gives zeroed memory, the type cannot contain a pointer (which is never null) nor be a class: build such values with a literal, or with `a.new`.

### 12.3 Allocation is in the signature

A function that allocates receives an `Arena*`, so the caller sees it and decides how long the memory lives:

<!-- run -->
```csharp
import std.io;
import std.text;

str greeting(Arena* arena, str name) {
    mut str[3] parts = ["Hello, ", name, "!"];
    return text.join(arena, parts[0..3], "");
}

i32 main() uses io {
    mut Arena arena = Arena.new(1 * KB);
    defer arena.free();
    io.print("{}", greeting(&arena, "Ada"));
    return 0;
}
```

```text
Hello, Ada!
```

### 12.4 Patterns

- **One arena per job.** A request handler, a frame of a game or a window, a file read: make an arena at the start, free it at the end.
- **A scratch arena.** A function that needs temporary memory makes a local arena and frees it with `defer`; only its result goes into the caller's arena.
- **An object that owns its arena.** A document keeps an `Arena` in a field, allocates its nodes there, and frees it when it closes (12.6).

> **Tip:** an arena is cheap to make. When in doubt, use a new one for a new kind of lifetime.

### 12.5 Moves and copies

An `Arena` owns its blocks, so it has **one owner**: it is moved, not copied. Using it as a value (initializing a variable, assigning, passing it by value, returning it, putting it in a literal) moves it, and the old variable cannot be used until it gets a new value. Calling its methods, taking its address and reading its fields do not move it.

<!-- error E0356 -->
```csharp
void f() {
    mut Arena a = Arena.new(64);
    mut Arena b = a;
    a.free();
}
```

The same goes for any value that contains an arena. To lend an arena, pass a pointer: `&a`. To duplicate a value that owns one, call `x.copy()`: each arena in it becomes a new, empty arena, and the other fields are copied as they are.

### 12.6 An object that owns its arena

A class can keep its arena, allocate its parts in it, and free everything at once:

<!-- run -->
```csharp
import std.io;
import std.vec;

class Document {
    mut Arena arena;
    vec.Vec<str>* lines;

    void init() {
        arena = Arena.new(4 * KB);
        lines = vec.new(&arena);
    }

    void add(keeps str line) {
        lines.push(line);
    }

    void close() {
        arena.free();
    }
}

i32 main() uses io {
    mut Arena arena = Arena.new(1 * KB);
    defer arena.free();
    Document* d = arena.new(Document);
    d.add("first");
    d.add("second");
    io.print("{}", d.lines.len());
    d.close();
    return 0;
}
```

```text
2
```

`keeps` on `add` says that the line is kept beyond the call; chapter 14 explains it.

## 13. What the Compiler Checks

Sosu has no lifetimes in its types. Instead, the compiler follows where the memory of each arena goes, inside each function and through a summary of each function it calls, and refuses what cannot be valid. These rules are called the *regions*.

### 13.1 The origins

Each pointer and each view has an *origin*, which is never written:

| Origin | Where the memory comes from |
|---|---|
| the function | `&x` of a local or a parameter; a view of a local array |
| an arena | `a.alloc`, `a.new`, `a.alloc_slice`, and what is reached through them |
| the caller | a parameter, and what is reached through it |
| static | string literals and constants |
| unchecked | what C or an `unsafe` block gives |

A variable takes the origins of every value it is given, without following the order of the branches.

### 13.2 A pointer into the function does not leave it (E0371)

<!-- error E0371 -->
```csharp
i32* f() {
    mut i32 x = 1;
    return &x;
}
```

### 13.3 No use after `free()` (E0372)

<!-- error E0372 -->
```csharp
i32 f() {
    mut Arena a = Arena.new(64);
    i32* p = a.alloc(i32);
    a.free();
    return *p;
}
```

The check follows the order of the source, as moves do: a free in a branch makes the variable unusable after the `if`. It sees a free in a function you call, a free of an arena in a field, and a free in an overriding method called through its parent.

### 13.4 What is returned outlives the function (E0373)

<!-- error E0373 -->
```csharp
i32* f() {
    mut Arena a = Arena.new(64);
    defer a.free();
    return a.alloc(i32);
}
```

The fix is to receive the arena:

<!-- check -->
```csharp
i32* make(Arena* arena) {
    return arena.alloc(i32);
}
```

Returning memory of a local arena is allowed when the arena is returned with it, in the same value: an object that owns its arena.

### 13.5 What is stored lives as long as where it goes (E0374)

A pointer may be written into memory that lasts no longer than what it points to: a local variable, memory of the same arena, or memory of the caller when it comes from the caller or from an `Arena*` parameter marked `keeps`.

<!-- error E0374 -->
```csharp
struct Holder {
    mut i32*? p;
}

void fill(Holder* h) {
    mut Arena a = Arena.new(64);
    defer a.free();
    h.p = some(a.alloc(i32));
}
```

Two arenas received as parameters may not live as long as each other: memory of one is not stored into the other.

### 13.6 A field of an arena gets a new value with it (E0378)

A field given memory of an arena of the same object belongs to that arena. After the arena is freed, the field cannot be read until it gets a value; a function that makes the arena again gives the field a value too, itself or through a method it calls.

<!-- error E0378 -->
```csharp
class Cache {
    mut Arena arena;
    mut i32[] items;

    void init() {
        arena = Arena.new(1 * KB);
        items = arena.alloc_slice(i32, 16);
    }

    void reset() {
        arena.free();
        arena = Arena.new(1 * KB);
    }
}
```

### 13.7 In lambdas

The rules hold in the body of a lambda too: its own arenas end with it, and what it captures follows the rules of its function. A free in a lambda is followed only for an arena of the lambda, since the lambda may never be called.

## 14. `keeps`

### 14.1 The word

A function may store a pointer it receives. If it does, the caller must give memory that lives long enough, so the function says it: the parameter is marked `keeps`. Most parameters are only read during the call and need nothing.

<!-- check -->
```csharp
class Library {
    mut str[8] titles;
    mut usize count;

    void init() {
        titles = [""; 8];
        count = 0;
    }

    void add(keeps str title) requires count < 8 {
        titles[count] = title;
        count += 1;
    }
}
```

Without `keeps`, storing `title` is refused (E0376). With it, a caller that gives memory ending sooner is refused (E0375):

<!-- error E0375 -->
```csharp
struct Holder {
    mut i32*? p;
}

void hold(Holder* h, keeps i32* p) {
    h.p = some(p);
}

void f(Holder* h) {
    mut Arena a = Arena.new(64);
    defer a.free();
    hold(h, a.alloc(i32));
}
```

### 14.2 The forms of `keeps`

| Written | Means |
|---|---|
| `void push(keeps T value)` | `value` may be stored beyond the call |
| `keeps H* h` | also what is reached through `h`: storing `h.next` elsewhere needs it |
| `keeps Arena* arena` | memory allocated in `arena` may be stored outside it; every constructor that keeps its arena says it |
| `void give(H* h) keeps self` | what `self` reaches may be stored elsewhere |
| `fn(keeps Node*) -> void` | a function type whose parameter is kept |
| `(keeps Node* n) => ...` | a lambda parameter that is kept |
| `window Main(keeps State* s)` | a window whose widgets bind memory of `s` |

<!-- check -->
```csharp
struct Node {
    i32 value;
}

struct Box {
    mut Node*? node;
}

class Pool {
    Arena* arena;

    void init(keeps Arena* arena) {
        self.arena = arena;
    }
}

void link(Box* to, keeps Box* from) {
    to.node = from.node;
}

void each(fn(keeps Node*) -> void f, keeps Node* n) {
    f(n);
}
```

### 14.3 Rules that go with it

- A function that keeps fewer parameters may be given where a function type keeps more; never the reverse.
- An overriding method keeps no more than the method it overrides.
- `keeps` describes the parameter and everything it reaches. When that is too coarse (the elements of a view move between two buffers, as in a merge sort), `unsafe` is the way out, with a comment.

## 15. What Is Not Checked, and `unsafe`

### 15.1 The promise

> **Warning:** Sosu is not memory safe, by choice. Without lifetimes in the types, some dangling pointers cannot be proved absent, and Sosu does not check pointers at run time in release builds. A program can have undefined behaviour without `unsafe`.

What Sosu promises: the compiler refuses the cases of the rules above; what it does not follow is listed in [section 7.7 of the specification](SPEC.md); and a debug build stops on a pointer used after the `free()` of its arena.

### 15.2 What passes

- Two places reached through one value, which may be in two arenas: `a.p = a.q`.
- A field of another object kept behind a flag of your program (`Cell.val` while `state == 0`), and the elements of an array in a field.
- A value that was closed by one function and read in another.
- A free through a name given by `mut`, through a pointer returned by a function, or through a value of function type.
- `unsafe` code, and what C does.

### 15.3 Debug builds

A debug build makes the pages of a freed arena unreadable, for the last 1 024 blocks freed by each thread, and stops the program on a read or a write through them, with the exit code 101 and a clear message. Test in debug before a release.

### 15.4 `unsafe`

`unsafe { ... }` is the only place for pointer arithmetic, pointer reinterpretation, integer and pointer conversions, and calls to C that are not wrapped. A function marked `unsafe` can only be called from an `unsafe` block. Inside it, the regions are off.

<!-- run -->
```csharp
import std.io;

i32 main() uses io {
    mut i32[4] values = [1, 2, 3, 4];
    i32[] view = values[0..4];
    mut i32 third = 0;
    unsafe {
        ui8* bytes = &view[0] as ui8*;
        i32* p = (bytes + 8) as i32*;
        third = *p;
    }
    io.print("{}", third);
    return 0;
}
```

```text
3
```

> **Tip:** keep `unsafe` blocks small, and wrap them in safe functions whose signatures say what callers must respect, with contracts where they help.

---

# Part IV. Abstraction

## 16. Generics

### 16.1 Generic functions

Type parameters come after the name, and are inferred at the call from the arguments, or else from the expected type.

<!-- run -->
```csharp
import std.io;

T largest<T: Ordered>(T[] items) requires items.len > 0 {
    mut T best = items[0];
    for (T x in items) {
        if (x > best) {
            best = x;
        }
    }
    return best;
}

T sum<T: Number>(T[] items) {
    i32 zero = 0;
    mut T total = zero as T;  // a literal is never a T by itself
    for (T x in items) {
        total = total + x;
    }
    return total;
}

i32 main() uses io {
    mut i32[3] a = [3, 9, 4];
    mut f64[2] b = [1.5, 2.25];
    mut str[2] c = ["pear", "apple"];
    io.print("{} {} {}", largest(a[0..3]), sum(b[0..2]), largest(c[0..2]));
    io.print("{}", largest<i32>(a[0..2]));
    return 0;
}
```

```text
9 3.75 pear
9
```

### 16.2 Constraints

A generic body is checked once, against its constraints: an operation on a `T` is allowed only if a constraint allows it. Without constraint, a `T` is assigned, passed, returned, stored and pointed to, nothing else.

| Constraint | Allows | Satisfied by |
|---|---|---|
| `Equal` | `==`, `!=` | numbers, `bool`, `char`, `str`, pointers, enums without data |
| `Ordered` | `Equal`, and `< <= > >=` | numbers, `char`, `str` |
| `Number` | `Ordered`, `+ - * /`, `min`, `max`, `as` to and from numbers, `io.print` | the integers and the floats |
| `Integer` | `Number`, `%`, bitwise operators, shifts | the integers |
| `Hash` | `Equal`, `hash(x)` | numbers, `bool`, `char`, `str`, pointers, enums without data |
| `Send` | crossing to another thread | values without pointers (chapter 23) |
| an interface | its methods, as direct calls | pointers to classes that implement it |

Several constraints are joined with `+`: `<K: Hash + Ordered>`.

### 16.3 Generic types

<!-- run -->
```csharp
import std.io;

struct Pair<A, B> {
    A first;
    B second;
}

enum Tree<T> {
    Leaf(T value),
    Node(Tree<T>* left, Tree<T>* right),
}

i32 count<T>(Tree<T> t) {
    return match (t) {
        Leaf(v) => 1,
        Node(l, r) => count(*l) + count(*r),
    };
}

i32 main() uses io {
    Pair<str, i32> p = Pair{ first: "age", second: 41 };
    io.print("{} {}", p.first, p.second);
    mut Tree<i32> a = Leaf(1);
    mut Tree<i32> b = Leaf(2);
    Tree<i32> both = Node(&a, &b);
    io.print("{}", count(both));
    return 0;
}
```

```text
age 41
2
```

- A type argument is a value type: a number, `bool`, `char`, `str`, a pointer (to a class too), a struct, an enum, an option, a result, an array or a view. A class is not a value: write `Vec<Node*>`, not `Vec<Node>`.
- A type argument cannot own an arena: `Vec<Arena>` is refused.
- Methods can have type parameters of their own.

### 16.4 How it compiles

Each set of type arguments gives its own C code, as C++ templates do: a generic costs nothing when it runs, and errors are reported in the generic code, never inside an instance.

## 17. Lambdas and Closures

### 17.1 Lambdas

`(parameters) => body` makes a function value. The body is an expression, or a block with `return`. A parameter is a type and a name, or a name alone when the expected type gives it.

<!-- run -->
```csharp
import std.io;
import std.sort;

struct Person {
    str name;
    i32 age;
}

i32 main() uses io {
    auto square = (i32 x) => x * x;
    io.print("{}", square(7));

    mut Person[3] people = [Person{ name: "Ada", age: 36 }, Person{ name: "Alan", age: 41 }, Person{ name: "Grace", age: 30 }];
    sort.sort_by(people[0..3], (a, b) => a.age < b.age);
    for (Person p in people) {
        io.print("{} {}", p.name, p.age);
    }
    fn(i32) -> i32 clamp = (x) => {
        if (x > 100) {
            return 100;
        }
        return x;
    };
    io.print("{}", clamp(250));
    return 0;
}
```

```text
49
Grace 30
Ada 36
Alan 41
100
```

`return` in a lambda leaves the lambda, not the function around it.

### 17.2 Captures

A lambda captures the variables it uses, **by copy**, when it is created. A copy cannot change (E0369): to change a variable, capture a pointer to it. A value that owns an arena cannot be captured: capture a pointer.

<!-- run -->
```csharp
import std.io;

void repeat(i32 n, fn() -> void f) {
    for (i32 i in 0..n) {
        f();
    }
}

i32 main() uses io {
    mut i32 total = 0;
    i32* t = &total;
    i32 step = 5;
    repeat(3, () => {
        *t += step;
    });
    io.print("{}", total);
    return 0;
}
```

```text
15
```

In a method, a lambda captures `self`, a pointer: the fields of the object are shared, not copied.

### 17.3 Where a closure lives

A lambda that captures variables, a *closure*, keeps what it captured in the variables of the function that made it, so it cannot outlive that function: returning it or storing it in a field is refused (E0370). To keep a closure, make it in an arena, which copies what it captures:

<!-- run -->
```csharp
import std.io;

class Button {
    fn() -> i32 on_click;

    void init(keeps fn() -> i32 on_click) {
        self.on_click = on_click;
    }
}

Button* make(Arena* arena, i32 k) {
    return arena.new(Button, arena.closure(() => k + 1));
}

i32 main() uses io {
    mut Arena arena = Arena.new(1 * KB);
    defer arena.free();
    Button* b = make(&arena, 41);
    io.print("{}", b.on_click());
    return 0;
}
```

```text
42
```

The closure then lives as long as the arena, and follows the rules of Part III.

### 17.4 Effect parameters

A higher-order function takes the effects of its callbacks with an effect parameter, `uses E`:

<!-- run -->
```csharp
import std.io;

void each<T, uses E>(T[] items, fn(T) -> void uses E f) uses E {
    for (T x in items) {
        f(x);
    }
}

i32 main() uses io {
    mut i32[3] values = [1, 2, 3];
    each(values[0..3], (v) => io.print("{}", v));  // here, `each` uses io
    mut i32 sum = 0;
    i32* s = &sum;
    each(values[0..3], (v) => {
        *s += v;
    });                                             // here, nothing
    io.print("{}", sum);
    return 0;
}
```

```text
1
2
3
6
```

## 18. Methods, Iteration and Small Abstractions

### 18.1 Methods on any struct, enum or class

Structs and classes have methods. A method that changes a struct needs to be called on something `mut`, or through a pointer.

### 18.2 Iterators

`for (T x in e)` works on ranges, arrays, views and the characters of a `str`. For your own sequences, the convention is a generic interface `Iterator<T>`, implemented by classes, and a loop that asks for the next value until there is none:

<!-- run -->
```csharp
import std.io;

interface Iterator<T> {
    T? next();
}

class Countdown : Iterator<i32> {
    mut i32 n;

    void init(i32 from) {
        n = from;
    }

    override i32? next() {
        if (n == 0) {
            return none;
        }
        n -= 1;
        return some(n + 1);
    }
}

i32 sum(Iterator<i32>* items) {
    mut i32 total = 0;
    mut bool more = true;
    while (more) {
        match (items.next()) {
            some(x) => {
                total += x;
            },
            none => {
                more = false;
            },
        }
    }
    return total;
}

i32 main() uses io {
    mut Arena arena = Arena.new(1 * KB);
    defer arena.free();
    Countdown* c = arena.new(Countdown, 3);
    io.print("{}", sum(c as Iterator<i32>*));
    return 0;
}
```

```text
6
```

---

# Part V. Programs

## 19. Modules and Packages

### 19.1 Modules

Each file of `src/` is a module, named after its package and path: `src/geometry/shapes.sosu` of the package `app` is `app.geometry.shapes`. Everything is private by default; `pub` exports a type, a function, a method, a field or a constant.

```text
app/
  sosu.toml
  src/
    main.sosu
    geometry/
      shapes.sosu
```

```csharp
// src/geometry/shapes.sosu
pub struct Rect {
    pub f64 width;
    pub f64 height;
}

pub f64 area(Rect r) {
    return r.width * r.height;
}
```

```csharp
// src/main.sosu
import std.io;
import app.geometry.shapes;

i32 main() uses io {
    shapes.Rect r = shapes.Rect{ width: 2.0, height: 3.0 };
    io.print("{}", shapes.area(r));
    return 0;
}
```

- An import starts with a package name and always names a module: `import app;` alone is an error.
- An imported module is used through the last name of its path, so the origin of every name is visible where it is used: `shapes.area(r)`, `io.print(...)`.
- The modules of a package may import each other; packages may not import each other in a cycle.
- `core` and `std` are always available. `core.prelude` is loaded with every program: its public names, for now `Utf8Error`, need no import.

### 19.2 Applications and libraries

A package with `src/main.sosu` is an application; one with `src/lib.sosu` is a library, used by other packages. The compiler loads the modules reachable by `import` from that entry file.

### 19.3 Dependencies

```toml
[package]
name = "app"
version = "0.1.0"

[dependencies]
shared = { path = "../shared" }
ui = { git = "https://git.example/sosu-ui", tag = "v0.1.0" }
fast = { git = "https://git.example/fast", rev = "3f2a91c" }
```

- A dependency is a folder (`path`) or a git repository with a `tag` or a `rev` (a commit). A branch is not accepted: it moves.
- Git dependencies are cloned into `build/deps/` of the root package, once per commit; `git` must be installed.
- `sosu.lock`, written by the compiler next to `sosu.toml`, pins the commit of every git dependency, transitive ones included. Keep it in version control.
- Dependencies are compiled from source with the program. There is no central registry yet.

> **Warning:** building a package runs code of its author: `sosuc` compiles the C files and applies the `[link]` settings of every dependency. Add a dependency only from a source you trust. Its URLs and values are checked so that none becomes a command (E0404, E0416).

### 19.4 The manifest

`sosu.toml` is a subset of TOML: comments, strings, one-line lists and one-line inline tables.

| Section | Keys |
|---|---|
| `[package]` | `name`, `version` (required) |
| `[dependencies]` | one key per dependency, as above |
| `[link]`, `[link.windows]`, ... | C libraries and C sources (chapter 25) |
| `[app]` | `id`, `name`, `version`, `icon`, `assets`: for packaging (chapter 26) |

## 20. Testing

### 20.1 `test` blocks

A `test` block sits next to the code it tests; `sosuc test` runs every `test` block of the package. Tests may use every effect.

<!-- test -->
```csharp
i32 fib(i32 n) {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

test "the first numbers of Fibonacci" {
    assert(fib(0) == 0);
    assert(fib(1) == 1);
    assert(fib(10) == 55);
}

test "arenas are fine in tests" {
    mut Arena arena = Arena.new(1 * KB);
    defer arena.free();
    i32[] values = arena.alloc_slice(i32, 4);
    assert(values.len == 4 && values[0] == 0);
}
```

- A failed `assert` names its position and its condition. On a comparison of numbers, `bool`, `char` or `str`, it shows both sides: `left: 4`, `right: 5`.
- A run-time error (an overflow, an index out of bounds, a failed contract) fails the current test, and the others still run.
- `sosuc test` exits with 0 when every test passes.

### 20.2 The `testing` package

`packages/testing` adds what `assert` does not show. Add it to `[dependencies]` (`testing = { path = ".../packages/testing" }`):

| Module | Gives |
|---|---|
| `testing.check` | `same_text`, `same_bytes`, `same_items`, `near`, `contains`: comparisons that print what differs |
| `testing.bench` | `bench.run(name, f)`: benchmarks, run by `sosuc test --bench` |
| `testing.fixture` | the data files of a test, golden files (`sosuc test --update` writes them again), temporary folders |

```csharp
import testing.check;

test "a report" {
    str report = "total: 3";
    assert(check.same_text(report, "total: 3"));
    assert(check.near(0.1 + 0.2, 0.3, 1e-9));
}
```

### 20.3 What to test

Functions without effects are the easiest to test: they depend only on their arguments. Push effects to the edges of a program (`main`, the event handlers), and keep the logic in functions that do not declare any.

## 21. Diagnostics and Tools

### 21.1 Reading an error

Each error has a stable code, a position, an explanation and, when possible, a fix:

```text
error[E0312]: non-exhaustive match
  --> shapes.sosu:14:12
   |
14 |     return match (s) {
   |            ^^^^^ missing case: Empty
   = help: add `Empty => ...` or `_ => ...`
```

A code keeps its meaning across versions; the messages may improve. [errors.md](errors.md) explains every code with an example. `--format=json` gives the same diagnostics for tools.

### 21.2 The formatter

`sosuc fmt` has one style and no option. It prints the file from its syntax tree and puts every comment back:

- four spaces; `{` at the end of the line; a non-empty block on several lines, an empty one as `{ }`;
- spaces around binary operators and after commas, none inside brackets;
- struct literals on one line: `Point{ x: 1.0, y: 2.0 }`;
- parentheses only where precedence needs them;
- `requires` and `ensures` on their own lines, `{` alone below them.

`sosuc fmt --check` reports the files that would change, for continuous integration.

### 21.3 Debug and release

| | debug (default) | `--release` |
|---|---|---|
| integer overflow | stops the program | wraps around |
| contracts | checked | off |
| array indexes | checked | checked (unless `--checks=off`) |
| use after `free()` of an arena | stops the program | not checked |
| C optimization | off | on |

A run-time error prints its position and stops the program with the exit code 101.

## 22. The Standard Library

`core` and `std` are plain Sosu, in `lib/`. This chapter is a tour; the [reference](REFERENCE.md) lists every function.

| Module | For |
|---|---|
| `std.io` | the console (`io.print`), whole files, open files |
| `std.fs` | folders, file information, paths |
| `std.os` | arguments, environment, exit, running a program |
| `std.text` | searching, splitting, joining, converting text |
| `std.vec`, `std.map` | growable vectors and hash maps |
| `std.sort` | sorting and binary search |
| `std.math` | the usual functions, `PI` |
| `std.random` | a seeded generator |
| `std.time` | clocks and calendar dates |
| `std.json` | JSON values, parsed and written |
| `std.cli` | command-line options |
| `std.log` | logging lines |
| `std.thread`, `std.channel` | concurrency (chapter 23) |

### 22.1 Text

<!-- run -->
```csharp
import std.io;
import std.text;

i32 main() uses io {
    mut Arena arena = Arena.new(4 * KB);
    defer arena.free();
    str csv = " name, age, city ";
    str[] fields = text.split(&arena, text.trim(csv), ", ");
    io.print("{} fields, the last is {}", fields.len, fields[2]);
    io.print("{}", text.to_upper(&arena, fields[0]));
    io.print("{}", text.replace(&arena, "a-b-c", "-", "+"));
    match (text.find("hello world", "world")) {
        some(at) => io.print("found at byte {}", at),
        none => io.print("not found"),
    }
    match (text.parse_i64("42")) {
        ok(n) => io.print("{}", n + 1),
        err(e) => io.print("not a number"),
    }
    io.print("{}", text.starts_with("sosu.toml", "sosu"));
    return 0;
}
```

```text
3 fields, the last is city
NAME
a+b+c
found at byte 6
43
true
```

### 22.2 Vectors and maps

`vec.Vec<T>` and `map.HashMap<K, V>` are classes that grow in the arena given at their creation.

<!-- run -->
```csharp
import std.io;
import std.map;
import std.vec;

i32 main() uses io {
    mut Arena arena = Arena.new(8 * KB);
    defer arena.free();

    vec.Vec<i32>* v = vec.new(&arena);
    for (i32 i in 0..5) {
        v.push(i * i);
    }
    match (v.pop()) {
        some(last) => io.print("popped {}", last),
        none => { },
    }
    io.print("{} items, the second is {}", v.len(), *v.at(1));

    map.HashMap<str, i32>* stock = map.new(&arena);
    stock.put("apples", 3);
    stock.put("pears", 5);
    stock.put("apples", 4);
    io.print("{} kinds", stock.len());
    match (stock.get("apples")) {
        some(n) => io.print("apples: {}", n),
        none => io.print("no apples"),
    }
    match (stock.remove("pears")) {
        some(n) => io.print("removed {} pears", n),
        none => { },
    }
    io.print("{}", stock.has("pears"));
    return 0;
}
```

```text
popped 16
4 items, the second is 1
2 kinds
apples: 4
removed 5 pears
false
```

`Vec` has `push`, `pop`, `get`, `at`, `set`, `insert`, `remove`, `len`, `clear` and `items()`, a view. `HashMap` has `put`, `get`, `at`, `has`, `remove`, `len`, `clear`, and `keys(arena)` and `values(arena)`. The keys need the constraint `Hash`.

### 22.3 Sorting

<!-- run -->
```csharp
import std.io;
import std.sort;

i32 main() uses io {
    mut i32[6] a = [5, 3, 9, 1, 7, 3];
    sort.sort(a[0..6]);
    io.print("{} {} {}", a[0], a[3], a[5]);
    match (sort.search(a[0..6], 7)) {
        some(i) => io.print("7 at {}", i),
        none => io.print("no 7"),
    }
    sort.sort_by(a[0..6], (x, y) => x > y);
    io.print("{} {}", a[0], a[5]);
    return 0;
}
```

```text
1 5 9
7 at 4
9 1
```

`sort`, `sort_by` and `sort_by_key` sort in place without memory, not stable; `stable_sort`, `stable_sort_by` and `stable_sort_by_key` keep the order of equal elements and take an arena.

### 22.4 Mathematics, randomness and time

<!-- run -->
```csharp
import std.io;
import std.math;
import std.random;
import std.time;

i32 main() uses io {
    io.print("{} {}", math.sqrt(2.0) > 1.41, math.floor(2.7));
    mut random.Rng rng = random.seeded(42);
    i64 die = rng.between(1, 6);
    io.print("{}", die >= 1 && die <= 6);
    match (time.date(2026, 11, 8)) {
        some(d) => io.print("{}", d.day_of_year()),
        none => io.print("not a date"),
    }
    io.print("{}", time.is_leap_year(2028));
    return 0;
}
```

```text
true 2.0
true
312
true
```

`random.seeded(seed)` gives the same numbers for the same seed and needs no effect; `random.from_system()` uses the effect `random`. `time.now()` and the clocks use the effect `time`; the calendar functions need none.

### 22.5 Files and the system

<!-- check -->
```csharp
import std.fs;
import std.io;
import std.os;

void report(Arena* arena) uses io {
    for (str a in os.args(arena)) {
        io.print("argument: {}", a);
    }
    match (io.write_file(arena, "notes.txt", "first line\n")) {
        ok => { },
        err(e) => io.print("cannot write"),
    }
    match (io.read_file(arena, "notes.txt")) {
        ok(content) => io.print("{}", content),
        err(e) => io.print("cannot read"),
    }
    match (fs.list(arena, ".")) {
        ok(names) => io.print("{} entries", names.len),
        err(e) => io.print("cannot list"),
    }
}
```

Everything that touches the disk or the system uses `io`, and can fail with an `io.IoError`.

### 22.6 JSON

<!-- run -->
```csharp
import std.io;
import std.json;

i32 main() uses io {
    mut Arena arena = Arena.new(8 * KB);
    defer arena.free();
    match (json.parse(&arena, "{\"name\": \"Ada\", \"langs\": [\"en\", \"fr\"]}")) {
        ok(v) => {
            match (json.get(v, "name")) {
                some(n) => io.print("{}", match (json.as_str(n)) {
                    some(s) => s,
                    none => "?",
                }),
                none => io.print("no name"),
            }
            match (json.get(v, "langs")) {
                some(l) => io.print("{} languages", json.len(l)),
                none => { },
            }
            io.print("{}", json.write(&arena, v));
        },
        err(e) => io.print("{}", json.message(&arena, e)),
    }
    return 0;
}
```

```text
Ada
2 languages
{"name":"Ada","langs":["en","fr"]}
```

### 22.7 Command-line options

<!-- check -->
```csharp
import std.cli;
import std.io;
import std.os;

i32 main() uses io {
    mut Arena arena = Arena.new(8 * KB);
    defer arena.free();
    cli.Command* c = cli.command(&arena, "greet", "Says hello.");
    c.flag("loud", 'l', "shout");
    c.option("name", 'n', "NAME", "who to greet", "world");
    match (c.parse(os.args(&arena))) {
        ok(m) => {
            if (m.flag("loud")) {
                io.print("HELLO, {}!", m.value("name"));
            } else {
                io.print("Hello, {}.", m.value("name"));
            }
        },
        err(e) => io.print("{}", c.message(&arena, e)),
    }
    return 0;
}
```

`c.help(arena)` gives the usage text.

## 23. Threads and Channels

### 23.1 The model

Threads share no memory. A thread runs a function on a *copy* of a value, with an arena of its own, freed when the function returns. Threads talk through channels, which copy what they carry. So there is nothing to lock: data races cannot be written outside `unsafe`.

Only values without pointers cross threads: numbers, `bool`, `char`, and the structs, enums, arrays, options and results made of them. This is the constraint `Send`, and the compiler checks it.

<!-- run -->
```csharp
import std.io;
import std.thread;
import std.channel;

struct Range {
    i64 from;
    i64 to;
    channel.Channel<i64> results;
}

i64 sum_squares(Arena* arena, Range r) uses threads {
    mut i64 total = 0;
    for (i64 i in r.from..r.to) {
        total += i * i;
    }
    bool sent = r.results.send(total);
    return total;
}

i32 main() uses io, threads {
    mut Arena arena = Arena.new(16 * KB);
    defer arena.free();
    channel.Channel<i64> results = channel.new<i64>(4);
    auto a = thread.spawn(&arena, sum_squares, Range{ from: 0, to: 1000, results: results });
    auto b = thread.spawn(&arena, sum_squares, Range{ from: 1000, to: 2000, results: results });
    i64 joined = a.join() + b.join();
    mut i64 received = 0;
    for (i32 i in 0..2) {
        match (results.receive()) {
            some(n) => {
                received += n;
            },
            none => { },
        }
    }
    io.print("{} {}", joined, joined == received);
    results.free();
    return 0;
}
```

```text
2664667000 true
```

### 23.2 The pieces

- `thread.spawn(arena, work, value)` runs `work(its own arena, value)` on a new thread, and returns a handle made in `arena`. `work` is a function, or a lambda without captures.
- `join()` waits for the thread and gives what `work` returned.
- `channel.new<T>(capacity)` makes a channel of `capacity` values. `send(x)` copies `x` in, waiting while it is full, and gives `false` once the channel is closed; `receive()` waits for a value and gives `none` once the channel is closed and empty; `close()` closes it, and `free()` releases it.
- A run-time error in a thread stops the whole program.
- Windows stay on the main thread: a worker gives its results to the interface through a channel.

---

# Part VI. Platforms

## 24. Windows and Widgets

### 24.1 The packages

| Package | Gives |
|---|---|
| `platform` | a window, its events and its loop, on SDL3; dialogs, clipboard, the data folder of the application |
| `render` | drawing in a canvas of pixels: rectangles, rounded corners, lines, text (a built-in font and TrueType fonts), PNG and JPEG images |
| `ui` | widgets in immediate mode, and the `.ui.sosu` files that describe them |
| `i18n` | translations of the texts of an interface |

They are in `packages/` of the release, written in Sosu. SDL3 is the only native library: on Windows, run `packages/platform/fetch-sdl3.ps1` once; on Linux and macOS, install it with your package manager.

### 24.2 A program with a window

A program gives `app.run` a handler, a class that implements `app.Handler`: `on_event` receives each event and returns `false` to close the window; `on_frame` draws a frame in a canvas.

```csharp
import std.io;
import platform.app;
import render.canvas;

class Demo : app.Handler {
    mut f32 x;

    void init() {
        x = 40.0;
    }

    override bool on_event(app.Event e) uses io {
        return match (e) {
            Quit => false,
            Key(code, down, mods) => {
                if (down && code == 0x4000004F) {
                    x += 10.0;  // the right arrow
                }
                return code != 27;  // Escape closes
            },
            _ => true,
        };
    }

    override void on_frame(canvas.Canvas* surface) uses io {
        surface.clear(canvas.Color{ r: 30, g: 30, b: 46 });
        surface.fill(canvas.Rect{ x: x, y: 40.0, w: 120.0, h: 80.0 }, canvas.Color{ r: 255, g: 140, b: 0 });
        surface.text(16.0, 16.0, "Right arrow: move", canvas.Color{ r: 255, g: 255, b: 255 }, 2);
    }
}

i32 main() uses io {
    mut Arena arena = Arena.new(1 * KB);
    defer arena.free();
    Demo* demo = arena.new(Demo);
    return app.run(app.Config{ title: "Demo", width: 640, height: 400 }, demo as app.Handler*);
}
```

The events: `Quit`, `Resized`, `Key`, `MouseMove`, `Click`, `Release`, `Wheel`, `TextInput`, and on phones `Touch` and the safe area of the screen.

### 24.3 Widgets in immediate mode

`ui.widgets` describes the interface at each frame, from the state of the program: there is no tree of widgets to keep in sync. A `Ui` keeps what lasts between frames (the focus, the scroll positions); each frame begins with `widgets.begin` and ends with `finish`. A widget is a call, and an action is its return value:

```csharp
mut widgets.Frame f = widgets.begin(&ui, surface, 0.0, 0.0);
f.column(8.0, 16.0);
f.title("Counter");
if (f.button("Add one")) {
    count += 1;
}
f.end();
f.finish();
```

### 24.4 `.ui.sosu` files

Writing those calls by hand gets long, so an interface can be described in a `.ui.sosu` file, translated by `sosuc` into ordinary Sosu functions that the type checker sees like any other code:

```csharp
// src/screen.ui.sosu
import app.state;

style Muted { color: #808080 }

component TaskRow(state.Task* t) {
    Row(gap: 8) {
        Checkbox(bind: t.done)
        Text(t.title)
    }
}

window Tasks(keeps state.Board* board) {
    title: "Tasks"
    width: 480
    height: 360

    Column(gap: 8, padding: 16) {
        Title("Today")
        TextField(bind: board.draft, width: 300, on_enter: board.add)
        for (i32 i in 0..board.count) {
            TaskRow(&board.tasks[i])
        }
        if (board.count == 0) {
            Text("Nothing to do", style: Muted)
        }
    }
}
```

- `window Name(params) { properties, then nodes }` becomes `pub void Name(widgets.Frame* ui_frame, params)`, and its properties `NameConfig()`, for `app.Config`.
- `component Name(params) { nodes }` is a reusable piece, used as a node.
- Nodes hold nodes; `if`, `else` and `for` work as in Sosu.
- `bind:` gives a widget the memory it edits; such a window parameter is marked `keeps`.
- `on_click:`, `on_enter:` and `on_change:` take a method or a call: `board.add`, `board.remove(i)`.
- A literal text is translated (`i18n`), and `{name}` in it is filled by the property of that name, which must be a `str`; `plural:` and `count:` choose the form of a text by a number.
- An unknown node or property is a compile error, with a suggestion.

The main nodes:

| Node | Widget |
|---|---|
| `Column`, `Row`, `Grid` | layout, with `gap`, `padding`, `align` |
| `Text`, `Title`, `Image`, `Space` | display |
| `Button`, `Checkbox`, `Radio`, `Slider` | actions and choices |
| `TextField`, `TextArea`, `NumberField`, `MoneyField`, `DateField`, `Dropdown` | input |
| `List`, `Table`, `Tree`, `Scroll` | collections, virtualized when long |
| `Group`, `Tabs`, `Dialog` | containers |
| `Progress` | a progress bar |
| `MenuBar`, `Menu`, `MenuItem`, `ContextMenu` | menus |
| `TitleBar` | a title bar drawn by the program |

`theme` blocks give colors and sizes for each widget and state (hover, focus), and `style` blocks name a look for `style:`. The [specification, chapter 10](SPEC.md), describes every node, theme and translation; `examples/form`, `examples/themes`, `examples/containers` and `examples/workspace` show them working.

### 24.5 Phones

The same program runs on Android and iOS: `sosuc package --target android` makes an APK, and `--target ios-sim` an application for the iOS simulator (on a Mac). Files go through the platform (`app.data_folder`, `app.read_asset`), never through fixed paths. `[app]` of `sosu.toml` gives the identifier, the name, the version, the icon and the asset files of the application.

## 25. C, and Sosu from Other Languages

### 25.1 Calling C

Declare the C functions in an `extern "C"` block, with the Sosu types of their C types. A call to C is `unsafe`, since the compiler cannot check what C does; wrap it in a safe function, and the rest of the program never sees it.

<!-- run -->
```csharp
import std.io;

extern "C" {
    f64 cbrt(f64 x);
    i32 abs(i32 x);
}

f64 cube_root(f64 x) {
    mut f64 r = 0.0;
    unsafe {
        r = cbrt(x);
    }
    return r;
}

i32 main() uses io {
    io.print("{}", cube_root(27.0));
    mut i32 a = 0;
    unsafe {
        a = abs(-5);
    }
    io.print("{}", a);
    return 0;
}
```

```text
3.0
5
```

The types of C, in Sosu:

| C | Sosu |
|---|---|
| `int`, `unsigned` | `i32`, `ui32` |
| `long long`, `size_t` | `i64`, `usize` |
| `intN_t`, `uintN_t` | `iN`, `uiN` |
| `float`, `double` | `f32`, `f64` |
| `char` | `ui8` |
| `bool` | `bool` |
| `T*` | `T*`, or `T*?` when it may be NULL |
| a function pointer | a function type |
| a `struct` | a `struct` with the same fields in the same order |

`long` is 32 bits on Windows and 64 elsewhere: avoid it in bindings. A C string is converted explicitly: `s.to_cstr(arena)` gives a `ui8*` ending with a NUL byte.

### 25.2 `sosuc bindgen`

Writing declarations by hand is error-prone. `sosuc bindgen` reads a C header through the C compiler's preprocessor, and writes a Sosu module:

```bash
sosuc bindgen vendor/thing/include/thing.h -I vendor/thing/include --prefix thing_ -o src/thing_c.sosu --check
```

Functions become an `extern "C"` block (each `uses io`), structs become structs, C enums and `#define` constants become constants, function pointers become function types. `--check` writes a C file of static assertions that compares every size and offset with the header. The module is committed like any source; regenerate it when the header changes.

### 25.3 Linking

C libraries are declared in `sosu.toml`, never in the code, so that the code stays the same on every target:

```toml
[link]
libs = ["thing"]                # thing.lib with MSVC, -lthing elsewhere
c = ["vendor/thing/thing.c"]    # or give the C sources, compiled with the program
include = ["vendor/thing/include"]

[link.windows]
paths = ["vendor/thing/lib/x64"]
copy = ["vendor/thing/lib/x64/thing.dll"]

[link.macos]
frameworks = ["Cocoa"]
```

Paths are relative to the package. Linking is transitive: a program links the libraries of its dependencies without naming them. `subsystem = "window"` removes the console of a windowed program in release builds on Windows.

### 25.4 Sosu from other languages

A Sosu library can be used from C, C++, C#, Java, Python, Rust, Go or Swift through the C ABI. Mark the functions to offer with `export "C"`, and build with `sosuc build --lib` (a static library) or `--lib=shared` (a shared one); `sosuc` writes the C header next to it.

<!-- check -->
```csharp
pub struct Vec2 {
    pub f32 x;
    pub f32 y;
}

export "C" f32 geometry_dot(Vec2 a, Vec2 b) {
    return a.x * b.x + a.y * b.y;
}
```

The functions keep their names in C. A run-time error inside the library does not stop the host process: each export runs under a guard, returns a zero value after an error, and `<package>_error()` tells the host what happened.

## 26. Building and Shipping

### 26.1 Targets

`--target os-arch` builds for another system or architecture:

| Target | Architectures | Needs |
|---|---|---|
| `windows` | x64, arm64 | MSVC or clang |
| `linux` | x64, arm64 | gcc or clang |
| `macos` | arm64, x64 | clang (Xcode Command Line Tools) |
| `android` | arm64 (x86_64 for the emulator) | the Android NDK and SDK |
| `ios` | arm64, and the simulator | Xcode, on a Mac |

```bash
sosuc build --target linux-arm64
```

### 26.2 Releases

`sosuc package` makes a folder with the program, its libraries (the DLLs of `copy`) and the files of `[app] assets`, and an archive of it, named after the system and the architecture:

```bash
sosuc package examples/workspace
```

On phones, `--target android` gives an APK signed with the debug key, and `--target ios-sim` an application for the simulator. Store releases are signed by the developer, with the tools of each platform.

### 26.3 Performance

`sosuc` itself is fast: a program of 80 000 lines is checked in a few hundredths of a second, and most of the time of a build is the C compiler. A release build is optimized by the C compiler; index checks stay on unless `--checks=off`. Generics are monomorphized, and non-virtual calls are direct, so abstractions cost nothing at run time unless they say so (`virtual`, interfaces).

---

# Appendices

## A. Quick Reference

```text
i32 x = 1;  mut i32 y = 2;  auto z = 3;      variables
pub i32 MAX = 8;                            constant
i32 f(i32 a, mut i32 b) uses io { }         function, effects
i32!E g()   i32? h()                        can fail, can be absent
try g()   match (h()) { some(v) => ..., none => ... }
void f(keeps T* p)   void m() keeps self    kept beyond the call
struct P { mut i32 x; }                     value, copied
enum E { A, B(i32 n) }                      sum type
class C : Base, Iface { void init() { } }   in an arena, through a pointer
interface I { void m(); }                   signatures
virtual   override   base.init(...)         redefinition
T* p = &x;   *p   p.field                   pointer, never null
T[N]   T[]   x[a..b]   [1, 2]   [0; 8]      arrays and views
x as f32   p as? Sub*                       conversions
(a) => a + 1   fn(i32) -> i32 uses io       lambda, function type
T f<T: Ordered + Hash>(T a)                 generic
for (T x in e) { }   for (i32 i in 0..n)    loops
defer a.free();                             at the end of the block
unsafe { }                                  outside the checks
test "name" { assert(...); }                test
```

## B. The Most Frequent Errors

| Code | Means | Usually |
|---|---|---|
| E0331 | not the type expected | add `as`, or fix the type |
| E0335 | not mutable | declare it `mut` |
| E0340 | an effect is missing | add it to `uses` |
| E0346 | a path without `return` | return a value on every path |
| E0356 | a value was moved | pass a pointer |
| E0369 | a lambda changes or owns a capture | capture a pointer |
| E0370 | a closure leaves its function | make it with `arena.closure` |
| E0371 | a pointer into the function escapes | allocate in an arena |
| E0372 | memory used after its `free()` | keep the arena alive, or give a new value |
| E0373 | returned memory ends with the function | receive an `Arena*` |
| E0374 | stored where it may outlive what it points to | use one arena |
| E0375 | given to `keeps` with memory that ends sooner | give memory that lives as long |
| E0376 | stored beyond the call without `keeps` | write `keeps` |
| E0378 | a field left in the old memory of an arena | give it a value in the new one |
| E0405 | a git dependency not fetched | run `sosuc check` once |

All the codes are in [errors.md](errors.md).

## C. From Another Language

| You write in C, C++ or C# | In Sosu |
|---|---|
| `p->x`, `A::b` | `p.x`, `a.b` |
| `x++` | `x += 1` |
| `NULL`, `null`, `nullptr` | `T?`, `none` |
| exceptions, `errno` | `T!E`, `try`, `match` |
| `malloc`/`free`, `new`/`delete`, a GC | an arena |
| `switch` | `match`, exhaustive |
| `(float)x`, implicit conversions | `x as f32` |
| `const` | the default; `mut` to change |
| global variables, `static` | constants, and values passed down |
| overloads, default arguments | distinct names |
| macros, templates | generics, constants |
| `virtual` by default (C#, Java) | `virtual` when asked |
| threads sharing memory | threads with copies, and channels |
