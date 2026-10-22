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
- The `core` and `std` packages are found in `$SOSU_HOME/lib`, or else in `../lib` next to `sosuc`.
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

[link.windows]                  # added on one target: windows, linux, macos, android, ios
paths = ["vendor/SDL3/lib/x64"] # where the linker looks, relative to the package
copy = ["vendor/SDL3/lib/x64/SDL3.dll"]

[link.macos]
frameworks = ["Cocoa"]
```

- `libs`: library names, translated for the C compiler; `paths`: folders searched by the linker; `frameworks`: macOS and iOS frameworks; `copy`: files placed next to the program (a DLL must be beside the `.exe` on Windows).
- Paths are relative to the package, never absolute, so that the manifest works on every machine.
- Linking is transitive: the libraries of a dependency are linked into the program that uses it, which does not need to know them. Duplicates are removed.
- The values are one-line lists of strings. [link.<target>] uses the target names of `--target` (§11.1); for now, the target is the system running `sosuc`.

## 10. User interface

The UI is a set of packages, never compiler syntax. Three layers, each replaceable:

| Layer | Role | v0 choice |
|---|---|---|
| `platform` | window, events, touch input, clipboard, cursor, high-density screens, lifecycle | SDL3 (alternative: `sokol_app`) |
| `render` | rectangles, lines, text, images | software rendering (pixel buffer), then GPU (Metal, Vulkan, D3D or SDL_GPU) |
| `ui` | layout, widgets, state, declarative syntax | widgets drawn by Sosu, identical rendering everywhere |

- SDL3 handles Windows, Linux, macOS, Android and iOS. Its interface stays narrow so that custom backends can be added later (Win32, Cocoa, X11 / Wayland, UIKit, native Android).
- Text is the real difficulty: `stb_truetype` or FreeType at first; fonts, HiDPI and international input (IME) must be planned from the start.
- Accessibility (screen readers): to be planned, outside v0.

### 10.1 Events and loop

Events are a sum `enum`, handled by an exhaustive `match`. On mobile, the OS owns the main loop: the `platform` runtime therefore owns it on every platform, and the program provides callbacks.

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
- `window Name(params) [uses ...] { properties, then nodes }` becomes `pub void Name(widgets.Frame* ui_frame, params)`. Its properties `title`, `width` and `height` become `pub widgets.Window NameConfig()`, which the program passes to `platform` (`app.Config`). `component Name(params) { nodes }` becomes the same function, without properties. A node `Name(values)`, or `module.Name(values)`, calls a component, its values in order.
- Nodes, and what they become (`f` stands for `ui_frame`); a number of pixels can be written `8` for `8.0`:

| Node | Becomes |
|---|---|
| `Column(gap: g, padding: p) { ... }`, `Row(...)` | `f.column(g, p)` (or `f.row_with(g, p)`), the nodes, `f.end()`; `gap` is 8 and `padding` 0 by default |
| `Text(s)`, `Title(s)`, `Space(n)` | `f.label(s)`, `f.title(s)`, `f.space(n)` |
| `Button(s, on_click: h, enabled: c)` | `if (f.button_enabled(s, c)) { h(); }`; `h` is a function or a method (`state.save`), or a call (`state.save(1)`) |
| `TextField(bind: x, width: w, on_enter: h)` | `if (f.text_field(&x, w)) { h(); }`; `bind:` is required, `width` is 200 by default |
| `Checkbox(s, bind: x, on_change: h)` | `if (f.checkbox(s, &x)) { h(); }`; without `s`, `f.checkbox_alone(&x)` |

- `if`, `else` and `for (T x in ...)` hold nodes, as in the examples.
- Not yet: `style` and `theme` blocks, `style:`, `align:`, grids (`row:`, `col:`, `span:`), `List`, `bind:` on other widgets than `TextField` and `Checkbox` (error E0216). `.ui.sosu` files have no formatter yet; `sosuc parse file.ui.sosu` shows the functions they become.

Grammar of a `.ui.sosu` file (the expressions, types and parameters are those of §12):

```ebnf
ui_file      = { import | view } ;
view         = ( "window" | "component" ) IDENT params [ effects ] "{" { property } { item } "}" ;
property     = IDENT ":" expr ;                                (* window only: title, width, height *)
item         = node | "if" "(" expr ")" children [ "else" ( item | children ) ]
             | "for" "(" [ "mut" ] ( type | "auto" ) IDENT "in" expr ")" children ;
node         = path "(" [ argument { "," argument } ] ")" [ children ] ;
argument     = IDENT ":" expr | expr ;
children     = "{" { item } "}" ;
```

### 10.3 Mobile specifics

- Touch screens: `Touch` events, gestures in `ui`.
- Safe areas (notches, system bars): `SafeAreaChanged`.
- Orientation, virtual keyboard and varying screen sizes: adaptive layout in `ui`.
- Lifecycle: `Suspend` / `Resume` / `LowMemory`; on iOS, rendering must stop in the background.

### 10.4 Milestones

1. Desktop: open a window, show a colored rectangle, close cleanly on `Quit`. Done on Windows and Linux with `packages/platform` and `examples/window`; `tests/window` checks it in the CI. Without generics to carry the application state, its v0 interface differs from the sketch of §10.1: the program implements the interface `app.Handler` with a class, whose fields hold the state, and calls `app.run(config, handler)`. `on_event(Event e)` returns `false` to close the window; `on_frame(canvas.Canvas* surface)` draws on a buffer of pixels the size of the window (package `render`, below). The events are `Quit`, `Resized`, `Key`, `MouseMove`, `Click`, `Release` and `TextInput` (its `str` lives until the end of the frame); `app.request_quit()` closes the window as the window button does.
   - `render.canvas` draws in Sosu into a `Canvas` (pixels 0xAARRGGBB): `clear`, `fill` (blended when the color is not opaque), `stroke`, `line`, `text` and `at`, all clipped to the canvas. `platform` shows it with one SDL texture per frame. Its tests (`sosuc test packages/render`) need no screen.
   - Text: `render.font` is a built-in 8 by 8 font for printable ASCII (font8x8, public domain); other characters are drawn as a box. `surface.text(x, y, text, color, scale)` draws each pixel of the font as a `scale` by `scale` square, `\n` starts a new line (`font.line_height()`: 10 pixels of the font), and `canvas.text_size(text, scale)` gives the size of the text. It needs no file, and stays the font of last resort.
   - TrueType fonts: `render.truetype` reads `.ttf` files and draws their glyphs in Sosu, antialiased, at any size. `truetype.load(arena, path)` gives a `Font*` (or `Unreadable`, `NotTrueType`); `surface.text_with(font, size, x, y, text, color)` draws text whose first line has its top at y, `size` being in pixels per em; `font.measure(text, size)`, `font.line_height(size)` and `font.ascent(size)` measure it. All of Unicode that the font covers is drawn (accented letters are composite glyphs); a character it does not have gets its missing glyph.
     - Reading: the tables `head`, `hhea`, `maxp`, `hmtx`, `loca`, `glyf` and `cmap` (formats 4 and 12), through checked reads: a damaged file gives wrong glyphs, never a read outside of its bytes. Drawing: the outlines (lines and quadratic curves, composite glyphs moved and scaled) accumulate the area they cover under each pixel; the image of a glyph is kept per size (256 images at most, then forgotten), and `close()` frees them.
     - Not yet: hinting (the instructions of a font are ignored), kerning, CFF outlines (`.otf`), color fonts, fallback to another font for missing characters.
     - `packages/render/fonts/NotoSans-Regular.ttf` (Noto Sans, SIL Open Font License, with `OFL.txt`) is the font of the examples and of the tests, run from the root of the repository. `ui.font` (a `widgets.Ui` field) gives it to the widgets: 18 pixels per em for labels, 27 for titles; without it, they use the 8 by 8 font.
2. The same program on the Android emulator, then on the iOS simulator.
3. A window described in `.ui.sosu` (title, text field, button) with the same interface on every target. Done on the desktop: `examples/form` describes its window in `screen.ui.sosu` (§10.2), and `tests/programs/screens` drives such a window with simulated input, without a screen.
   - The `ui` package: without lambdas (v1), a widget cannot call the program back, so `ui` works in immediate mode, which matches the re-evaluation of §10.2. At each frame, the program describes its interface from its state, `if (f.button("OK")) { ... }`; a `widgets.Ui` keeps what lasts between frames (the input received, the widget pressed, the text field that has the keyboard), and a `widgets.Frame` places the widgets of the frame in nested columns and rows (`column(gap, padding)`, `row_with(gap, padding)`, `end()`; `row()` and `end_row()` for a row with the default gap).
   - Widgets: `label`, `title`, `button` and `button_enabled` (true when clicked), `checkbox(text, bool*)` and `checkbox_alone(bool*)`, `text_field(TextBuffer*, width)` (a click gives it the keyboard; true on Enter), `space`. `TextBuffer` holds up to 256 bytes of UTF-8. `widgets.Window` holds what a window of a `.ui.sosu` file says about itself.
   - `ui` does not depend on `platform`: the program passes it the input (`mouse_move`, `mouse_button`, `type_text`, `key`), so its tests run without SDL. The translator of `.ui.sosu` generates the calls to the widgets.

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

- The Sosu program is compiled into a library (`.so` on Android, static library on iOS), loaded by a small host project (Android activity, Xcode project). SDL3 provides this host code; in v0, project templates are provided by hand, and `sosuc package --target android|ios` (v1) generates them.
- Sosu compiles ahead of time (AOT): no problem with the ban on run-time generated code on iOS.
- Files: always through `std.app` (sandbox), never hard-coded paths.
- Permissions and native APIs (camera, notifications, purchases...): dedicated packages, with host code (JNI on Android, Objective-C on iOS); outside v0.
- Zero dependencies applies to the compiler, not to mobile: the NDK, Xcode, signing and the stores remain mandatory. Even the future custom backend (step 2) will produce ARM64, but linking, signing and packaging will go through the official tools. The C backend is therefore the realistic path for mobile, and deserves to be kept as a permanent option.

### 11.3 Backends

1. Step 1: `sosuc` generates C99, compiled by `cc` (or by the target platform's compiler). The generated C contains `#line` directives, so that errors and the debugger point to the lines of the `.sosu` files.
2. Step 2: custom x86-64 / ARM64 backend (ELF, Mach-O, PE) on the same IR.
3. Step 3: self-hosting (Sosu compiles Sosu).

### 11.4 Compiler as a library, and commands

The compiler is designed as a library (`libsosu`: lexer, parser, type checking, IR) shared by all the tools, including a future language server (LSP) for completion in editors.

- `sosuc build [--target <os>-<arch>]`: compiles (cross-compiling depending on the available tooling). The generated C file and the program go to the `build/` folder of the package. The C compiler is MSVC on Windows (found through `vswhere`), `cc` elsewhere, or the one named by the `SOSU_CC` variable (gcc or clang style). `--target` is not implemented yet.
- `sosuc run`: compiles and runs in one command (useful for learning and workshops).
- `sosuc test`: compiles and runs the `test` blocks of the package, prints one line per test and a summary; the exit code is 1 if a test failed.
- `sosuc check`: checks a package without compiling it.
- `sosuc fmt [--check] path...`: formats files, or the modules of packages (not their dependencies), in place (§7.6). With `--check`, it changes nothing and fails if a file is not formatted (for the CI).
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
style       = "style" IDENT "{" prop { "," prop } "}" ;
theme       = "theme" IDENT "{" { prop | style } "}" ;
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
9. Mobile: Android (NDK, emulator), then iOS (simulator), with the same code
10. Generics (monomorphized), `Vec` / `HashMap` containers
11. `sosuc bindgen`, `sosuc package`, cross-compilation
12. Custom native backend (x86-64 / ARM64; ELF, Mach-O, PE)
13. Research: `comptime`, effect polymorphism, regions, concurrency, language server (LSP)
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
- Program entry: callbacks provided to the `platform` runtime, which owns the main loop.
- `.ui.sosu` syntax: only in dedicated files in v0.
- Interface hot reload: after v0.
- Manifest format: a subset of TOML (`sosu.toml`, §8.1), implemented.
- Source of dependencies in v0: local path or Git repository, without a central registry. Local paths are implemented; Git sources are parsed but not fetched yet.
- A `struct` cannot implement an interface in v0 (reserved for classes); the parser rejects `struct S : I`.
- Imported names are qualified by the module name: `vector.Vec2`, `io.print(...)` (§8.1).
- C libraries are declared in `sosu.toml` only ([link] sections, §9.1), not in the source code.
- Lexical details settled by the implementation: §3.1 (literals), §5.1 (`as`, ranges, struct literals, function types), §12 (grammar).
- Array literals `[a, b]` and `[value; n]`; `&x` requires `mut x`; `result` is contextual (only in `ensures`) (§5.3).
- Moves: `Arena` and the values containing one are moved, checked in the order of the source; `.copy()` duplicates them with new, empty arenas (§6).

### 14.2 Open questions

- Effect polymorphism (higher-order functions): to be handled with generics in v1? (proposed: yes)
- Abstract classes: unnecessary, interfaces are enough? (proposed: yes)
- Class destructors: none in v0, external resources are released with `defer`? (proposed: yes)
- Uninitialized variables: the grammar requires a value at declaration, while §7.7 mentions checking "variables initialized before being read" in debug. (proposed: keep the value mandatory; the debug check then concerns memory from `alloc`)
- Multiple effects in a function type (`fn() -> void uses io, time`) rely on the list of predefined effects to tell them from the next parameter. User-defined effects would need another syntax. (proposed: no user-defined effects in v0)
