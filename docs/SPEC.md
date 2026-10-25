# Sosu — Specification v0

Extension: `.sosu` — Compiler: `sosuc` (written in C99, libc only)

## 1. Goals

- Native, low-level, statically typed language.
- Syntax readable for a C++/C# developer.
- Cross-platform: Windows, Linux, macOS, Android, iOS (see §11).
- Memory-frugal: no GC, no reference counting.
- Declarative user interface, provided by packages (see §10).
- Compiler with no dependency other than the libc.

## 2. Design principles

1. No hidden cost: no implicit allocation or copy.
2. Value types (`struct`) are the foundation; class instances live in arenas (see §5.2).
3. Immutable by default (`mut` to modify).
4. Syntax that is simple to parse (no generics in v0).
5. The whole language is in English: keywords, standard library, std identifiers, compiler messages.
6. Symbols inherited from C/C++, with their inconsistencies fixed (see §5.1).
7. Local readability: a fragment of code can be understood without knowing the whole project (no implicit conversion, no hidden control flow, no undefined behavior).
8. Absence and failure are in the types (`T?`, `T!E`), never in exceptions or implicit null pointers.
9. Small core, one way to do things: no overloading, no macros, no preprocessor.
10. Intent is checkable: contracts and tests are part of the language (see §7).
11. The same code for every platform: everything that depends on the OS is isolated in packages (see §8 and §10).

## 3. Lexicon

- Source files are UTF-8 (a leading BOM is allowed). Identifiers and symbols are ASCII; other characters only appear in strings, characters and comments.
- Comments: `// line` and `/* block */`. Block comments do not nest.
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`.
- v0 keywords (English):
  `struct class interface enum import extern pub return if else while for in break continue defer mut auto true false as match try requires ensures test uses unsafe virtual override self base`
- Reserved for later: `comptime`.
- Contextual words of `.ui.sosu` files (not reserved elsewhere): `window`, `component`, `style`, `theme`.
- Predefined constructors (not reserved): `some`, `none`, `ok`, `err`.
- `fn` is not a keyword: `fn(` in a type starts a function type (§5.1).
- Literals: integers (`42`, `0xFF`, `0b1010`), floats (`3.14`), strings (`"..."`), characters (`'a'`).

### 3.1 Literals in detail

- Integers: decimal, hexadecimal (`0x`, lowercase prefix) or binary (`0b`), at most 2^64 - 1. A decimal literal cannot start with `0` (`007` is an error): C reads it as octal, Sosu has no octal literals.
- Floats: digits, a dot and digits (`3.14`), and an optional exponent (`1e5`, `2.5e-3`, lowercase `e`). A digit is required after the dot, so `0..n` is a range and `3.abs()` a method call.
- No type suffix: `300u8` is an error, the conversion is written `300 as u8`.
- Strings stay on one line. Escapes, in strings and characters: `\n \t \r \0 \\ \' \"`, `\xNN` (ASCII only, `\x00` to `\x7F`, so that a `str` stays valid UTF-8) and `\u{N}` (1 to 6 hexadecimal digits, a Unicode scalar value).
- A character literal holds exactly one code point.
- `++`, `--` and `::` are recognized only to report an error that gives the Sosu form (§5.1).

## 4. Types

| Category | Types |
|---|---|
| Integers | `i8 i16 i32 i64 u8 u16 u32 u64 isize usize` |
| Floats | `f32 f64` |
| Others | `bool char void` |
| String | `str` (valid UTF-8, pointer + length, not owned) |
| Pointer | `T*` (never null; possibly absent pointer: `T*?`) |
| Option | `T?` (values `some(x)` or `none`) |
| Result | `T!E` (values `ok(x)` or `err(e)`) |
| Array | `T[N]` (fixed size), `T[]` (view: pointer + length); `x[a..b]` is the view of the elements `a` to `b - 1` of an array or a view |
| User-defined | `struct`, `enum` (sum types, see §7.1) |

Conversions: always explicit with `as` (`x as f32`).

### 4.1 Text and UTF-8

- A `str` always holds valid UTF-8, without a null terminator; its length is in bytes.
- `char` is a Unicode code point (32 bits).
- Iteration: `for (char c in s.chars())`; byte access: `s.bytes[i]`.
- Building a `str` from arbitrary bytes can fail: `str.from_bytes(u8[]) -> str!Utf8Error`.
- C strings: explicit conversion (`s.to_cstr(arena)` allocates in an arena; `str.from_cstr(u8*)` is `unsafe`). `to_cstr` takes an `Arena` or an `Arena*` and returns a `u8*` ending with a NUL byte. `Utf8Error` (one variant, `Invalid`) is declared in `core.prelude`. `str.from_cstr` is not implemented yet.

## 5. Declarations and syntax

C/C++ style: type first. `auto` for inference.

```sosu
import std.io;

struct Point {
    f32 x;
    f32 y;

    f32 length_squared() { return x * x + y * y; }
}

i32 add(i32 a, i32 b) {
    return a + b;
}

i32 main() uses io {
    i32 n = 3;            // immutable
    mut i32 total = 0;    // mutable
    for (i32 i in 0..n) {
        total += add(i, 1);
    }
    auto p = Point{ x: 3.0, y: 4.0 };
    io.print("length² = {}", p.length_squared());
    return 0;
}
```

Parsing note: a statement starting with `Ident Ident` is a declaration, otherwise an expression
(enough as long as there are no generics). More precisely, a statement is a declaration when it
starts with `mut`, `auto`, or a type followed by a name (`Point* p`, `u8[4] buf`, `i32!E r`).

Every variable receives its value where it is declared: `i32 x;` is an error. There are no global
variables in v0.

Control flow: `if / else`, `while`, `for (init; cond; step)`, `for (T x in a..b)`, `break`, `continue`.

Lambdas (v1): `(i32 a) => a + 1`; the body is an expression or a `{ ... }` block.

### 5.1 Symbols and operators

C/C++ base, with these fixes:

| Inconsistency in C/C++ | Sosu rule |
|---|---|
| `.` and `->` | `.` only, pointers are dereferenced automatically |
| `&` = reference, address-of and bitwise AND | no references; `&x` (unary) = address, `a & b` (binary) = bitwise AND |
| `a & b == c` badly parenthesized by default | bitwise operators bind tighter than comparisons |
| `if (x = 1)` accepted | assignment is a statement, not an expression |
| `++` / `--`, undefined evaluation order | removed (`x += 1`); evaluation is left to right, defined |
| `i32* a, b`: `b` is not a pointer | one variable per declaration |
| `int a[4]`, `int (*f)(int)` | type always on the left: `i32[4]`, `fn(i32) -> i32` |
| `::` and `.` | `.` only (modules, members) |
| `const`, `static`, `volatile`... | `mut` only in v0 |
| implicit conversions | `as` only |
| undefined signed overflow | never undefined: in debug, an error (stops with a message); in release, modulo 2^n arithmetic; explicit `wrapping_*`, `checked_*` (returns `T?`) and `saturating_*` operations; division by zero = stop |
| `switch` with fall-through | replaced by an exhaustive `match`, without fall-through (see §7.1) |
| comma operator | removed |
| `if (c) x;` without braces | braces are mandatory |
| `a < b < c` accepted (unexpected meaning) | compile error |

Operators:
- Arithmetic: `+ - * / %`
- Comparison: `== != < <= > >=`
- Logical: `&& || !`
- Bitwise: `& | ^ ~ << >>` (`>>` arithmetic on signed, logical on unsigned)
- Assignment (statement): `= += -= *= /= %= &= |= ^= <<= >>=`
- Access: `.` `[]`; address / dereference: `&` `*`
- Conversion: `as`; conversion to a subclass: `as?` (returns `T*?`); ranges: `..` (exclusive), `..=` (inclusive); ternary: `?:`

Precedence (tightest first):

1. Postfix: `()` `[]` `.`
2. Unary: `! ~ - & *` and `as`
3. `* / %`
4. `+ -`
5. `<< >>`
6. `&`
7. `^`
8. `|`
9. `== != < <= > >=` (not chainable)
10. `&&`
11. `||`
12. `..` `..=` (not chainable: `0..n + 1` is `0..(n + 1)`)
13. `?:`

Details:

- `as` applies to the result of the unary operators: `-x as f32` is `(-x) as f32`.
- After `as`, `*` and `?` belong to the type unless an operand follows: `p as u8*` converts to a pointer, `x as i32 * 2` multiplies, `x as bool ? a : b` is a conditional.
- A struct literal is written `Point{ x: 1.0, y: 2.0 }` (trailing comma allowed). It cannot appear in `requires` / `ensures`, where the brace opens the function body.
- Function types: `fn(i32, i32) -> i32 uses io`; the arrow and the return type are mandatory (`-> void`). In a function type, the `uses` list only continues after a comma with a predefined effect (§7.4), so that the comma is not confused with a parameter separator.

### 5.2 Classes, inheritance and interfaces

`struct`: value type, copied, no inheritance. `class`: instance allocated in an arena, handled through a pointer (`T*`), never copied.

```sosu
interface Drawable {
    void draw(Render* r) uses io;
}

class Widget : Drawable {
    f32 x;
    f32 y;

    void init(f32 x, f32 y) { self.x = x; self.y = y; }

    virtual void on_click() uses io { }
    override void draw(Render* r) uses io { /* ... */ }
}

class Button : Widget {
    str label;

    void init(str label) { base.init(0.0, 0.0); self.label = label; }

    override void on_click() uses io { /* ... */ }
}

Button* b = arena.new(Button, "OK");
Widget* w = b as Widget*;     // to the parent class
w.on_click();                  // virtual call (announced by `virtual`)
Button* back = w as? Button*; // `as?` returns Button*?
```

- **Single inheritance and interfaces**: `class Dog : Animal, Walker`. At most one parent class, which must come first; the other elements are interfaces.
- **Methods are non-virtual by default** (direct call). `virtual` allows overriding; `override` is mandatory to override a method or implement an interface method, and it is an error if there is nothing to override. The cost of a virtual call therefore only appears where the code announces it.
- **Construction**: an `init(...)` method; `arena.new(Type, args)` allocates then calls `init`. The `init` of a derived class starts with `base.init(...)`. `self` is the instance, `base` the parent class.
- **Fields**: assignable in `init`; elsewhere, they must be declared `mut`.
- **No destructor** in v0: external resources are released with `defer`, memory with the arena.
- **Conversions**: to the parent class or an interface with `as`; to a subclass with `as?` (checked at run time, returns `T*?`).
- **Interfaces**: signatures only; only classes implement them (see §14.2).

### 5.3 Names, values and typing rules

- **Names in a function**: local variables and parameters first, then (in a method) the fields and methods of `self`, written without `self.` as in the examples (`x * x`), then the declarations of the module, then the built-in names below. A local variable cannot reuse the name of another local or parameter of the same function.
- **Built-in names**, available everywhere without import: `some`, `none`, `ok`, `err`, `assert`, `min`, `max`, `abs`, and the modulo operations of §5.1 `wrapping_add`, `wrapping_sub`, `wrapping_mul` (two integers of the same type; `checked_*` and `saturating_*` are not implemented yet). They are part of the compiler, since they work for several types and Sosu v0 has no generics.
- **The prelude**: `core.prelude` (`lib/core/src/prelude.sosu`) is loaded with every program, and its public names are used without import, after the names of the module (which can hide them). It is ordinary Sosu, kept small: for now `Utf8Error`.
- **`self`** is a pointer to the instance (`T*`) in the methods of structs and classes; `base` is the same pointer seen as the parent class.
- **Number literals take the expected type**: in `i64 x = 3;`, `3` is an `i64`; in `a + 1` with `a: u8`, `1` is a `u8`. Without an expected type, an integer literal is an `i32` and a float literal an `f64`. A literal that does not fit is an error. An integer literal is never a float: write `3.0`.
- **Operators need operands of the same type**: `i32 + i64` is an error, convert one side with `as`. Comparisons give a `bool`; `&&`, `||` and `!` need `bool`; bitwise operators need integers. Shifts accept any integer as the shift amount.
- **Mutability**: assigning to a variable or a parameter requires it to be declared `mut`. Assigning to a field requires the field to be `mut`, except in `init`; through a value (not a pointer), the variable holding it must also be `mut`. `*p = v` and the elements of a slice can always be assigned. A view `x[a..b]` shares the elements of `x` without copying them (checked: `a <= b <= x.len`); taking one of an array is like taking its address, so the array must be `mut`.
- **Taking an address** `&x` follows the same rules, since the pointer allows writing: `x` must be `mut` (and a field must be `mut`). Pointers that only allow reading may come later.
- **Array literals**: `[1, 2, 3]` (trailing comma allowed) and `[value; count]` (count: an integer literal). The element type comes from the expected array type, or else from the first element; the number of elements must match the type (`i32[3] a = [1, 2];` is an error).
- **`result`** is not a keyword: in `ensures` it names the returned value; everywhere else it is an ordinary name.
- **Enum values** are written `Shape.Circle(1.0)` or `Shape.Empty`; the name of the enum may be left out when the expected type is known (`Shape s = Circle(1.0);`), as in `match` patterns.
- **Statements**: an expression statement must be a call or a `match`; `x == 1;` is an error. The value of a call returning `T!E` must be used (with `try`, `match`, or a variable).
- **Functions** that return a value must end every path with `return`.
- **Options and results**: `none`, `ok(x)` and `err(e)` need an expected type (`i32? x = none;`); `some(x)` takes the type of `x`. `try` applies to a `T!E` value, in a function that returns a result with the same error type `E`; for absence, use `match`. A function that returns nothing but can fail returns `void!E`, and ends with `return ok();`.
- **Conversions with `as`**: between numeric types, `char` and `bool` to integers, `u8` to `char`, and a class pointer to a parent class or interface pointer. Pointer reinterpretation (`p as u8*`) and integer / pointer conversions need `unsafe`. `as?` converts a class or interface pointer to a subclass pointer and returns `T*?`.
- **`io.print`** is built into the compiler: `io.print("x = {}, y = {}", x, y)` checks at compile time that the format is a string literal with one `{}` per value, and that each value is a number, a `bool`, a `char` or a `str`. It ends the line. A float always prints with a dot (`25.0`).
- **`%`** works on integers only. **`==`** compares numbers, `bool`, `char`, `str` (by content), pointers, and enums without data; enums with data are compared with `match`.
- **Conversions at run time**: between integers, `as` keeps the low bits (modulo 2^n, `300 as u8` is an error only for a literal); from a float to an integer, a value out of range stops the program.
- **Run-time errors** (overflow, division by zero, index out of bounds, failed `assert` or contract) print the position and stop the program with the exit code 101; in `sosuc test`, they fail the current test only.

## 6. Memory

- No GC, no reference counting.
- **Arenas**: a first-class allocator.

```sosu
Arena arena = Arena.new(1 * MB);
defer arena.free();                 // runs at the end of the scope

Point* p = arena.alloc(Point);      // lives until arena.free()
```

- `alloc` stops the program with a message when memory runs out; `try_alloc` returns `T*?` to handle that case yourself.
- `Arena` is a built-in type, with `KB`, `MB` and `GB` (`usize`) for sizes. `Arena.new(size)` gives the size of its blocks (a bigger allocation gets its own block). Its methods: `alloc(T)` and `try_alloc(T)` give zeroed memory, so `T` cannot contain a pointer (never null) nor be a class; `new(T, args)` creates a class, or a struct, and calls its `init` with `args` (without `init`, it takes no argument and the type must be valid when zeroed); `free()` releases every block, and using the arena afterwards stops the program.
- The type given to `alloc`, `try_alloc` and `new` is written `Type` or `module.Type`.
- `alloc_slice(T, count)` gives a zeroed `T[]` of `count` elements (same rule on `T` as `alloc`), for sizes known at run time only.
- An `Arena` owns its blocks: it is moved, not copied (below).
- `defer`: runs a statement when leaving the scope (in reverse order).
- **Move by default** for types owning a resource; explicit copy with `.copy()`.
  - In v0, the types owning a resource are `Arena` and the values containing one (struct fields, array elements, option and result values, enum variant fields).
  - Using such a variable as a value (initializing a variable, assigning, passing it by value, returning it, putting it in a literal) moves it: the variable can no longer be used afterwards, until it is assigned again. Calling its methods, taking its address and reading its fields do not move it.
  - The check follows the order of the source: after a move in a branch, the variable is no longer usable after the `if`. A variable declared outside a loop cannot be moved inside it.
  - A value cannot be moved out of a field, an element or a pointer: pass a pointer instead.
  - `x.copy()` gives a new value without moving `x` (also through a pointer). Each `Arena` in it is a new, empty arena with the same block size; every other member is copied as is. Pointers are copied too, so they still point into the arenas of `x`: they stay valid only as long as those arenas. Copying a freed arena stops the program. A struct may declare its own `copy` method, which is called instead.
  - Values without a resource have no `.copy()`: they are already copied by value.
- Plain `struct`s (without a resource) are copied by value.
- No hidden allocation: no global allocator; every function that allocates explicitly receives an `Arena*`.
- No borrow checker in v0: safety relies on arenas and `defer`.
- Research track (v1): a simple region rule checked by the compiler, without lifetime parameters. A pointer coming from an arena can neither be stored in a structure that outlives the arena, nor be returned beyond its scope.

## 7. Reliability and readability

### 7.1 Sum types and `match`

An `enum` can carry data. `match` must be exhaustive: a forgotten case is a compile error. `_` is the default case, written explicitly. An arm is an expression or a block; arms are separated by commas. Patterns are `_`, `Variant` and `Variant(a, b)`. `match` is an expression; used as a statement, it ends with its `}` (no `;`).

```sosu
enum Shape {
    Circle(f32 radius),
    Rect(f32 w, f32 h),
    Empty,
}

f32 area(Shape s) {
    return match (s) {
        Circle(r) => 3.14159 * r * r,
        Rect(w, h) => w * h,
        Empty => 0.0,
    };
}
```

### 7.2 Options and errors

No exceptions, no implicit `null`. Absence is `T?`, failure is `T!E` (`E` is an ordinary `enum`). `try` propagates the error to the caller and makes that early return visible.

```sosu
enum ParseError { Empty, NotANumber }

i32!ParseError parse(str s) { ... }

i32!ParseError twice(str s) {
    i32 n = try parse(s);       // on error: return err(...)
    return ok(n * 2);
}

i32? find(i32[] items, i32 target) { ... }   // some(index) or none
```

### 7.3 Contracts and tests

```sosu
i32 clamp(i32 x, i32 lo, i32 hi)
    requires lo <= hi
    ensures result >= lo && result <= hi
{
    if (x < lo) { return lo; }
    if (x > hi) { return hi; }
    return x;
}

test "clamp stays in range" {
    assert(clamp(15, 0, 10) == 10);
}
```

- `requires` / `ensures`: checked in debug mode (stops with a clear message), configurable in release (`--contracts=check|off`).
- `test`: built-in test block, run by `sosuc test`.

### 7.4 Effects visible in signatures

- Allocation: visible through the `Arena*` parameter (see §6).
- Failure: visible through `!E` in the return type.
- Interactions with the outside world: `uses` clause, checked by the compiler from v0.

Predefined effects:

| Effect | Covers |
|---|---|
| `io` | console, files, network, windows and display |
| `time` | reading the clock |
| `random` | random generators |
| `threads` | creating threads, channels, locks |

Rules:

- A function only calls functions whose effects are included in its own; without `uses`, it has no effect.
- Effects are always written, never inferred: the signature is enough to know them.
- They are part of the function type (`fn(i32) -> i32 uses io`). An overriding method (`override`) cannot add an effect to the method it overrides.
- `extern "C"` functions declare their effects by hand; `unsafe` does not bypass effects.
- `test` blocks may use every effect.
- Known limitation: no effect polymorphism in v0. A higher-order library function (`map`, for example) only accepts callbacks without effects; generics (v1) will make it possible to abstract over them.

```sosu
void log(str msg) uses io { ... }

i32 main() uses io {
    log("hello");
    return 0;
}
```

### 7.5 Compiler diagnostics

Each error has a stable code, a precise position, an explanation and, when possible, a suggested fix. Two formats: readable text and JSON (`--format=json`).

```
error[E0312]: non-exhaustive match
  --> shapes.sosu:14:12
   |
14 |     return match (s) {
   |            ^^^^^ missing case: Empty
   = help: add `Empty => ...` or `_ => ...`
```

### 7.6 Canonical formatter and minimal core

- `sosuc fmt`: a single style, no configuration option. It prints the file from its syntax tree, so a file must parse to be formatted, and puts every comment back where it was. The style:
  - 4 spaces of indentation; `{` at the end of the line, `}` on its own line; a non-empty block always spans several lines, an empty one is `{ }`;
  - one statement per line; spaces around binary operators and after commas, none inside parentheses or brackets; struct literals on one line: `Point{ x: 1.0, y: 2.0 }`;
  - parentheses only where the precedence needs them (`(a + b) * c`, `-(x as f32)`, `(p as u8*) + 1`);
  - at most one blank line, where the source has one; none after `{`;
  - a comment on its own line stays on its own line; a comment after code stays at the end of the line, after two spaces;
  - a function with contracts puts each `requires` / `ensures` on its own line and `{` alone below them;
  - an enum written on one line stays on one line; otherwise one variant per line, each followed by a comma;
  - long lines are not broken in v0.
- No function or operator overloading, no macros, no preprocessor, no exceptions, no implicit conversion.

### 7.7 `unsafe` and debug checks

- `unsafe { ... }` is the only place where pointer arithmetic, raw pointer conversions (`p as T*`) or unwrapped calls to C are allowed. A function marked `unsafe` can only be called from an `unsafe` block.
- Outside `unsafe`, a `T*` always points to a valid value.
- In debug, everything is checked: array and slice bounds, variables initialized before being read, integer overflows, contracts.
- In release, bounds stay checked by default (`--checks=off` disables them); contracts and overflows are not.
- Options of `sosuc build`, `run` and `test`: `--release` (optimized C, modulo arithmetic, no contracts), `--checks=on|off` (indexes), `--contracts=check|off` (`requires` / `ensures`). Division by zero always stops the program, as do `assert` failures.

```sosu
unsafe {
    u8* p = base as u8*;
    u8 first = *(p + 4);
}
```

## 8. Engine, library and packages

Rule: the **compiler** contains what cannot be written in Sosu, the **library** what can be written in Sosu, and **packages** everything heavy or optional.

| Level | Content |
|---|---|
| Compiler | basic types, `struct`, `enum`, `match`, `T?`, `T!E`, `try`, `defer`, contracts, `test`, `unsafe`, C interop, intrinsics (`sizeof`, `alignof`, memory operations), modules, `fmt`, diagnostics |
| `core` (no OS, no libc) | `min` / `max` / `abs`, checked arithmetic, `str` and slice utilities, `Arena`, `assert` |
| `std` (OS layer) | files, input / output, `print`, time, environment, processes, `std.app` (application data, cache and document folders, mobile sandbox included), randomness |
| Packages | `platform`, `render`, `ui`, networking, HTTP, JSON, regex, audio, 2D graphics, cryptography, C bindings |

- Without generics, `Vec<T>` and `HashMap<K, V>` cannot be written in Sosu: v0 is limited to slices and arenas; containers arrive with generics (v1, monomorphized).
- Modules and packages: see §8.1.

### 8.0 The standard library in v0

`core` and `std` are written in Sosu, on the C library through `extern "C"` (§9), like any package: the compiler only provides what cannot be written in Sosu v0 (`io.print`, `min`, `max`, `abs`, the text conversions of §4.1, arenas). The runtime gives `std` two small functions where C differs between systems (`errno`, file sizes). Available so far:

- `std.io`: `io.print(...)`; `io.read_file(arena, path) -> str!IoError` (the whole file as text, allocated in the arena); `io.read_bytes(arena, path) -> u8[]!IoError` (the same, as bytes: a font, an image); `io.write_file(arena, path, text) -> void!IoError`. `IoError` is `NotFound`, `PermissionDenied`, `InvalidUtf8` or `Other`.
- `std.time` (effect `time`): `time.monotonic_ns()` and `time.monotonic_ms()` (a clock that never goes backwards, for durations), `time.unix_seconds()` (the wall clock), `time.sleep_ms(ms)`.
- `std.random`: `random.Rng`, a generator holding its state (SplitMix64; not for cryptography): `next()` (64 bits), `below(bound)`, `between(low, high)`, `unit()` (a float in [0, 1)). `random.seeded(seed)` gives the same numbers for the same seed, without effect; `random.from_system()` seeds it differently on each run, with the effect `random`.
- `core.prelude`: `Utf8Error` (§5.3).

### 8.1 Packages and manifest

A package is a folder with a `sosu.toml` manifest (a subset of TOML):

```
myapp/
  sosu.toml
  src/
    main.sosu
    login.ui.sosu
  tests/
```

```toml
[package]
name = "myapp"
version = "0.1.0"

[dependencies]
ui = { git = "https://git.example/sosu-ui", tag = "v0.1.0" }
shared = { path = "../shared" }
```

- `import` starts with the package name: `import ui.button;` refers to `src/button.sosu` in the `ui` package; `import myapp.login;` refers to a file of the current package. `core` and `std` are implicit.
- An import is used through the last name of its path, so that the origin of every name is visible where it is used (principle 7): after `import geometry.vector;`, the code writes `vector.Vec2` and `vector.length(v)`; after `import std.io;`, `io.print(...)`. The names of the current module are used directly. Two imports cannot end with the same name, and a declaration cannot have the name of an import.
- More generally, `import pkg.a.b;` refers to `src/a/b.sosu` of `pkg`, which must be the current package, one of its dependencies, or `core` / `std`. `import pkg;` alone is an error: an import always names a module.
- The entry file of a package is `src/main.sosu` for an application, or `src/lib.sosu` for a library. The compiler loads the modules reachable by `import` from it.
- The modules of a package may import each other.
- The `core` and `std` packages are found in `$SOSU_HOME/lib`, or else in `../lib` next to `sosuc`: the folder of the program run, or, when it was run by its name alone (an editor runs `sosuc lsp`), the folder of PATH where it was found.
- `sosu.toml` syntax: comments (`#`), strings, one-line lists of strings, and one-line inline tables of strings. Sections: [package], [dependencies], [link] and [link.<target>] (§9.1). `[package]` requires `name` and `version`. Package names, and dependency keys, are identifiers since `import` uses them; the key of a dependency must be the name of the package it points to. `core` and `std` are reserved.
- Visibility: private by default; `pub` exports a type, a function or a method.
- Dependencies are sources compiled with the project. `sosu.lock` pins the exact versions (tag or commit).
- In v0, a dependency comes from a local path or a Git repository; a central registry will come later.
- A Git dependency names a `tag` or a `rev` (a commit, possibly abbreviated); a branch is not accepted, since it moves. The compiler calls the `git` command, which must be installed: the repository is cloned into `build/deps/<name>-<commit>` of the root package, once per commit. A URL without a scheme (`../ui`, `C:/repos/ui`) is a local repository, relative to the package that declares it.
- `sosu.lock`, next to the `sosu.toml` of the root package, is written by the compiler and kept in version control. One line per Git dependency of the program, transitive ones included: `<name> <url> tag|rev <tag or rev> <commit>`. While a dependency keeps its URL and its `tag` or `rev`, the commit of the lock is used, even if the tag has moved since, and nothing is fetched once the clone exists. Changing the `tag` or `rev` fetches the new version and updates the lock; it is not written when a dependency has an error. The locks of the dependencies themselves are not read.
- URLs, tags and revs are passed to the shell: they may contain letters, digits and `-._~:/@+=,` only (and `\` in URLs).
- No circular dependencies between packages.
- `platform`, `render` and `ui` are packages like any other.

## 9. C interop

```sosu
extern "C" {
    i32 puts(u8* s);
}
```

- `struct`s are compatible with the C layout.
- An `extern` call is `unsafe`, unless it is wrapped in a safe function.
- `sosuc bindgen` (v1): generates declarations from C headers (functions, structs, enums, constants). In v0, bindings are written by hand.
- The Sosu declarations must match the C ones exactly (`int` is `i32`, a C struct is a Sosu `struct` with the same fields in the same order): the C compiler never sees the C headers.

### 9.1 Linking C libraries

C libraries are declared in the `sosu.toml` of the package that uses them, never in the source code, so that the code stays the same on every target (principle 11):

```toml
[link]                          # every target
libs = ["SDL3"]                 # SDL3.lib with MSVC, -lSDL3 with gcc and clang
subsystem = "window"            # its programs open windows: no console in release, on Windows

[link.windows]                  # added on one target: windows, linux, macos, android, ios
paths = ["vendor/SDL3/lib/x64"] # where the linker looks, relative to the package
copy = ["vendor/SDL3/lib/x64/SDL3.dll"]

[link.macos]
frameworks = ["Cocoa"]
```

- `libs`: library names, translated for the C compiler; `paths`: folders searched by the linker; `frameworks`: macOS and iOS frameworks; `copy`: files placed next to the program (a DLL must be beside the `.exe` on Windows).
- `subsystem`: `"window"` for a package whose programs open windows (`platform` declares it). In a `--release` build on Windows, a program that links such a package has no console behind its windows (`/SUBSYSTEM:WINDOWS` with MSVC, `-mwindows` with MinGW); a debug build keeps it, for `io.print`. `"console"` is the default.
- Paths are relative to the package, never absolute, so that the manifest works on every machine.
- Linking is transitive: the libraries of a dependency are linked into the program that uses it, which does not need to know them. Duplicates are removed.
- The values are one-line lists of strings. [link.<target>] uses the target names of `--target` (§11.1); for now, the target is the system running `sosuc`.

### 9.2 Using Sosu from other languages (planned)

Status: design, not implemented yet. The grammar of §12 will gain `export` when it is.

A library written in Sosu is offered to other languages through the C ABI, which they all know how to call: C and C++ directly, C# with P/Invoke, Java with the FFM API (or JNI), Python with `ctypes` or `cffi`, Rust, Go, Swift. `sosuc` produces a C library and its header; each language uses its own tools on them. The compiler contains nothing specific to C#, Java or any other language.

```sosu
// geometry/src/lib.sosu
pub struct Vec2 {
    pub f32 x;
    pub f32 y;
}

export "C" f32 geometry_dot(Vec2 a, Vec2 b) {
    return a.x * b.x + a.y * b.y;
}
```

```c
/* build/geometry.h, generated */
typedef struct geometry_Vec2 { float x; float y; } geometry_Vec2;
float geometry_dot(geometry_Vec2 a, geometry_Vec2 b);
```

Exported functions:

- `export "C"` marks a function of the top level that the library offers. Its C name is its Sosu name, unchanged: the author chooses it, usually prefixed with the name of the library, since C has a single namespace. Two exports with the same name are an error; a C keyword or a name starting with `sosu_` is refused.
- Methods are not exported: a function takes the object instead (`export "C" void doc_close(Doc* d)`).
- Types allowed in the signature of an export, whose C form the header gives:
  - integers, floats and `bool` (`int32_t`, `float`, `bool`...), `char` (`uint32_t`, a Unicode scalar value);
  - `struct`s whose fields are all allowed (C layout, §9), and pointers to the allowed types;
  - `str` (`sosu_str`: `const uint8_t *ptr; size_t len;`, UTF-8, not terminated by a NUL) and `T[]` (`ptr` and `len`);
  - a pointer to a class, as an opaque pointer (`typedef struct geometry_Shape geometry_Shape;`): the host only passes it back to exported functions;
  - function types made of allowed types, for callbacks into the host.
- Not allowed in v0: `T?`, `T!E`, `enum`s (with or without data), `Arena` by value, interfaces. An export translates them (a `bool` result and an out pointer, an error code...).
- Effects and contracts are kept: an export can have any `uses` clause (the host is outside the effect system, as C is), and its `requires` are checked on entry, like any call from the host.

Building:

- `sosuc build --lib` compiles the package (entry `src/lib.sosu`, no `main`) into a static library (`build/<name>.a`, or `.lib` with MSVC); `--lib=shared` into a shared one (`.so`, `.dll` with its import library, `.dylib`). Both come with `build/<name>.h`.
- The header is plain C99 (`stdint.h`, `stdbool.h`, `stddef.h`), usable from C++ (`extern "C"` guards). It declares the exports, and the types they use under the name `<package>_<Type>`. The `#line` directives stay in the C code of the library: a debugger shows the `.sosu` files.
- The runtime (`sosu_rt.c`) is compiled into the library. In a shared library, its symbols, and those of the generated code, are hidden: only the exports are visible.
- The C libraries of the package and of its dependencies (`[link]`, §9.1) are linked into a shared library. For a static library, `sosuc` writes their list next to it, since the host links them.
- Mobile (§11.2) uses the same output: the Android activity loads a shared library, the Xcode project links a static one.

Run-time errors:

- In a program, a run-time error (overflow, index out of bounds, failed contract) stops it with the code 101. A library must not stop the process of its host: each export runs under a guard, as `sosuc test` already does for each test. After an error, the export returns a zero value, and `<package>_error()` gives the message, or NULL if the last call succeeded. The message is kept per thread.
- After an error, what the failed call was building is left as it is, and its `defer`s did not run: the host should discard the objects that call used.

Memory and threads:

- What Sosu allocates lives in its arenas, and is freed by Sosu, never by the host. A library offers pairs of functions: `doc_open` creates an object that owns its arena, `doc_close` frees it. A `str` or a `T[]` returned to the host stays valid until the arena that holds it is freed.
- Memory of the host passed to an export (a buffer, a `str`) is only read or written during the call: Sosu keeps no pointer to it afterwards (a rule that v0 does not check).
- Sosu has no global variables: a library is reentrant, and two threads can call it at the same time on objects of their own (an arena belongs to one thread, §14.1).

Use from other languages (nothing to add to the compiler):

| Language | How it calls the library | From the header |
|---|---|---|
| C, C++ | `#include "geometry.h"`, then link | as is |
| C# | `[LibraryImport("geometry")]`, `[StructLayout(LayoutKind.Sequential)]` | ClangSharp, or by hand |
| Java (22 and later) | FFM API (`java.lang.foreign`) | `jextract` |
| Java (older), Android | JNI, through a small C file | by hand |
| Python | `ctypes` or `cffi` | `cffi` reads the header |
| Rust, Go, Swift | `extern "C"`, cgo, a module map | `bindgen`, cgo, Swift's C importer |

## 10. User interface

The UI is a set of packages, never compiler syntax. Three layers, each replaceable:

| Layer | Role | v0 choice |
|---|---|---|
| `platform` | window, events, touch input, clipboard, cursor, high-density screens, lifecycle | SDL3 (alternative: `sokol_app`) |
| `render` | rectangles, lines, text, images | software rendering (pixel buffer), then GPU (Metal, Vulkan, D3D or SDL_GPU) |
| `ui` | layout, widgets, state, declarative syntax | widgets drawn by Sosu, identical rendering everywhere |

- SDL3 handles Windows, Linux, macOS, Android and iOS. Its interface stays narrow so that custom backends can be added later (Win32, Cocoa, X11 / Wayland, UIKit, native Android).
- Text is the real difficulty. v0 starts with a built-in 8 by 8 bitmap font (ASCII, §10.4); proportional fonts will come through `stb_truetype` or FreeType behind the same call. HiDPI and international input (IME) must be planned from the start: typed text already arrives as `TextInput` (UTF-8), after the keyboard layout and the input methods.
- Accessibility (screen readers): to be planned, outside v0.

### 10.1 Events and loop

Events are a sum `enum`, handled by an exhaustive `match`. On mobile, the OS owns the main loop: the `platform` runtime therefore owns it on every platform, and the program provides callbacks.

The sketch below is the target. What v0 implements (§10.4) passes the callbacks as an `app.Handler`, and has the events `Quit`, `Resized`, `Key`, `TextInput`, `MouseMove`, `Click` and `Release`; touch, safe areas and the lifecycle come with mobile.

```sosu
enum Event {
    Quit,
    Resized(i32 w, i32 h),
    Key(Key key, bool down),
    TextInput(str text),
    MouseMove(f32 x, f32 y),
    Click(f32 x, f32 y, Button button),
    Touch(i32 id, TouchPhase phase, f32 x, f32 y),
    SafeAreaChanged(f32 top, f32 right, f32 bottom, f32 left),
    Suspend,        // application in the background: save the state, stop rendering
    Resume,
    LowMemory,      // free the cache arenas
}

void on_event(App* app, Event e) uses io {
    match (e) {
        Quit => { app.running = false; },
        Resized(w, h) => { app.ui.resize(w, h); },
        Suspend => { app.save(); },
        _ => { app.ui.handle(e); },
    }
}

i32 main() uses io {
    auto config = AppConfig{ title: "Hello", width: 800, height: 600 };
    return platform.run(config, on_init, on_event, on_frame);
}
```

A window has its own arena; a per-frame arena is reset at every frame for the widget tree.

### 10.2 `.ui.sosu` interface files

Windows and components are described in `.ui.sosu` files, separate from the logic. The format is inspired by XAML, but lighter: braces, no tags, no namespaces. The examples below show the whole design; what v0 translates is listed under *Translation in v0*, after the rules.

```sosu
// login.ui.sosu
import app.login;

window LoginWindow(LoginState* state) {
    title: "Login"
    width: 400
    height: 300

    Column(gap: 8, padding: 16) {
        Text("Name")
        TextField(bind: state.name)
        Row(gap: 8, align: End) {
            Button("Cancel", on_click: state.cancel)
            Button("OK", on_click: state.submit, enabled: state.name.len > 0)
        }
    }
}
```

Reusable components, lists and styles:

```sosu
// tasks.ui.sosu
import app.tasks;

style Muted { color: Gray }

component TaskRow(Task* t) {
    Row(gap: 8) {
        Checkbox(bind: t.done)
        Text(t.title, style: t.done ? Muted : Normal)
    }
}

window TaskWindow(TaskState* state) {
    title: "Tasks"

    List {
        for (Task* t in state.tasks) {
            TaskRow(t)
        }
    }
}
```

Rules:

- A node is written `Type(arguments) { children }`. The children and properties of a window are separated by line breaks; named arguments (`name: value`) are separated by commas.
- Values are ordinary Sosu expressions, checked by the compiler. An `enum` value is written without qualifier when the expected type is known (`align: End`).
- `bind:` creates a two-way binding; its target must be mutable (`mut` field).
- Data comes from typed parameters of the window or component. No data context, no code-behind, no converters (an expression is enough).
- `if`, `for` and `match` are used directly in the tree, with the Sosu syntax.
- Grids are set with `row`, `col` and `span`, without attached properties.
- Styles and themes: named `style` and `theme` blocks.
- The description is re-evaluated when the state changes, in the frame arena (see §10.1). No change notification and no dependency properties.

XAML equivalents:

| XAML | `.ui.sosu` |
|---|---|
| opening and closing tags | braces |
| namespaces (`xmlns`) | `import` |
| `Grid.Row="1"` | `row: 1` |
| `{Binding Path=Name, Mode=TwoWay}` | `bind: state.name` |
| `DataContext` and code-behind | typed parameters |
| `IValueConverter` | Sosu expression |
| `DataTemplate` | `for`, `if`, `match` |
| `INotifyPropertyChanged` | re-evaluation from the state |
| `ResourceDictionary` | `style`, `theme` |
| `UserControl` | `component` |

Compilation:

- `sosuc` translates each `.ui.sosu` into ordinary Sosu functions, which describe the interface with the widgets of the `ui` package at each frame (immediate mode, §10.4). The translation builds the syntax tree directly, with the positions of the interface file: the type checker's errors point to it, without `#line`.
- No reflection and no run-time parsing: an unknown property is a compile error, with a suggestion.
- The translator lives next to the parser, whose expressions it reuses, and only knows the interface of `ui.widgets`; the type checker and the code generator see ordinary functions.
- Hot reload (changing the interface without recompiling): possible later, by interpreting the file during development.

Translation in v0:

- `import app.screen;` loads `src/screen.sosu`, or else `src/screen.ui.sosu`. The package must depend on `ui`; `import ui.widgets;` is implied.
- `window Name(params) [uses ...] { properties, then nodes }` becomes `pub void Name(widgets.Frame* ui_frame, params)`. Its properties `title`, `width`, `height` and `icon` (the path of a PNG file) become `pub widgets.Window NameConfig()`, which the program passes to `platform` (`app.Config`). `component Name(params) { nodes }` becomes the same function, without properties. A node `Name(values)`, or `module.Name(values)`, calls a component, its values in order.
- Nodes, and what they become (`f` stands for `ui_frame`); a number of pixels can be written `8` for `8.0`:

| Node | Becomes |
|---|---|
| `Column(gap: g, padding: p) { ... }`, `Row(...)` | `f.column(g, p)` (or `f.row_with(g, p)`), the nodes, `f.end()`; `gap` is 8 and `padding` 0 by default |
| `Text(s)`, `Title(s)`, `Space(n)` | `f.label(s)`, `f.title(s)`, `f.space(n)` |
| `Image(img, size: s)` | `f.image(img, s)`: an `image.Image` (§10.4), `s` pixels high, as wide as its proportions say; its own size when `size` is left out |
| `MenuBar { ... }`, `Menu(s) { ... }`, `ContextMenu { ... }` | `f.menu_bar()`, `f.menu(s)` or `f.context_menu()`, the nodes, then `f.end_menu_bar()`, `f.end_menu()` or `f.end_context_menu()`; a `MenuBar` holds `Menu`s, a menu holds `MenuItem`, `Separator` and `Menu` nodes (a submenu) |
| `MenuItem(s, on_click: h, shortcut: k, enabled: c)` | `if (f.menu_item(s)) { h(); }`, or `f.menu_item_enabled(s, c)`, or with a shortcut `f.menu_item_with(s, k, c)`; `Separator` is `f.separator()` |
| `Button(s, on_click: h, enabled: c)` | `if (f.button_enabled(s, c)) { h(); }`; `h` is a function or a method (`state.save`), or a call (`state.save(1)`) |
| `TextField(bind: x, width: w, on_enter: h)` | `if (f.text_field(&x, w)) { h(); }`; `bind:` is required, `width` is 200 by default |
| `TextField(bind: x, accept: Digits, max: n, mask: m)` | `f.text_field_with(&x, w, widgets.Format{ accept: widgets.Accept.Digits, max: n, mask: m })`: `accept:` names a variant of `widgets.Accept` |
| `Checkbox(s, bind: x, on_change: h)` | `if (f.checkbox(s, &x)) { h(); }`; without `s`, `f.checkbox_alone(&x)` |
| `Radio(s, bind: x, value: v, on_change: h)` | `if (f.radio(s, x == v) && x != v) { x = v; h(); }`: one radio per value of an `enum` without data, `v` written without its type (`Small`); choosing the radio already chosen changes nothing |
| `List(width: w, height: h, selected: i) { ... }` | `f.list(w, h, some(&i))` (or `none`), the nodes, `f.end_list()`: each node in the list is an item; `i` (an `i32`, -1 for none) is the item chosen; `width` is 300 and `height` 200 by default |
| `Scroll(width: w, height: h) { ... }` | `f.scroll(w, h)`, the nodes, `f.end_scroll()`; `width` and `height` are required |
| `TitleBar(...) { ... }`, `Dropdown(...) { Option(...) }` | see §10.5 |

- `if`, `else` and `for (T x in ...)` hold nodes, as in the examples. Identities (§10.6): each node of a widget that has one is preceded by `f.at(site)`, its offset in the file; a component used as a node is called between `f.push_id(site)` and `f.pop_id()`; a `for` is between `f.push_id(site)` and `f.pop_id()`, each iteration between `f.push_item()` (or `f.push_id(key as u64)` with `for (T x in ..., key: k)`) and `f.pop_id()`. `focus: c` on a `TextField`, a `Button`, a `Dropdown` or a `List` becomes `if (c) { f.focus_here(); }` before it. A node without arguments may leave out its parentheses: `List { ... }`.
- `theme` and `style` blocks, and `style:` on a node: see §10.7. `Grid` and the places of its nodes (`row:`, `col:`, `span:`, `align:`), `Group`, `Tabs` and `Tree`: see §10.8. Not yet: `bind:` on other widgets than `TextField`, `Checkbox`, `Radio` and `Dropdown` (error E0216). `.ui.sosu` files have no formatter yet; `sosuc parse file.ui.sosu` shows the functions they become.

Grammar of a `.ui.sosu` file (the expressions, types and parameters are those of §12):

```ebnf
ui_file      = { import | view | theme | style } ;
view         = ( "window" | "component" ) IDENT params [ effects ] "{" { property } { item } "}" ;
property     = IDENT ":" expr ;                                (* window only: title, width, height, icon *)
item         = node | "if" "(" expr ")" children [ "else" ( item | children ) ]
             | "for" "(" [ "mut" ] ( type | "auto" ) IDENT "in" expr [ "," "key" ":" expr ] ")" children ;
node         = path [ "(" [ argument { "," argument } ] ")" ] [ children ] ;
argument     = IDENT ":" expr | expr ;
children     = "{" { item } "}" ;
theme        = "theme" IDENT [ ":" IDENT ] "{" { property | selector } "}" ;  (* §10.7 *)
style        = "style" IDENT "{" { property | state } "}" ;
selector     = IDENT "{" { property | state } "}" ;                (* a widget: Button, TextField... *)
state        = IDENT "{" property { [ "," ] property } "}" ;        (* hover, focus, placeholder... *)
```

### 10.3 Mobile specifics

- Touch screens: `Touch` events, gestures in `ui`.
- Safe areas (notches, system bars): `SafeAreaChanged`.
- Orientation, virtual keyboard and varying screen sizes: adaptive layout in `ui`.
- Lifecycle: `Suspend` / `Resume` / `LowMemory`; on iOS, rendering must stop in the background.

### 10.4 Milestones

1. Desktop: open a window, show a colored rectangle, close cleanly on `Quit`. Done on Windows and Linux with `packages/platform` and `examples/window`; `tests/window` checks it in the CI. Without generics to carry the application state, its v0 interface differs from the sketch of §10.1: the program implements the interface `app.Handler` with a class, whose fields hold the state, and calls `app.run(config, handler)`. `on_event(Event e)` returns `false` to close the window; `on_frame(canvas.Canvas* surface)` draws on a buffer of pixels the size of the window (package `render`, below). While the window is resized, Windows holds the loop of `run` until the mouse is released: `on_frame` is then called from an SDL event watch, on each `SDL_EVENT_WINDOW_EXPOSED` of the live resize, so that the window never shows a black area. The events are `Quit`, `Resized`, `Key(code, down, mods)` (`app.Mods`: `shift`, `ctrl`, `alt`, `command`; a key held repeats its `down`), `MouseMove`, `Click`, `Release`, `Wheel(x, y)` (steps; `y > 0` away from the user, natural scrolling already applied) and `TextInput` (its `str` lives until the end of the frame); `Config.icon` (an `image.Image`) gives the window its icon, for the system (task bar, Alt+Tab, its title bar); `app.request_quit()` closes the window as the window button does, `app.minimize()` and `app.toggle_maximize()` act on it as its other buttons do (§10.5, with the events `CustomTitleBar` and `Maximized`), `app.clipboard(arena)` and `app.set_clipboard(arena, text)` read and write the clipboard.
   - `render.canvas` draws in Sosu into a `Canvas` (pixels 0xAARRGGBB): `clear`, `fill` (blended when the color is not opaque), `stroke`, `fill_circle` (a disc, its edge antialiased), `line`, `text` and `at`, all clipped to the canvas, and to the rectangles of `push_clip(rect)` until `pop_clip()` (16 levels; `clear` ignores them). `platform` shows it with one SDL texture per frame. Its tests (`sosuc test packages/render`) need no screen.
   - Text: `render.font` is a built-in 8 by 8 font for printable ASCII (font8x8, public domain); other characters are drawn as a box. `surface.text(x, y, text, color, scale)` draws each pixel of the font as a `scale` by `scale` square, `\n` starts a new line (`font.line_height()`: 10 pixels of the font), and `canvas.text_size(text, scale)` gives the size of the text. It needs no file, and stays the font of last resort.
   - TrueType fonts: `render.truetype` reads `.ttf` files and draws their glyphs in Sosu, antialiased, at any size. `truetype.load(arena, path)` gives a `Font*` (or `Unreadable`, `NotTrueType`); `surface.text_with(font, size, x, y, text, color)` draws text whose first line has its top at y, `size` being in pixels per em; `font.measure(text, size)`, `font.line_height(size)` and `font.ascent(size)` measure it. All of Unicode that the font covers is drawn (accented letters are composite glyphs); a character it does not have gets its missing glyph.
     - Reading: the tables `head`, `hhea`, `maxp`, `hmtx`, `loca`, `glyf` and `cmap` (formats 4 and 12), through checked reads: a damaged file gives wrong glyphs, never a read outside of its bytes. Drawing: the outlines (lines and quadratic curves, composite glyphs moved and scaled) accumulate the area they cover under each pixel; the image of a glyph is kept per size (256 images at most, then forgotten), and `close()` frees them.
     - Not yet: hinting (the instructions of a font are ignored), kerning, CFF outlines (`.otf`), color fonts, fallback to another font for missing characters.
     - `packages/render/fonts/NotoSans-Regular.ttf` (Noto Sans, SIL Open Font License, with `OFL.txt`) is the font of the examples and of the tests, run from the root of the repository. `ui.font` (a `widgets.Ui` field) gives it to the widgets: 18 pixels per em for labels, 27 for titles; without it, they use the 8 by 8 font.
   - Images: `render.image` reads PNG files in Sosu. `image.load(arena, path)` gives an `Image` (`width`, `height`, `pixels` 0xAARRGGBB, alpha not premultiplied), or `Unreadable`, `NotPng`, `Damaged`; `image.decode(arena, bytes)` does the same from bytes. Every kind of PNG file is read: gray, RGB, palette, with alpha or a transparent color (`tRNS`), 1 to 16 bits, interlaced (Adam7) or not. The data is inflated (zlib) in Sosu, through checked reads: a damaged file gives `Damaged`, never a read outside of its bytes; the checksums are not verified. `surface.draw_image(img, rect)` draws it scaled into `rect`, blended with what is below: each pixel takes the average of the pixels it covers, weighted by their alpha (the nearest one when it is enlarged). Not yet: other formats (JPEG, GIF), color profiles and gamma (`iCCP`, `gAMA` are ignored).
2. The same program on the Android emulator, then on the iOS simulator.
3. A window described in `.ui.sosu` (title, text field, button) with the same interface on every target. Done on the desktop: `examples/form` describes its window in `screen.ui.sosu` (§10.2), and `tests/programs/screens` drives such a window with simulated input, without a screen.
   - The `ui` package: without lambdas (v1), a widget cannot call the program back, so `ui` works in immediate mode, which matches the re-evaluation of §10.2. At each frame, the program describes its interface from its state, `if (f.button("OK")) { ... }`; a `widgets.Ui` keeps what lasts between frames (the input received, the widget pressed, the text field that has the keyboard), and a `widgets.Frame` places the widgets of the frame in nested columns and rows (`column(gap, padding)`, `row_with(gap, padding)`, `end()`; `row()` and `end_row()` for a row with the default gap).
   - Widgets: `label`, `title`, `button` and `button_enabled` (true when clicked), `checkbox(text, bool*)` and `checkbox_alone(bool*)`, `radio(text, chosen)` (true when clicked), `list(width, height, i32*? selected)` ... `end_list()`, `text_field(TextBuffer*, width)` and `text_field_with(TextBuffer*, width, Format)` (true on Enter), `dropdown(width)`, `option(text, chosen)` (true when clicked) and `end_dropdown()`, `title_bar(height, gap, padding)` ... `end_title_bar()` (§10.5), the menus (`menu_bar`, `menu`, `context_menu`, `menu_item`, `separator`, below), `space`. `TextBuffer` holds up to 256 bytes of UTF-8. `widgets.Window` holds what a window of a `.ui.sosu` file says about itself.
   - Text fields: a click gives the keyboard and places the cursor, dragging selects. Left and Right (with Shift: selecting), Home, End, Backspace and Delete edit at the cursor; Ctrl+A selects all, Ctrl+C and Ctrl+X copy (`ui.take_copied()` gives the text, for `app.set_clipboard`), and a paste is typed text (`ui.type_text(app.clipboard(arena))`). The text is clipped to the field and scrolls to keep the cursor in view. The input of a frame is replayed in order (typing, then a key, then typing again).
   - Formats: `Format{ accept, max, mask }`. `accept` is `Any`, `Digits`, `Integer` (and `-` first) or `Decimal` (and `-` first, and one `.` or `,`); `max` limits the characters; a mask such as `99/99/9999` takes a digit for `9`, a letter for `A`, anything for `*`, and writes its other characters when the cursor reaches them. What does not fit is not typed.
   - Lists: the widgets placed directly in a list are its items, one under the other, clipped to it. The mouse wheel over it scrolls it (`ui.scroll_wheel(steps)`, 40 pixels a step), within its content; a bar on its right shows where. With `selected`, a click chooses an item and gives the list the keyboard, Up and Down move the choice, which comes into view, and the item chosen is highlighted. `Ui` keeps the scroll of 8 lists between frames.
   - Menus: `menu(title)` ... `end_menu()` describes a menu at each frame, open or not, so that the shortcuts of its items work when it is closed. Outside of a menu, its title is placed as a widget (side by side in `menu_bar()` ... `end_menu_bar()`): a click opens the menu under it, and while a menu opened by its title is open, the mouse over another title opens that one. In a menu, `menu(title)` is an entry with a chevron that opens a submenu beside it when the mouse is over it (4 levels). `context_menu()` ... `end_context_menu()` is the menu of the widget placed just before it (a list: the whole list), opened under the mouse by the right button (`ui.right_button(down)`); when several match, as an item and its list, the first described opens. `menu_item(text)` is true when it is clicked; `menu_item_enabled(text, enabled)` dims it; `menu_item_with(text, shortcut, enabled)` shows a shortcut at its right, `"Ctrl+S"`, `"Ctrl+Shift+Z"`, `"F5"`, `"Delete"` (`Cmd` is `Ctrl`), which works even with the menu closed: a key press triggers the first item described that has it, and a shortcut without Ctrl, other than F1 to F12, does nothing while a text field has the keyboard. `separator()` draws a line. A click elsewhere, a right press elsewhere or Escape closes the menus; an open menu stays on the surface, and its width follows its widest entry from the next frame.
   - `ui` does not depend on `platform`: the program passes it the input (`mouse_move`, `mouse_button`, `right_button`, `scroll_wheel`, `scroll_wheel_x`, `type_text`, `key(code, down, shift, ctrl)`), so its tests run without SDL, and puts what it copies on the clipboard (`examples/form` shows the few lines). The translator of `.ui.sosu` generates the calls to the widgets.

Widgets of `ui`: available and planned. The names are those of a `.ui.sosu` file (§10.2), each one with a method of `widgets.Frame` behind it.

| Widget | State | Design |
|---|---|---|
| `Text`, `Title` (labels) | available | |
| `Button` | available | `enabled:` dims it and ignores clicks |
| `Checkbox` | available | with or without a label |
| `Column`, `Row`, `Space` | available | nested, with `gap` and `padding` |
| `Image` | available | a PNG image (`render.image`), scaled; the icon of a window, beside its title (§10.5) |
| `TextField` (one line) | available | the editing core: cursor, selection, clipboard, scrolling; 256 bytes |
| `TextField` with a format | available | `accept:` (`Digits`, `Integer`, `Decimal`), `max:`, `mask:` (`"99/99/9999"`) |
| `NumberField` | available | bound to an `i64`, with `decimals:` for fixed-point numbers, `min:`, `max:`, `step:` (§10.9) |
| `MoneyField` | available | a `NumberField` with a symbol and groups of thousands, 2 decimals by default (§10.9) |
| `Slider`, `Progress` | available | an `i64` dragged or moved by keys; a bar with a value, or busy (§10.9) |
| `DateField` | available | a mask and a calendar above the rest (§10.9) |
| `Dialog` | available | modal, centered, above the rest (§10.9) |
| `Table` | available | columns of sizes, sorting, borders dragged, rows chosen, only those in view (§10.9) |
| `TextArea` (several lines) | available | bound to a `LongText`: lines, wrapping, Up and Down, undo, scrolling (§10.9) |
| `Radio` | available | `Radio("Small", bind: size, value: Small)`: the translator generates the comparison and the assignment for the `enum` (`==` on enums without data), so the widget itself needs no generics |
| `List` | available | `List(selected: index, height: 200) { for (...) { ... } }`: scrolling (wheel, bar), selection by click and by keyboard; `Ui` keeps the scroll position between frames. A `Scroll` with items: its bar can be dragged; with the keyboard, Up, Down, Home, End, Page Up and Page Down. Not yet: a selection of several items |
| `Scroll` | available | `Scroll(width: 300, height: 200) { ... }`: any content, scrolled by the wheel (the innermost area first), its bars and the keyboard (§10.6) |
| `Dropdown` | available | `Dropdown(bind: country) { Option("France", value: FR) ... }`: the open list is drawn above the rest, and takes the clicks before the widgets under it. Not yet: choosing with the keyboard, scrolling a long list (16 options at most) |
| `MenuBar`, `Menu`, `MenuItem`, `Separator` | available | menus drawn by `ui`, in a title bar as in VS Code, or anywhere; submenus; shortcuts |
| `ContextMenu` | available | the menu of the widget placed before it, opened by the right button |
| `TitleBar` | available | a title bar drawn by the program, with widgets in it (§10.5) |
| `Grid` | available | columns and rows in pixels, `Auto` or `Fill`; cells, spans, alignment (§10.8) |
| `Group` | available | a frame with a title on its border; it can fold (§10.8) |
| `Tabs`, `Tab` | available | the content of the tab chosen only; closable tabs, Ctrl+Tab (§10.8) |
| `Tree`, `Branch`, `Leaf` | available | a list of indented items; branches open and close with the mouse and the keyboard (§10.8) |

Foundations, to build first, since most planned widgets rely on them:

1. Clipping in `render`: a stack of clip rectangles on the `Canvas`, so that nothing overflows a field, a list or a text area. Done.
2. The editing core of text fields: a cursor (Left, Right, Home, End, Delete), a selection (Shift, the mouse), the clipboard (through SDL in `platform`), key repeat. Done (key repeat comes from the system); not yet: moving by word (Ctrl+Left), undo.
3. Text buffers that grow, in an arena, beside the fixed `TextBuffer`.
4. A layer above the others: what is drawn at the end of the frame (open dropdowns, later tooltips and menus), and takes the input first, from its rectangle at the previous frame. Done for dropdowns and menus: `Ui` keeps the rectangles of the open list of a dropdown and of the open menus, and the widgets under them do not see the mouse.
5. Keyboard navigation (Tab, Shift+Tab from field to field), and the mouse wheel as an event of `platform` (done: `Wheel`). Done, with the identity of the widgets and the scrolling of any area (§10.6).

Order: the editing core and formatted one-line fields, `Radio`, `List` with scrolling (done), `Dropdown` with the layer above (done), then `TextArea`. Worth adding afterwards: slider, progress bar, tabs, dialogs, number spinner, date picker, tooltips, menus.

Two limits of v0 apply to every text widget: without a TrueType font (`ui.font`), the built-in font only draws ASCII (other characters are kept, and drawn as boxes), and the composition of input methods (IME) is not shown: only the final text arrives, as `TextInput`.

### 10.5 Title bars drawn by the program

A window can draw its own title bar and put widgets in it (a search field, a dropdown, buttons), as VS Code does. The bar still behaves as a title bar: dragging its empty space moves the window, a double-click maximizes it, and it has the minimize, maximize and close buttons. The system draws nothing: the window has no decorations, and the system asks the program, point by point, what is under the mouse.

In a `.ui.sosu` file, `TitleBar` is the first node of a window, at most once; its children are placed in a row, as in `Row`:

```sosu
window Editor(state.Editor* editor) {
    title: "Sosu editor"
    width: 900
    height: 600

    TitleBar(height: 40, gap: 8, padding: 4) {
        Text("Sosu")
        TextField(bind: editor.search, width: 240)
        Dropdown(bind: editor.mode, width: 140, on_change: editor.rebuild) {
            Option("Debug", value: Debug)
            Option("Release", value: Release)
        }
        Button("Run", on_click: editor.run)
    }
    Column(gap: 8, padding: 16) {
        Text(editor.output.text())
    }
}
```

| Node | Becomes |
|---|---|
| `TitleBar(height: h, gap: g, padding: p) { ... }` | `f.title_bar(h, g, p)`, the nodes, `f.end_title_bar()`; `height` is 40, `gap` 8 and `padding` 4 by default. `EditorConfig()` gets `custom_title_bar: true` |
| `Dropdown(bind: x, width: w, on_change: h) { ... }` | `f.dropdown(w)`, its options, `f.end_dropdown()`; `width` is 160 by default |
| `Option(s, value: v)`, only in a `Dropdown` | `if (f.option(s, x == v) && x != v) { x = v; h(); }`: as `Radio`, one option per value of an `enum` without data |

- `TitleBar` elsewhere than first in a window (in a component, in a `Column`, after another node, twice) is an error. `Option` outside a `Dropdown`, or another node in a `Dropdown` (except `if` and `for` around options), is an error.

`ui` (still without depending on `platform`):

- `f.title_bar(height, gap, padding)` opens a row across the whole width of the surface, at the top of the frame, on a background of its own; `f.end_title_bar()` draws the three window buttons at its right end (46 pixels wide each, the close button red under the mouse), then the frame goes on under the bar.
- What the bar holds is remembered in `Ui` for the next frame: its rectangle, those of the window buttons, and those of the interactive widgets placed in it (buttons, text fields, checkboxes, radios, dropdowns; 16 at most). Labels are not interactive: they drag the window, as the title of a system bar does.
- `ui.is_drag_area(x, y)`: in the bar, and on none of these rectangles, nor on an open dropdown. The system asks between two frames, so the answer comes from the last frame drawn.
- The window buttons do not act themselves: `ui.take_window_request()` gives `some(Minimize)`, `some(ToggleMaximize)` or `some(Close)` once after a click, as `take_copied()` gives the text copied, and the program passes it to `platform`. `ui.maximized` (set by the program) chooses the icon of the middle button: one square, or two when the window is maximized.
- `ui.custom_title_bar` (set by the program, false by default) says whether the system left the bar to the program. When false, `TitleBar` is a tool bar: the same row of widgets at the top, without window buttons and without drag area, under the title bar of the system.
- `Dropdown` brings the layer above (foundation 4 of §10.4): a click on the closed box opens the list of its options under it; drawn at `finish()`, over everything else, it takes the input before the widgets under it, from its rectangle at the last frame. A click on an option chooses it and closes the list, a click elsewhere or Escape closes it without choosing. The box shows the text of the option chosen.

`platform`:

- `app.Config` gains `title_bar: app.TitleBar*? = none`. `TitleBar` is an interface with one method, `bool is_drag_area(f32 x, f32 y)`, that the program implements beside `Handler` (`class App : app.Handler, app.TitleBar`), usually by asking its `Ui`.
- With a `title_bar`, `run` creates the window without decorations (`SDL_WINDOW_BORDERLESS`, still resizable), and gives SDL a hit test (`SDL_SetWindowHitTest`): a function of `platform`, passed as a C callback, with the interface as its data. On each point, it answers a border (`SDL_HITTEST_RESIZE_*`, 6 pixels wide, none when the window is maximized), else the drag area (`SDL_HITTEST_DRAGGABLE`) when `is_drag_area` says so, else the normal area: the program's widgets get the click.
- Events: `CustomTitleBar(bool active)`, once before the first frame, when the program asked for a title bar: `true` if the hit test was accepted; `Maximized(bool on)`, when the window is maximized or restored.
- `app.minimize()` and `app.toggle_maximize()` act on the window as `app.request_quit()` does, through an event that `run` receives (`SDL_EVENT_USER`), since the program has no handle on the window.
- Double-click and the rest: on Windows, the drag area is the caption of the system (`HTCAPTION`), which gives the double-click that maximizes, the snapping to the edges of the screen, and the window menu (Alt+Space). Elsewhere, SDL moves the window itself, and a double-click does nothing (a limit of v0).

Fallback, when the system leaves its own decorations (`SDL_SetWindowHitTest` refused, a backend without hit test, a phone or a tablet, where a window has no title bar):

- `run` gives the window its decorations back (`SDL_SetWindowBordered`), and sends `CustomTitleBar(false)`. The program does not change: its `TitleBar` becomes the tool bar described above, under the bar of the system. On a phone, it is the bar at the top of the screen, below the safe area (§10.3).

Tests: the tests of `ui` check the drag area, the window buttons and their requests, and a dropdown opened, chosen and closed; `tests/programs/titlebar` drives, without a screen, a window whose title bar holds a text field, a dropdown and buttons; `tests/window` asks for a title bar, and checks that `CustomTitleBar` comes before the first frame (`false` with the dummy video driver of the CI, `true` on a Windows desktop). `examples/editor` shows it on the desktop: on Windows, dragging the bar moves the window and a double-click maximizes it. As everywhere with SDL, the click that brings a window to the front is not given to the program. `examples/workspace` is a fuller one, in the manner of VS Code: actions, a search field and two dropdowns in the bar, over panes that use every widget; with the environment variable `SOSU_SYSTEM_TITLE_BAR` set, it shows the fallback (the bar of the system, and the bar of the program as a tool bar).

### 10.6 Foundations of the widgets: identity, keyboard, scrolling

The first of the batches that complete `ui` (grid, frames, tabs, tree, table, text area...): what they all rely on.

#### Identity of the widgets

Today a widget is the number of its call in the frame. A widget that appears (`if`) or an item added to a list (`for`) shifts the numbers of those after it: the field that has the keyboard, the button held or the list scrolled becomes another one. The grid, the tabs and the tree make widgets appear and disappear all the time, so this comes first.

- An identity is a 64-bit hash (FNV-1a) of a path: the scopes open around the widget, then the widget itself. `Ui` keeps identities (`active`, `focus`, the dropdown open, the menus open), never numbers; 0 is none.
- `f.at(site)` gives the widget that follows its place, a `u32`; without it, the place is the number of the call in its scope, as today, so code written by hand keeps working.
- `f.push_id(key)` ... `f.pop_id()` open a scope, `key` a `u64`.
- The translator of `.ui.sosu` files writes them:
  - each node gets `f.at(site)`, `site` being its offset in the file: a widget keeps its identity whatever appears before it;
  - the body of a `for` is a scope per iteration, its key the index of the loop, or the value of `key:` when the loop gives one: `for (Task* t in state.tasks, key: t.id) { ... }`, so that an item keeps its focus when another is inserted before it;
  - a component used as a node is a scope, its key the site of the node: two uses of `TaskRow` are distinct.
- What lasts between frames per widget (the scroll of a list or of a `Scroll`, later the branches open in a tree, the tab chosen) is in a table of `Ui` keyed by identity (64 entries). An entry not seen during a frame is freed at its end: a list that disappears forgets its scroll, as a widget that no longer exists has no state.
- Two widgets with the same identity in a frame (a loop with a `key:` that repeats): the second gets the number of its call instead, and a debug build reports it once (`io.print` is not available to `ui`: `ui.duplicates` counts them, and the tests check it is 0).

#### Keyboard navigation

- Tab and Shift+Tab move the keyboard to the next and previous widget that takes it, in the order of the frame: text fields, buttons, checkboxes, radios, dropdowns, lists, and the `Scroll` areas (below). The order of the frame is that of the description, so of the `.ui.sosu` file. A widget that is disabled is skipped; at the end, it goes round. Text fields do not type the Tab key.
- The widget that has the keyboard shows it: the accent border that text fields and lists already have, on the others too (a ring 2 pixels wide).
- What the keys do, to the widget that has the keyboard:
  - a button: Space or Enter clicks it;
  - a checkbox: Space checks or unchecks it; a radio: Space chooses it;
  - a dropdown: Space, Enter or Alt+Down opens it; closed, Up and Down change the choice; open, Up and Down move the highlight, Enter chooses, Escape closes;
  - a list: Up, Down, Home, End, Page Up, Page Down move the choice, which comes into view;
  - a `Scroll` area: the arrows, Page Up, Page Down, Home and End scroll it.
- Menus: Alt pressed and released alone, or F10, gives the keyboard to the first title of the bar. Left and Right go from title to title (from menu to menu when one is open), Down or Enter opens, Up and Down go from item to item, skipping separators and disabled items, Right opens a submenu and Left closes it, Enter chooses, Escape closes one level. The first context menu of the widget that has the keyboard opens with Shift+F10 or the Menu key, under that widget.
- `ui.key` already receives every key; Alt alone needs its release too (`ui.key(code, false, ...)`, which `platform` already gives). Mnemonics (`&File`, Alt+F) are not part of this batch.
- A widget can take the keyboard when the window opens: `TextField(bind: x, focus: true)` (`f.focus_here()` before the widget), applied once, the first time that identity is seen.

#### Scrolling

- `Scroll(width: w, height: h) { ... }` becomes `f.scroll(w, h)`, the nodes in a column, `f.end_scroll()`: an area `w` by `h` pixels, clipped, whose content scrolls when it is larger. `width` and `height` are required in this batch: the layout does not yet know how to fill what is left of the window (the `Split` panes and the grid will).
- The wheel scrolls the innermost area under the mouse that can still scroll that way (at its end, it goes to the one around it); Shift and the wheel, or the horizontal wheel (`Wheel(x, y)`), scroll sideways.
- A bar on the right (and at the bottom, when the content is wider) shows the position; its thumb can be dragged, and a click on the bar beside it moves by a page. This applies to `List` too, which becomes a `Scroll` with items: the dragging that lists lacked comes with it.
- When Tab gives the keyboard to a widget inside a `Scroll`, the area scrolls it into view (as `List` already does for its item chosen).
- What a `Scroll` keeps (its position, the size of its content at the last frame) is in the table of identities: there is no limit of 8 any more.

#### Tests

- `ui`: identities that survive an `if` and an insertion in a `for` with a `key:`; Tab and Shift+Tab around a frame with a disabled button; the keys of each widget; the menus with the keyboard; nested `Scroll` areas, the wheel at the end of the inner one, the dragged thumb, a focused widget brought into view.
- The translator: `at`, `push_id` and `pop_id` around loops and components, `key:`, `focus:`, `Scroll`.
- `tests/programs`: a screen with an `if` that inserts a field before the field being edited, which keeps the keyboard.

### 10.7 Placeholders, translations, themes and styles

Status: available (placeholders, translations, themes and styles).

#### Placeholders

- `TextField(bind: x, placeholder: "Search…")`: a hint drawn in the field, dimmed, while its text is empty, even when it has the keyboard; it never becomes text. `Format` gains `placeholder` (a `str`, "" for none): `text_field_with(&x, w, Format{ placeholder: "Search…" })`.
- `Dropdown(bind: x, placeholder: "Choose…")`: shown in its box while no option is chosen (no option is drawn chosen). `f.dropdown_with(width, placeholder)`.
- The color of the hint comes from the theme (`placeholder` in `TextField` and `Dropdown`, below). A literal placeholder is translated, as every literal text of a `.ui.sosu` file (below).

#### Translations

A program is written in one language (the source language, English in the examples), and translated by catalogs, one per language, read at run time: changing the language is choosing another catalog, and the next frame is in that language.

- Catalogs are files `<name>.locale.<language>.sosu` in `locales/`: `messages.locale.fr.sosu`, `messages.locale.fr-ch.sosu`, the language and, if needed, the country, in lower case; `messages.locale.default.sosu` holds the source texts, the template. Their content is that of gettext (`.po`), the format of translators and of their tools (Poedit, Weblate, Crowdin), with the source text as the key. `sosuc` reads the subset of the format that matters: `msgid`, `msgstr`, `msgctxt`, `msgid_plural` and `msgstr[n]`, comments, strings on several lines, the escapes `\"`, `\\`, `\n`, `\t`; and the header's `Plural-Forms`, whose expression (`n != 1`, `n > 1`, the Slavic forms with `%`, `&&`, `||`, `?:`) is evaluated by `i18n`. A missing or empty translation shows the source text. `sosuc fmt` does not format them, and the language server reports nothing in them. A program chooses the most specific catalog available: `fr-ch`, then `fr`, then the source language (`i18n.best`).
- A new package, `i18n`, without dependencies (in Sosu, through checked reads, as `render.truetype`): `i18n.load(arena, path) -> Catalog*!I18nError` (`Unreadable`, `Damaged` with the line), `catalog.text(source)`, `catalog.plural(one, other, n)`, `catalog.in_context(context, source)`, and `i18n.best(available, wanted) -> usize?`, which picks a language among those available from the preferred ones (`fr-CA`, then `fr`, then none).
- `platform`: `app.locales(arena) -> str[]`, the preferred languages of the user (`SDL_GetPreferredLocales`), first the most preferred, written `fr-FR`; the event `LocaleChanged` when they change.
- `ui`: `ui.catalog: i18n.Catalog*?` (none: the source language). Every literal text of a `.ui.sosu` file is translated: the labels of `Text`, `Title`, `Button`, `Checkbox`, `Radio`, `Option`, `Menu`, `MenuItem`, placeholders, and the `title` of a window. A text that is an expression (`Text(task.title)`) is data, and is not translated. In code written by hand: `f.tr("Save")`.
- Values in a text: `Text("Hello, {name}", name: user.name)`. The braces name values given by the node; each one must be given, and each value given must be used (an error otherwise, E0215). A translator can move them: `"Bonjour, {name}"`. A value in braces is a `str` (the translator works on the syntax, before the types are known); only `count:` is an integer, written in decimal digits (`f.decimal(n)` in code). Separators of thousands and decimals by language: later, with `NumberField`.
- Plurals: `Text("{count} file", plural: "{count} files", count: n)`: `count`, an integer, chooses the form, by the rule of the catalog (by the rule of English without one).
- The same text in two meanings (`"Open"`, the verb and the state): `context: "verb"`, the `msgctxt` of gettext.
- Extraction: `sosuc i18n extract [package] -o locales/messages.locale.default.sosu` writes the template of the catalogs: every literal text of the `.ui.sosu` files of the package, and the literal arguments of `f.tr(...)` in its `.sosu` files, each with its file and line (`#: screen.ui.sosu:12`), its context and plural. Translators start from it (`msginit`, Poedit, after renaming the file `.pot`), and update their catalogs from it (`msgmerge`): `sosuc` writes the template, it does not merge.
- What is not translated in v0: the shortcuts of menus (`Ctrl+S`), the direction of text (right to left), numbers, dates and money formatted by language.

#### Themes and styles

The colors and sizes of the widgets, fixed in the code of `ui` today, come from a theme; styles (the classes of CSS) change them for some nodes. Both are written in `.ui.sosu` files, beside the windows, and checked by the compiler as the rest.

```sosu
// theme.ui.sosu
theme Dark {
    accent: #89B4FA               // names of the theme: colors and numbers used below
    surface: #313244
    text: #CDD6F4

    Window { background: #1E1E2E }
    Text { color: text }
    Button {
        background: surface
        color: text
        radius: 6
        padding: 16
        hover { background: #45475A }
        pressed { background: #181825 }
        focus { border: accent, border_width: 2 }
        disabled { color: #6C7086 }
    }
    TextField {
        background: #181825
        placeholder { color: #7F849C }
        focus { border: accent }
    }
}

theme Light : Dark {              // a theme can start from another one, and change what differs
    accent: #1E66F5
    surface: #CCD0DA
    text: #4C4F69
    Window { background: #EFF1F5 }
}

style Danger {
    background: #F38BA8
    color: #1E1E2E
    hover { background: #EBA0AC }
}

style Card {
    background: #181825
    border: #313244
    radius: 8
    padding: 12
}
```

```sosu
// screen.ui.sosu
import app.theme;

window Tasks(state.Tasks* s) {
    Column(style: theme.Card) {
        Text(s.title, style: s.late ? theme.Danger : theme.Normal)
        Button("Delete", on_click: s.delete, style: theme.Danger)
        Button("Archive", style: [theme.Card, theme.Danger])  // several: the later ones win
    }
}
```

- Properties: `color` (of text), `background`, `border` (its color), `border_width`, `radius` (rounded corners, antialiased: `render` gains `fill_rounded` and `stroke_rounded`), `padding` (inside the widget or the layout), `gap` (between the widgets of a `Column` or a `Row`), `height` (of a line of widgets), `font_size` (pixels per em), `accent` (the mark of a checkbox or a radio, the cursor of a text field, the ring of the keyboard, the selection). A value is a color, `#RRGGBB` or `#RRGGBBAA` (a token of `.ui.sosu` files), a number, or a name of the theme; any Sosu expression of the right type (a `canvas.Color`, an `f32`) is accepted too.
- States, inside a selector or a style: `hover`, `pressed`, `focus` (has the keyboard), `disabled`, `checked` (a checkbox checked, a radio chosen, an item chosen), `open` (a dropdown, a menu); and the parts `placeholder` (the hint of a field or of a dropdown), `selection` (the text selected), `bar` (the scroll bars of a `Scroll` or a list). Several states can apply: `focus` and `hover` both, in the order of this list.
- Selectors of a theme: the widgets by the name of their node (`Window`, `Text`, `Title`, `Button`, `TextField`, `Checkbox`, `Radio`, `Dropdown`, `List`, `Scroll`, `Column`, `Row`, `TitleBar`, `MenuBar`, `Menu` for the open menus, `Image`), with their states.
- The cascade, from the weakest: the theme `Default` of `ui` (the colors of today), the selector of the widget in the theme, its states, then the styles of the node in their order, and their states. `color` and `font_size` are inherited: a `Column(style: Card)` gives its text color to the labels inside, unless they have their own.
- What they become: `theme Dark` becomes `pub widgets.Theme Dark()`, `style Danger` `pub widgets.Style Danger()`, values that the program keeps and passes; `theme.Normal` is a style that changes nothing (`widgets.Style{ }`). In `style:`, a name that starts with a capital (`Danger`, `theme.Danger`) is called; any other expression is a `widgets.Style` already (`s.style`). Themes and styles have no parameters in v0.
- The names of a theme become constants of its function (`auto accent = widgets.rgb(...)`; a whole number is an `f32`); those named `color`, `font_size` and `accent` are also its `all`. A selector or a style becomes a `widgets.Style{ normal: widgets.Props{ ... }, hover: ... }`, each property `some(value)`.
- `theme Light : Dark`: the parent is a theme written above, in the same file. Its body is read again with the names of `Light`, so that a name `Light` changes applies to the selectors it inherits; a selector both give is `widgets.merge(parent, child)`. `widgets.over(weak, strong)` does the same for two themes in code: `widgets.over(theme, widgets.Theme{ all: widgets.Props{ font_size: some(22.0) } })`.
- A node with `style: s` is preceded by `f.style(s)`, which the next node takes (4 styles at most, the later ones win); `style: [a, b]` gives both, in order. A `Column` or a `Row` without `gap:` or `padding:` takes them from the theme and its styles (`f.column(-1.0, -1.0)`).
- The program chooses the theme: `ui.theme = theme.Dark()`, at any time; without one, `widgets.default_theme()`. `ui` also gives `widgets.light_theme()`. `platform` says what the user chose: `app.system_theme() -> SystemTheme` (`Light`, `Dark`, `Unknown`, from `SDL_GetSystemTheme`) and the event `SystemThemeChanged(dark)`; a program that follows the system switches between two themes.
- `Window { background: ... }` is drawn by `widgets.begin` over the whole surface, so the program no longer clears it itself (it still can, before `begin`).
- Not in v0: selectors by position (`Column > Text`), styles by state of the program other than through an expression (`style: s.late ? Danger : Normal` covers it), transitions and animations, other fonts than one per size (bold and italic need more fonts in `ui.font`).

#### Tests

- Placeholders: drawn while the text is empty, not with text, never typed into the text; the dropdown with no option chosen.
- `i18n`: catalogs read (contexts, plurals, strings on several lines, escapes, a damaged file with its line), the plural rules of English, French, Russian and Arabic, `best` with regions and fallbacks.
- The translator: literals translated, expressions not; values in braces, missing or unused (errors); plural and context; `theme` and `style` blocks, `#RRGGBB`, inheritance of themes, `style:` with one style, several, an expression.
- `ui`: the cascade (theme, state, styles in order, inheritance of `color`), rounded corners drawn, a frame drawn with `light_theme` and with a theme of a `.ui.sosu` file.
- `sosuc i18n extract` on a package: the template, its references, contexts and plurals; `tests/programs` drives a screen in two languages and two themes.
- `examples/workspace` in French and English, in a light and a dark theme following the system.

### 10.8 Containers: grid, group, tabs, tree

Status: available. Written in four batches, in this order: `Grid` (it brings the sizes that fill what is left), `Group`, `Tabs`, then `Tree`. Each relies on §10.6 (identities, the table of states, the keyboard) and §10.7 (themes: each one is a selector, with its states and parts).

```sosu
window Settings(state.Settings* s) {
    Tabs(bind: s.page) {
        Tab("General") {
            Grid(columns: [Auto, Fill], gap: 8) {
                Text("Name", row: 0, col: 0)
                TextField(bind: s.name, row: 0, col: 1)
                Text("Folder", row: 1, col: 0)
                TextField(bind: s.folder, row: 1, col: 1)
                Group("Options", row: 2, col: 0, span: 2) {
                    Checkbox("Open at start", bind: s.at_start)
                    Checkbox("Check for updates", bind: s.updates)
                }
            }
        }
        Tab("Files", closable: true, on_close: s.close_files) {
            Tree(selected: s.file) {
                for (Node* n in s.roots, key: n.id) {
                    Branch(n.name, open: n.expanded) {
                        for (Node* c in n.children, key: c.id) {
                            Leaf(c.name, on_click: s.open(c))
                        }
                    }
                }
            }
        }
    }
}
```

#### Sizes

Until now a widget has the size it asks for, and `Scroll` needs `width:` and `height:`. A grid divides space between its columns, so a size can now be one of:

- a number of pixels: `120`;
- `Auto`: as large as its content, measured at the last frame (as a painted column already keeps its size, §10.7); the first frame lays it out at 0, the next one at its size, and the program draws continuously, so this is not seen;
- `Fill`, or `Fill(2)`: a share of what is left, after the numbers and the `Auto` sizes, in proportion to the weights (1 by default).

`widgets.Size` is an enum with data: `Pixels(f32 px)`, `Auto`, `Fill(f32 weight)`, for the columns and rows of a grid. In a `.ui.sosu` file, a number stands for `Pixels(n)`, and `Auto`, `Fill` (a weight of 1) and `Fill(2)` are written without qualifier. `width:` and `height:` of `Scroll`, `List`, `Tree`, `TextField` and `Dropdown` take `Fill` too, which becomes `f.fill_width()` or `f.fill_height()`: what is left of the layout, or the cell in a grid (`Auto` is the size of the widget). So `Scroll(width: Fill, height: Fill)` fills its cell; a `Scroll` still needs a size when nothing around it gives one.

#### Grid

- `Grid(columns: [...], rows: [...], gap: g, padding: p) { ... }`: `columns` is required (16 at most); `rows` is optional, `Auto` for each row not given (64 rows at most). `gap` is between cells, both ways.
- A node in a grid gives its cell: `row: r, col: c` (from 0), and `span: n` (columns) or `row_span: n`. A node without them takes the next free cell, row by row, as text is written: the common form of a form is then `Text`, field, `Text`, field. A cell taken twice is an error at run time only if the program computes it; literal places are checked by the translator (E0215).
- `align:` places a widget in its cell when it is narrower: `Start` (the default), `Center`, `End`, `Stretch` (a field or a button as wide as its cell). Across a `Column` or a `Row`, `align:` places a widget in the room the layout has (`f.align(how)` before it): in a column, the width it can fill; in a row, its height, or a line. A column or a row is not aligned itself. A node with `col:` and no `row:` goes in the current row if that column is still free, else in the next one; with `row:` and no `col:`, in the next free cell of that row.
- What it becomes: `f.grid(gap, padding)`, `f.grid_column(size)` for each column and `f.grid_row(size)` for each row given, then `f.cell(row, col, span, row_span, align)` before each node that gives its place (`-1`: the next free cell), and `f.end_grid()`. The sizes measured by its cells at the last frame are kept in `Ui` by the identity of the grid (8 grids, 16 columns and 64 rows each); a container placed in a cell starts there, and fills its width. A grid is painted as a column: its style can give it a background and a border.
- A grid with `Fill` columns fills the width it is given: the window, a cell, a tab, or a `Scroll`'s width. A grid at the top of a window fills the window, so a form follows its resizing.

#### Group

- `Group("Options") { ... }`: a frame with a title, drawn on its border (the `fieldset` of HTML, the `GroupBox` of WPF), holding a column of nodes; `Group { ... }` without a title is a frame alone. The title is translated, as every literal text (§10.7).
- `gap:` and `padding:` as a `Column`; `collapsible: true` adds a triangle before the title: a click (or Space, when the title has the keyboard) folds the group to its title. Its state is kept by identity, or bound: `open: s.options_open`.
- What it becomes: `f.group(title, gap, padding)` (`f.group_untitled(gap, padding)` without a title), the nodes, `f.end_group()`; when it folds, `if (f.group_folding(title, gap, padding, some(&open) or none)) { the nodes }`, then `f.end_group()`. A group that folds has a title.
- Theme: the selector `Group`, with the part `title` (in `widgets.Style`, the field `placeholder`).

#### Tabs

- `Tabs(bind: s.page) { Tab("General") { ... } Tab("Files") { ... } }`: a row of tabs, and under it the content of the tab chosen only; `bind:` is an `i32`, the index of the tab (from 0). Without `bind:`, `Tabs` keeps its choice by identity.
- `Tab(title, closable: true, on_close: handler)`: a cross on the tab (`on_close:` alone gives it one); `enabled: false` dims it. `Tabs(placement: Bottom)` puts the row below the content: at the bottom of the room it has, or under the content of the last frame (`Top` by default; left and right: later).
- The content of a tab that is not chosen is not described at all: its widgets do not exist in that frame, and keep nothing (§10.6), except what the program keeps; a `Scroll` in it is at its top again when it comes back. Keeping it is a question below.
- Too many tabs for the width: two arrows at the end of the row scroll it by 120 pixels, as the wheel does; a tab chosen (a click, a key, Ctrl+Tab) comes into view.
- Keyboard: the row of tabs takes the keyboard (Tab); Left and Right, Home and End choose; Ctrl+Tab and Ctrl+Shift+Tab choose the next and previous tab from anywhere inside the `Tabs`, Ctrl+W closes a closable tab.
- What it becomes: `f.tabs(some(&page) or none)` (`f.tabs_at(chosen, widgets.Side.Bottom)` with `placement:`), then for each tab `if (f.tab(title, closable, enabled)) { the nodes }` (`f.tab` returns true for the tab chosen, the only one to describe its content), `if (f.tab_closed()) { on_close(); }` when it has `on_close:`, and `f.end_tabs()`. Each tab is drawn when it is described, its title measured then; a click or a key chooses at the next frame, so a frame describes one content only.
- Theme: the selector `Tabs`, its states `hover`, `checked` (the tab chosen), `disabled`, and the part `bar` (the row).

#### Tree

- `Tree(selected: s.file, width: w, height: h) { ... }`: a `List` whose items are indented branches and leaves; it scrolls as a `List` (it is one), and is chosen the same way. `selected:` is an `i32`, the index of the visible item, as for `List`; `Leaf(..., on_click: ...)` gives a leaf its own action.
- `Branch(text, open: s.n.expanded) { ... }`: a triangle and a text; its children, `Branch` and `Leaf` nodes (in `if` and `for` too), are described only when it is open. `open:` binds the state to a `mut bool` of the program; without it, the branch keeps it by identity, closed at first (a `for` with a `key:` is then needed for it to follow its item, §10.6). `icon: state.folder` (an `image.Image`) puts it before the text, 16 pixels high, for a leaf too.
- `Leaf(text)`: an item without children. Branches and leaves holding other widgets (`Branch { Checkbox(...) Text(...) }`) are for later, with drag and drop between items.
- Keyboard, on the item chosen: Up and Down move; Right opens a branch, or goes to its first child; Left closes it, or goes to its parent; Enter is `on_click`; `*` opens all under it (later).
- What it becomes: `f.tree(width, height, some(&selected) or none)`, then `if (f.branch(text, some(&open) or none)) { children; f.end_branch(); }` and `if (f.leaf(text)) { on_click(); }` (`leaf` is true when clicked, or on Enter; `branch_with` and `leaf_with` with an icon), and `f.end_tree()`. The depth of a branch indents its items by 16 pixels.
- Theme: the selector `Tree`, its states `hover`, `checked` (the item chosen), `focus`, `open` (a branch), and the parts `selection` and `bar` of `List`.

#### Tests

- `ui`: a grid with pixels, `Auto` and `Fill` columns, at two widths; the next free cell; `span`; `align`; a `Scroll` that fills its cell. A group, folded and open. Tabs: the content of the chosen tab only, the keys, Ctrl+Tab, a closed tab, the row that scrolls. A tree: branches open and closed, bound and kept by identity, the keys, the indentation, a choice that survives a branch opened above it.
- The translator: `Grid` with `row:`, `col:`, `span:`, `align:`; `Size` values (`Auto`, `Fill`, `Fill(2)`, a number); cells given twice (E0215); `Group`, `Tabs` and `Tab`, `Tree`, `Branch` and `Leaf`; a `Tab` outside `Tabs`, a `Leaf` outside a `Tree` (E0215).
- `examples/containers`: a grid with every size, span and alignment; groups; tabs; a tree of folders, recursive, with icons. `examples/workspace`: its three panes in a grid that follows the window, the details of a file in tabs (General, Advanced), the files in a tree of folders, a group of options.

Open questions:

- The content of a tab that is not chosen: forgotten, or described hidden, so that its scrolls and its focus survive? (decided: forgotten; immediate mode does not describe it, and the program has the state that matters)
- `Auto` sizes from the last frame, or a first pass that measures without drawing? (decided: the last frame, as the painted columns; a pass of measure would describe every frame twice)
- Trees of many items (a file system): every visible item is described at each frame, as a `List`; virtualization (describing only what is in view) needs the height of each item before it is described. (proposed: later, with `List` of many items, by a fixed height of item)

### 10.9 The rest of the interface: text, numbers, tables, popups, input, mobile

Status: available. Everything the interface still lacked, in one branch, in batches, each with its tests: the editing core, `TextArea`, `NumberField` and `MoneyField`, `Slider` and `Progress`, tooltips, dialogs, `DateField`, long lists and `Table`, mnemonics, styles and themes with parameters, the built-in font beyond ASCII, input methods, then touch screens and mobile events.

#### The editing core

- `widgets.LongText`: text that grows, in an arena of its own (§6: a value that owns an arena is moved, so it lives in a state class). `TextBuffer` (256 bytes) stays for short fields. Both offer `text()`, `set`, `insert`, `remove`, and the editing core works on either. The translator does not know types, so `TextField(bind: x)` takes a `TextBuffer` and `TextArea` a `LongText`; in code, `f.text_field_long(&x, width, format)` is a field of one line over a `LongText`.
- Moving and deleting by word: Ctrl+Left and Ctrl+Right (with Shift: selecting), Ctrl+Backspace and Ctrl+Delete; a double click selects a word, a triple click the line (the text, in a field of one line).
- Undo and redo: Ctrl+Z, Ctrl+Y and Ctrl+Shift+Z, in the field that has the keyboard. `Ui` keeps the edits of that field (64 at most, their text in a pool of 4 KB); typing letters in a row is one edit; the history is forgotten when the keyboard goes to another widget.

#### `TextArea`

- `TextArea(bind: s.notes, rows: 8, width: Fill, wrap: true, placeholder: "Notes")`: text on several lines, bound to a `LongText`. Enter breaks a line (Ctrl+Enter is `on_enter:`); Tab moves the keyboard, unless the area has `tabs: true`: Tab then types four spaces.
- Up and Down keep the column of the cursor; Page Up and Page Down move by the height of the area; Home and End go to the ends of the line, Ctrl+Home and Ctrl+End to those of the text.
- `wrap: true` breaks lines at the last space that fits (else anywhere); `wrap: false` scrolls sideways. The area scrolls as a `Scroll` (§10.6) to keep the cursor in view; the mouse places the cursor and selects, by line.

#### `NumberField` and `MoneyField`

- Numbers are integers, also when they have decimals: `NumberField(bind: s.price, decimals: 2)` binds an `i64` in hundredths, so no float is ever rounded on its way to the screen and back. One widget, without generics.
- `NumberField(bind: x, min:, max:, step:, decimals: 0, decimal: ".", group: "")`: digits, one `-` first, and one decimal separator; Up and Down (and the wheel over it, when it has the keyboard) add or take `step`; two small buttons on the right do the same. The text being typed may be out of range or incomplete (`-`, `12.`): the value is set when the text is a number in range, and the field shows the value again when it loses the keyboard.
- `MoneyField(bind: s.total, symbol: "€", decimals: 2, decimal: ",", group: " ", symbol_first: false)`: a `NumberField` that shows its symbol and its groups of thousands when it does not have the keyboard. Separators are explicit (§14.2), not read from the system.
- `f.number_field(i64* value, Number format)` and `f.money_field(...)`, `widgets.Number` gathering `min`, `max`, `step`, `decimals`, `decimal`, `group`, `symbol`.

#### `Slider` and `Progress`

- `Slider(bind: s.volume, min: 0, max: 100, step: 1, width: 200)`: an `i64`, dragged by its thumb, clicked on its track, Left / Right / Home / End / Page Up / Page Down with the keyboard.
- `Progress(value: s.done, max: s.total, width: Fill)`: a bar; `Progress(width: 200)` without a value moves by itself (indeterminate), which needs the time (below).

#### Time, tooltips

- `ui` does no input or output (§10.4): the program gives it the time, `ui.clock(ms)`, a monotonic time in milliseconds, before each frame: `ui.clock(app.now_ms())` (`now_ms` reads the clock of SDL with the effect `io`, which `on_frame` has; `std.time` would need the effect `time`). Without it, what needs time (tooltips, an indeterminate progress, the caret blinking) stays still.
- `tooltip: "Save the file"` on any widget: after 500 ms over it without moving, a small box under the mouse, on the layer above, until the mouse leaves; its text is translated as every literal.

#### Dialogs

- `Dialog(open: s.asking, title: "Delete the file?", width: 400) { ... }`: a window inside the window, centered, over a darkened backdrop, on the layer above. While it is open, the widgets under it see neither the mouse nor the keys; Tab stays inside it; Escape, or a click on its cross, sets `open` to false. Its content is described where the node is, and drawn above the rest at `finish()` as the open menus are.
- What it becomes: `if (f.dialog(title, width, &open)) { the nodes; f.end_dialog(); }`.

#### `DateField`

- `DateField(bind: s.due, monday_first: true, format: "dd/mm/yyyy")`: a field with a mask (§10.4) and a button that opens a calendar of the month on the layer above (as a dropdown): arrows for the months, a click or Enter on a day chooses it, Escape closes. `widgets.Date { year, month, day }`; a date not valid in the field leaves the value as it was.
- The names of the months and days are English texts translated by the catalog (§10.7), as the other literals of `ui`.

#### Long lists and `Table`

- A `List` of many items describes only those in view: `List(count: s.count, item: i, selected: s.chosen, height: Fill) { Text(s.names[i]) }`. `item: i` names the index of the item being described, an `i32`; every item is as high as the first one (measured at the last frame), so the list knows which ones show without describing the others. What it becomes: `f.list_of(count, width, height, selected)`, then `for (i32 i in f.first_shown()..f.last_shown()) { push_id(i), the nodes, pop_id() }`, `f.end_list()`.
- `Table(count: s.count, item: i, selected: s.row, sort: s.order, height: Fill) { Column("Name", width: Fill, sortable: true) { Text(s.files[i].name) } Column("Size", width: 100, align: End) { Text(s.size_text(i)) } }`: a header with the titles of the columns, then rows of cells, only those in view, as a long `List`.
  - The widths of the columns are sizes (§10.8: pixels, `Auto`, `Fill`); the border between two titles can be dragged, and the width chosen is kept by the identity of the table.
  - `sort:` binds a `widgets.Sort { column, descending }`: a click on a sortable title sets it (again: descending); the program sorts its rows (`ui` only shows the order with an arrow on the title).
  - Rows are chosen as the items of a `List` (click, Up, Down, Page Up, Page Down, Home, End); `on_activate:` on Enter or a double click.
  - What it becomes: `f.table(count, width, height, selected, sort)`, `f.table_column(title, size, sortable, align)` for each column, then for each row shown, `f.table_cell()` before the nodes of each column, and `f.end_table()`.
- Trees stay described whole (they hold fewer items than lists, and their items are not all as high).

#### Mnemonics

- `&` in the text of a `Menu`, a `MenuItem`, a `Button`, a `Checkbox`, a `Radio` or a `Tab` marks its letter: `Menu("&File")`, `Button("&Save")`; `&&` is an `&`. The letter is underlined while Alt is held, or once the menus have the keyboard.
- Alt and the letter opens the menu of the bar, clicks the button, checks the box, chooses the radio or the tab; in an open menu, the letter alone chooses the item. Letters given twice: the first one in the order of the frame. Translations carry their own `&`.

#### Styles and themes with parameters

- `style Size(f32 px) { font_size: px }` becomes `pub widgets.Style Size(f32 px)`, and `style: Size(22)` calls it; a theme can have parameters too, `theme Brand(canvas.Color accent) : Dark { ... }`, and a theme that starts from it gives them: `theme Red : Brand(#D20F39) { }`. Parameters are typed as the parameters of a function (§5).
- States of a widget other than those of §10.7: still not in v0; an expression in `style:` covers the state of the program.

#### The built-in font beyond ASCII

- Without a TrueType font (`ui.font`), the 8 by 8 font of `render` draws ASCII only. It gains the letters of Latin-1 and of Latin Extended-A (French, German, Spanish, Italian, Portuguese, the Nordic and Central European languages), each drawn as its base letter and a mark above or below it (acute, grave, circumflex, diaeresis, tilde, ring, cedilla, caron, ogonek...), from a table of 2 bytes per letter. Other characters stay boxes.

#### Input methods

- The composition of an input method (Chinese, Japanese, Korean, accents typed in two keys) is shown in the field that has the keyboard, underlined, before it becomes text: `platform` gives the event `TextEditing(str text, i32 cursor)` (`SDL_EVENT_TEXT_EDITING`) and `ui.compose(text, cursor)` receives it; an empty text ends the composition.
- `ui.caret_area()`: the rectangle of the cursor of the field that has the keyboard, which `platform` passes to the system (`app.set_text_input_area(x, y, w, h)`, `SDL_SetTextInputArea`) so that the window of candidates opens next to it.

#### Touch screens and mobile events (§10.3)

- `platform` gives `Touch(i64 finger, f32 x, f32 y, TouchPhase phase)` (`Down`, `Move`, `Up`, `Cancel`; `SDL_EVENT_FINGER_*`, in pixels of the window). `ui.touch(...)` treats the first finger as the mouse (a tap is a click; a long press, 500 ms, is the right button), and a finger that drags in a `Scroll`, a list or a table scrolls it, with inertia, instead of selecting. On the desktop, SDL also gives the first finger as the mouse: a program forwards either (the examples forward the mouse), not both.
- The virtual keyboard: `ui.wants_text()` is true while a text widget has the keyboard; the program shows or hides the keyboard of the system when it changes, `app.show_keyboard(on)` (`SDL_StartTextInput`, `SDL_StopTextInput`); the safe area says what the keyboard leaves.
- Safe areas: `SafeAreaChanged(f32 left, f32 top, f32 right, f32 bottom)` (`SDL_GetWindowSafeArea`); `ui.safe_area(...)`, and `widgets.begin` places the frame inside it (the background of the window is still drawn everywhere).
- Orientation and size: `Resized` already says it; a grid with `Fill` follows. The display scale: `ScaleChanged(f32 scale)` (`SDL_GetWindowDisplayScale`); a program keeps its widgets readable on a dense screen through its theme, `widgets.over(theme, widgets.Theme{ all: widgets.Props{ font_size: some(18.0 * scale) } })` (scaling every size of `ui` is for later).
- The life of the program: `Suspend` (`SDL_EVENT_WILL_ENTER_BACKGROUND`), `Resume` (`SDL_EVENT_DID_ENTER_FOREGROUND`), `LowMemory`, `Terminating`. While suspended, `platform` does not call `on_frame` and looks for events every 100 ms (on iOS, drawing in the background is not allowed). On mobile systems, SDL gives these events through an event watch: that is for step 9 (§13).
- On the desktop, a touch screen gives the same events; building for Android and iOS is step 9 of the roadmap (§13), outside this branch: the events are written and tested here, and run there.

#### Tests

- `ui`: words, undo and redo, a `LongText` of 100 KB; a `TextArea` (lines, wrapping, Up and Down, scrolling to the cursor); numbers typed, out of range, stepped, shown with their groups and symbol; a slider dragged and moved by keys; a tooltip after 500 ms; a dialog that takes the mouse and the keys and closes with Escape; a calendar opened, a day chosen; a list of 100 000 items described by the items in view only; a table sorted, a column resized, a row chosen; mnemonics with Alt; a composition shown then typed; a tap, a long press and a drag that scrolls.
- `render`: the accented letters of the built-in font, against their base letters.
- The translator: every new node and property, `item:` and `count:`, styles and themes with parameters, mnemonics in translated texts.
- `platform`: the translation of the new SDL events (touch, text editing, life, safe area) is checked by `sosuc check`, and tried on a window: running `platform` needs SDL and a display, which the tests of the CI do not have.
- `examples/editor` gains a `TextArea` with undo; `examples/workspace` its output in a long list, its files in a `Table` (name, size, date, sortable) and a tooltip; `examples/containers` a tab of inputs: numbers, money, a date, a slider, progress bars, a text area, a dialog, letters with Alt.

Open questions:

- A tab character in a `TextArea`: never in v0 (Tab moves the keyboard, as in a browser), or typed when the area has `tabs: true`? (decided: `tabs: true`, off by default; it types four spaces)
- `NumberField` with an `f64` once there are generics, or integers with decimals for good? (decided: integers; a float field can come later, for measurements)
- The time given by the program (`ui.clock`) or read by `ui` with the effect `time`? (decided: given; `ui` stays without effects, and its tests choose the time)

## 11. Targets and compilation

### 11.1 Targets

Target name: `--target <os>-<arch>`.

| Target | Architectures | Format | Required tooling (step 1, through C) |
|---|---|---|---|
| `windows` | x64, arm64 | PE | clang or MSVC |
| `linux` | x64, arm64 | ELF | gcc or clang |
| `macos` | arm64, x64 | Mach-O | clang (Xcode Command Line Tools) |
| `android` | arm64 (x64 for the emulator) | ELF `.so` | Android NDK + SDK; APK / AAB packaging through Gradle |
| `ios` | arm64 (+ arm64 simulator) | Mach-O | Xcode (clang + iOS SDK); a Mac is required |

Order: Android first (tooling usable on any OS), iOS next.

The web (WebAssembly) is not planned for v0; it can be added as a target later.

### 11.2 Mobile: how it works

- The Sosu program is compiled into a library (`.so` on Android, static library on iOS; `sosuc build --lib`, §9.2), loaded by a small host project (Android activity, Xcode project). SDL3 provides this host code; in v0, project templates are provided by hand, and `sosuc package --target android|ios` (v1) generates them.
- Sosu compiles ahead of time (AOT): no problem with the ban on run-time generated code on iOS.
- Files: always through `std.app` (sandbox), never hard-coded paths.
- Permissions and native APIs (camera, notifications, purchases...): dedicated packages, with host code (JNI on Android, Objective-C on iOS); outside v0.
- Zero dependencies applies to the compiler, not to mobile: the NDK, Xcode, signing and the stores remain mandatory. Even the future custom backend (step 2) will produce ARM64, but linking, signing and packaging will go through the official tools. The C backend is therefore the realistic path for mobile, and deserves to be kept as a permanent option.

### 11.3 Backends

1. Step 1: `sosuc` generates C99, compiled by `cc` (or by the target platform's compiler). The generated C contains `#line` directives, so that errors and the debugger point to the lines of the `.sosu` files.
2. Step 2: custom x86-64 / ARM64 backend (ELF, Mach-O, PE) on the same IR.
3. Step 3: self-hosting (Sosu compiles Sosu).

### 11.4 Compiler as a library, and commands

The compiler is designed as a library (`libsosu`: lexer, parser, type checking, IR) shared by all the tools, including the language server.

- `sosuc build [--target <os>-<arch>]`: compiles (cross-compiling depending on the available tooling). The generated C file and the program go to the `build/` folder of the package. The C compiler is MSVC on Windows (found through `vswhere`), `cc` elsewhere, or the one named by the `SOSU_CC` variable (gcc or clang style). `--target` is not implemented yet.
- `sosuc run`: compiles and runs in one command (useful for learning and workshops).
- `sosuc test`: compiles and runs the `test` blocks of the package, prints one line per test and a summary; the exit code is 1 if a test failed.
- `sosuc check`: checks a package without compiling it.
- `sosuc fmt [--check] path...`: formats files, or the modules of packages (not their dependencies), in place (§7.6). With `--check`, it changes nothing and fails if a file is not formatted (for the CI).
- `sosuc lsp`: the language server of editors (Language Server Protocol, JSON-RPC on stdin and stdout, behind `Content-Length` headers). While typing, it checks the package of the open file as `sosuc check` does, with the text of the open files in place of their files on disk, and publishes the diagnostics. It shows the type of what is under the mouse (the signature of a function or method, the type of a variable or field), goes to the definition of a name (variable, parameter, function, method, field, variant, type, module), and completes the members after `.` (fields and methods of a value, public names of a module, variants of an enum) and otherwise the names in scope (variables, functions, types, imported modules, keywords). Positions are in UTF-16 code units, as the protocol has them. Hover, definition and member completion need a package that loads without errors (semantic analysis only runs then); a file outside any package, or that no import reaches, is only parsed. Renaming and the references of a name come later.
- `sosuc build --lib[=shared]` (planned, §9.2): a static or shared library of the `export "C"` functions of the package, with its C header, for other languages and for mobile.
- `sosuc package` and `sosuc bindgen`: v1.

## 12. Grammar (v0, EBNF)

This is the grammar implemented by `sosuc`. `mut` belongs to declarations (variables, fields,
parameters), not to types.

```
program      = { import | decl } ;
import       = "import" path ";" ;
path         = IDENT { "." IDENT } ;
decl         = [ "pub" ] ( struct | class | interface | enum | function ) | extern | test ;

struct       = "struct" IDENT "{" { member } "}" ;
class        = "class" IDENT [ ":" path { "," path } ] "{" { member } "}" ;
member       = [ "pub" ] ( field | [ "virtual" | "override" ] function ) ;  (* virtual / override: classes only *)
field        = [ "mut" ] type IDENT [ "=" expr ] ";" ;
interface    = "interface" IDENT "{" { signature } "}" ;
enum         = "enum" IDENT "{" [ variant { "," variant } [ "," ] ] "}" ;
variant      = IDENT [ "(" [ type IDENT { "," type IDENT } ] ")" ] ;
extern       = "extern" STRING "{" { signature } "}" ;
test         = "test" STRING block ;

function     = [ "unsafe" ] type IDENT "(" [ params ] ")" [ effects ] { contract } block ;
signature    = [ "unsafe" ] type IDENT "(" [ params ] ")" [ effects ] ";" ;
params       = param { "," param } ;
param        = [ "mut" ] type IDENT ;
effects      = "uses" IDENT { "," IDENT } ;
contract     = ( "requires" | "ensures" ) expr ;

type         = base_type { "*" | "?" | "[" [ INT ] "]" | "!" base_type } ;
base_type    = path | "fn" "(" [ type { "," type } ] ")" "->" type [ effects ] ;

block        = "{" { stmt } "}" ;
stmt         = block | var_decl ";" | simple ";" | if | while | for | return
             | "break" ";" | "continue" ";" | "defer" ( block | simple ";" )
             | "unsafe" block | match ;
var_decl     = [ "mut" ] ( type | "auto" ) IDENT "=" expr ;
simple       = expr [ assign_op expr ] ;
assign_op    = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=" ;
if           = "if" "(" expr ")" block [ "else" ( if | block ) ] ;
while        = "while" "(" expr ")" block ;
for          = "for" "(" [ "mut" ] ( type | "auto" ) IDENT "in" expr ")" block
             | "for" "(" [ var_decl | simple ] ";" [ expr ] ";" [ simple ] ")" block ;
return       = "return" [ expr ] ";" ;

expr         = range [ "?" expr ":" expr ] ;
range        = binary [ ( ".." | "..=" ) binary ] ;
binary       = cast { binary_op cast } ;           (* precedence: §5.1 *)
cast         = prefix { "as" [ "?" ] type } ;
prefix       = ( "-" | "!" | "~" | "&" | "*" | "try" ) prefix | postfix ;
postfix      = primary { call | "[" expr "]" | "." IDENT | struct_lit } ;
call         = "(" [ expr { "," expr } [ "," ] ] ")" ;
struct_lit   = "{" [ IDENT ":" expr { "," IDENT ":" expr } [ "," ] ] "}" ;  (* after a path *)
primary      = INT | FLOAT | STRING | CHAR | "true" | "false" | "self" | "base"
             | IDENT | "(" expr ")" | match | array ;
array        = "[" [ expr ( ";" INT | { "," expr } [ "," ] ) ] "]" ;   (* [1, 2, 3] or [0; 64] *)
match        = "match" "(" expr ")" "{" [ arm { "," arm } [ "," ] ] "}" ;
arm          = pattern "=>" ( block | expr ) ;
pattern      = "_" | path [ "(" [ IDENT { "," IDENT } ] ")" ] ;
```

`.ui.sosu` files (sketch):

```
ui_file     = { import | window | component | style | theme } ;
window      = "window" IDENT "(" [ params ] ")" "{" { prop } node "}" ;
component   = "component" IDENT "(" [ params ] ")" "{" node "}" ;
style       = "style" IDENT "{" { prop | state } "}" ;                 (* see §10.2 and §10.7 *)
theme       = "theme" IDENT [ ":" IDENT ] "{" { prop | selector } "}" ;
prop        = IDENT ":" expr ;
node        = IDENT [ "(" [ arg { "," arg } ] ")" ] [ "{" { node | if | for | match } "}" ] ;
arg         = [ IDENT ":" ] expr ;
```

Inside a node, `if`, `for` and `match` follow the syntax of §5 and §7.1, with nodes in their blocks.

## 13. Roadmap

1. Lexer (C)
2. Parser (recursive descent) -> AST, import resolution by package (manifest)
3. Semantic analysis and type checking (including `match` exhaustiveness, inheritance and effects)
4. C generation, with `#line` (virtual tables for classes)
5. C interop, "Hello window" on desktop (SDL3)
6. Arenas, `defer`, move, `unsafe`, debug checks
7. Tooling: `sosuc fmt`, `sosuc test`, `sosuc run`, JSON diagnostics, contracts
8. `platform` / `render` / `ui` packages: window, rectangle, events, then the `.ui.sosu` translator
9. Libraries for other languages (`export "C"`, `sosuc build --lib`, §9.2), then mobile with the same output: Android (NDK, emulator), then iOS (simulator), with the same code
10. Generics (monomorphized), `Vec` / `HashMap` containers
11. `sosuc bindgen`, `sosuc package`, cross-compilation
12. Custom native backend (x86-64 / ARM64; ELF, Mach-O, PE)
13. Research: `comptime`, effect polymorphism, regions, concurrency
14. Self-hosting

## 14. Decisions and open questions

### 14.1 Decisions made

- Object model: `struct` (value) and `class` (single inheritance + interfaces); non-virtual methods by default, explicit `virtual` and `override` (§5.2).
- Modules: named packages from v0, with a manifest (§8.1).
- Effects: `uses` clause from v0 (§7.4).
- Mobile: Android first, iOS next (§11.1).
- Contracts: checked in debug, ignored in release.
- Array bounds: checked in release by default, can be disabled (`--checks=off`).
- Integer overflow: an error in debug, modulo arithmetic in release.
- Function overloading: excluded.
- Concurrency: threads and message channels, an arena belongs to a single thread, UI on the main thread.
- Program entry: the `platform` runtime owns the main loop and calls the program back through the interface `app.Handler`, implemented by a class that holds the state (§10.4).
- `.ui.sosu` syntax: only in dedicated files in v0.
- Interface hot reload: after v0.
- Manifest format: a subset of TOML (`sosu.toml`, §8.1), implemented.
- Source of dependencies in v0: local path or Git repository (`tag` or `rev`), without a central registry. Both are implemented; Git dependencies are cloned with the `git` command and pinned by `sosu.lock` (§8.1).
- A `struct` cannot implement an interface in v0 (reserved for classes); the parser rejects `struct S : I`.
- Imported names are qualified by the module name: `vector.Vec2`, `io.print(...)` (§8.1).
- C libraries are declared in `sosu.toml` only ([link] sections, §9.1), not in the source code.
- Lexical details settled by the implementation: §3.1 (literals), §5.1 (`as`, ranges, struct literals, function types), §12 (grammar).
- Array literals `[a, b]` and `[value; n]`; `&x` requires `mut x`; `result` is contextual (only in `ensures`) (§5.3).
- Moves: `Arena` and the values containing one are moved, checked in the order of the source; `.copy()` duplicates them with new, empty arenas (§6).
- Views `x[a..b]` of arrays and slices, without copy, bounds checked; a view of an array needs it `mut`, like `&x` (§4, §5.3).
- User interface (§10.4): `render` draws in software into a buffer of pixels, with a built-in 8 by 8 font; `ui` works in immediate mode (no lambdas in v0) and does not depend on `platform`, so that it is tested without a screen.

### 14.2 Open questions

- Effect polymorphism (higher-order functions): to be handled with generics in v1? (proposed: yes)
- Abstract classes: unnecessary, interfaces are enough? (proposed: yes)
- Class destructors: none in v0, external resources are released with `defer`? (proposed: yes)
- Uninitialized variables: the grammar requires a value at declaration, while §7.7 mentions checking "variables initialized before being read" in debug. (proposed: keep the value mandatory; the debug check then concerns memory from `alloc`)
- Multiple effects in a function type (`fn() -> void uses io, time`) rely on the list of predefined effects to tell them from the next parameter. User-defined effects would need another syntax. (proposed: no user-defined effects in v0)

Libraries for other languages (§9.2):

- C names of the exports: the Sosu name as written, or prefixed with the package automatically (`geometry_dot` from `dot`)? (proposed: as written, the author chooses; a collision with another library is a link error, as in C)
- Errors in an export: a guard and `<package>_error()`, or an error code in every exported signature? (proposed: the guard; an export that wants to report its own errors returns them, as a C function would)
- The guard leaves the `defer`s of the failed call unrun. Run them (unwinding the frames) or accept it in v0? (proposed: accept it; the host discards what the call used)
- `enum`s without data at the boundary, as `uint32_t` constants in the header? And `T?`, `T!E`? (proposed: enums without data yes, with their constants; options and results no, in v0)
- Several Sosu libraries in the same process: shared libraries hide their runtime, but two static ones would define it twice. (proposed: a static library comes with a separate `sosu_rt` library, linked once by the host)
- The message of the last error is kept per thread, which needs C11 (`_Thread_local`) or a compiler extension (`__declspec(thread)` with MSVC), while the generated C is C99. (proposed: use them where available; this is the only part of the runtime outside C99)
- Bindings for C# or Java generated by `sosuc` (`sosuc bindgen --to=csharp`)? (proposed: no; the header and the existing tools (`jextract`, ClangSharp, `cffi`) are enough, and keep the compiler free of any other language)

Widgets of `ui` (§10.4):

- Identity of the widgets: decided, see §10.6 (a hash of the place of the node in the file and of the keys of the loops around it).
- The key of an item in a loop: an expression after the loop, `for (Task* t in state.tasks, key: t.id)`, or a node `Key(t.id) { ... }`? (proposed: `key:` in the loop; it says what the loop is over, and adds no node)
- Alt alone for the menus: Windows and Linux do it, macOS does not (its menus are in the bar of the system, §14.2 above). (proposed: Alt and F10 everywhere in v0, the same on every system)
- Catalogs of translations: gettext files, read at run time, or a format of Sosu compiled into the program? (decided: the content of gettext, the format of translators and of their tools, in files named `<name>.locale.<language>.sosu`; read at run time, a program changes language without being rebuilt, and a translator tries a catalog)
- The key of a translation: the source text (gettext), or a name (`greeting.hello`)? (proposed: the source text; the `.ui.sosu` file stays readable, and a missing translation shows a real text)
- Styles and themes with parameters (`style Size(f32 s) { font_size: s }`), and styles chosen by the state of a widget other than those of §10.7? (proposed: not in v0; an expression in `style:` covers the state of the program)
- Formats of numbers and money: read from the locale of the system, or given explicitly? (proposed: explicit in v0, `decimal: ","`; the same program then shows the same thing everywhere, and tests do not depend on the machine)
- Money: an integer of minor units (`i64` cents), or a decimal type of the language? (proposed: `i64` minor units in v0; a currency with 3 decimals gives `places: 3`)
- `NumberField` without generics: one widget per type (`i64`, `f64`), or a text field and a conversion by the program? (proposed: one per type until generics, then a single generic one)
- `Radio` and `Dropdown` compare values of an `enum`: the translator generates a `match`, or the language gains `==` on `enum`s without data? (proposed: `==` on `enum`s without data, useful everywhere; the translator uses it)
- Text that grows: a `TextBuffer` that owns an arena is moved, not copied (§6). Acceptable for a field of a state class? (proposed: yes; the state is a class, handled through a pointer, so nothing has to be copied)
- The layer above answers clicks from its rectangle at the previous frame: one frame late when a dropdown opens. (proposed: accept it; it is the usual price of immediate mode)
- Title bars (§10.5): the drag area is asked of the program point by point (`app.TitleBar`), or `ui` gives `platform` a list of rectangles at each frame? (proposed: asked point by point; nothing is copied at each frame, and `platform` does not need to know the types of `ui`)
- Title bars: Windows 11 shows its snap layouts when the mouse rests on the maximize button, which needs `HTMAXBUTTON`, an answer that SDL's hit test does not have. (proposed: accept it in v0; a Win32 backend could add it)
- Title bars: the double-click that maximizes, on macOS and Linux, where SDL moves the window itself. Detect it in `ui` and send `ToggleMaximize`? (proposed: later; SDL takes the clicks of the drag area before the program sees them)
- Menus are drawn by `ui`, the same on every system. On macOS, the menus of an application belong in the bar at the top of the screen: give them to the system there (`NSMenu`, through a backend outside of SDL), or keep them in the window? (proposed: in the window in v0; the bar of macOS later, from the same `MenuBar`)
- Menus and the keyboard: Alt (or F10) to reach the bar, the arrows between menus and items, Enter to choose, the letter of an item. (proposed: later, with the keyboard navigation of §10.4, foundation 5)
- Accessibility (screen readers) needs the tree of widgets, which immediate mode does not keep. (proposed: later; the frame could record its widgets as it draws them)
