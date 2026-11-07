# Sosu: A Quick Guide

From a first program to a window, in an evening. Written for Sosu 0.4.

## Preface

Sosu is a native, low-level language built around one idea: you should know what a function does by reading its signature. Its effects, its failures and its allocations are all written there, and the compiler checks them.

This guide is for programmers who know C, C++ or C#. It does not explain what a pointer or a loop is. It shows how Sosu does things, in the order you will need them, and it stops at what most programs use. For the rest, there are the [specification](SPEC.md), the [library reference](REFERENCE.md) and the [list of errors](errors.md).

**How to read it.** Each chapter is short and can be read alone once you know the first seven. Type the examples: every program here is compiled and run by the tests of the repository, so what you read is what you get.

**Conventions.** Code is in `monospace`. A block after a program shows what it prints. Three kinds of boxes appear in the text:

> **Tip:** a shorter or better way to do something.

> **Note:** something worth knowing, but that you can skip at first.

> **Warning:** a place where a program can go wrong without the compiler saying so.

Contents:

1. [A First Program](#1-a-first-program)
2. [Values and Types](#2-values-and-types)
3. [Control Flow](#3-control-flow)
4. [Functions and Effects](#4-functions-and-effects)
5. [Structs, Enums and `match`](#5-structs-enums-and-match)
6. [Absence and Failure](#6-absence-and-failure)
7. [Memory](#7-memory)
8. [Classes and Interfaces](#8-classes-and-interfaces)
9. [Generics and Lambdas](#9-generics-and-lambdas)
10. [The Standard Library](#10-the-standard-library)
11. [Packages and Tests](#11-packages-and-tests)
12. [Threads and Channels](#12-threads-and-channels)
13. [A Window](#13-a-window)
14. [Talking to C](#14-talking-to-c)
15. [Quick Reference](#15-quick-reference)

## 1. A First Program

### Installing

The [releases](https://github.com/Clement-Bonet/Sosu/releases) hold `sosuc` for Windows, Linux and macOS, with its library. Unpack the archive and put its `bin/` folder in your `PATH`.

`sosuc` writes C, so it needs a C compiler to finish the job: MSVC on Windows, `cc` (gcc or clang) elsewhere.

```bash
sosuc --version
```

### Hello

A program is a *package*: a folder with a `sosu.toml` file and its sources.

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

And `src/main.sosu`:

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

Run it from the `hello` folder:

```bash
sosuc run
```

Look at this signature: `uses io` says that `main` touches the outside world. Remove it and the program no longer compiles. That is Sosu in one line.

`io.print` ends the line, and replaces each `{}` with a value. The compiler checks that there are as many values as `{}`.

### The commands you will use

| Command | What it does |
|---|---|
| `sosuc check` | checks the package, without compiling it: the fastest |
| `sosuc run` | compiles and runs it |
| `sosuc build` | compiles it into `build/` |
| `sosuc test` | runs its `test` blocks |
| `sosuc fmt` | formats the files in place |
| `sosuc package` | makes a release of a program, with its archive |

`sosuc run --release` gives an optimized program.

> **Tip:** `sosuc lsp` is a language server. It gives your editor the errors, hover, go to definition and completion. It reads what is there and fetches nothing, so run `sosuc check` once in a terminal after cloning a project with git dependencies.

## 2. Values and Types

### Variables

The type comes first, as in C. Every variable gets its value where it is declared, and it cannot change unless it is `mut`.

<!-- run -->
```csharp
import std.io;

i32 main() uses io {
    i32 year = 2026;
    mut i32 count = 0;
    count += 1;
    auto ratio = 0.5;  // `auto`: the type of the value, here f64
    io.print("{} {} {}", year, count, ratio);
    return 0;
}
```

```text
2026 1 0.5
```

There are no global variables. A module can have constants:

<!-- check -->
```csharp
pub i32 MAX_PLAYERS = 4;
str GREETING = "Welcome";
```

### Numbers

| Kind | Types |
|---|---|
| Integers | `i8 i16 i32 i64`, `ui8 ui16 ui32 ui64`, `isize usize` |
| Floats | `f32 f64`, and `uf32 uf64`, which are never negative |
| Others | `bool`, `char` (a Unicode code point) |

A literal takes the type that is expected. Alone, an integer is an `i32` and a float an `f64`. `3` is never a float: write `3.0`.

Nothing converts by itself. Operands have the same type, and `as` converts:

<!-- run -->
```csharp
import std.io;

i32 main() uses io {
    i32 items = 7;
    i64 total = 1000;
    f64 average = total as f64 / items as f64;
    io.print("{}", total + items as i64);
    io.print("{}", average > 142.0);
    return 0;
}
```

```text
1007
true
```

An integer overflow stops the program in a debug build, with its position. In a release build it wraps around. A division by zero always stops it. Use `wrapping_add`, `wrapping_sub` and `wrapping_mul` when wrapping is what you want.

### Text

A `str` is UTF-8 text: a pointer and a length in bytes. It does not own its bytes.

<!-- run -->
```csharp
import std.io;

i32 main() uses io {
    str word = "café";
    io.print("{} bytes", word.len);
    mut i32 letters = 0;
    for (char c in word.chars()) {
        letters += 1;
    }
    io.print("{} letters", letters);
    return 0;
}
```

```text
5 bytes
4 letters
```

### Arrays and views

`T[N]` is an array of fixed size, stored in place. `T[]` is a *view*: a pointer and a length. `a[x..y]` gives the view of the elements `x` to `y - 1`. Indexes are checked, in release builds too.

<!-- run -->
```csharp
import std.io;

i32 sum(i32[] values) {
    mut i32 total = 0;
    for (i32 v in values) {
        total += v;
    }
    return total;
}

i32 main() uses io {
    mut i32[4] scores = [3, 9, 4, 1];
    scores[3] = 2;
    io.print("{} {}", sum(scores[0..4]), sum(scores[1..3]));
    i32[3] zeros = [0; 3];
    io.print("{}", zeros.len);
    return 0;
}
```

```text
18 13
3
```

### Pointers

`T*` is a pointer, and it is never null. `&x` takes an address and `*p` follows it. A field or a method is always reached with `.`, through a pointer too. There is no `->`.

<!-- run -->
```csharp
import std.io;

struct Point {
    mut i32 x;
    mut i32 y;
}

void move_right(Point* p) {
    p.x += 1;
}

i32 main() uses io {
    mut Point p = Point{ x: 1, y: 2 };
    move_right(&p);
    io.print("{} {}", p.x, p.y);
    return 0;
}
```

```text
2 2
```

## 3. Control Flow

Braces are mandatory, and conditions are in parentheses. `x++` does not exist: write `x += 1`.

<!-- run -->
```csharp
import std.io;

i32 main() uses io {
    for (i32 i in 0..3) {
        io.print("{}", i);
    }
    for (mut i32 i = 10; i > 8; i -= 1) {
        io.print("{}", i);
    }
    mut i32 n = 3;
    while (n > 0) {
        n -= 1;
        if (n == 1) {
            continue;
        }
        io.print("n = {}", n);
    }
    str size = n == 0 ? "empty" : "some";
    io.print("{}", size);
    return 0;
}
```

```text
0
1
2
10
9
n = 2
n = 0
empty
```

`0..3` stops before `3`, and `0..=3` includes it. `match` replaces `switch`, and has its own chapter.

## 4. Functions and Effects

### Functions

<!-- run -->
```csharp
import std.io;

i32 add(i32 a, i32 b) {
    return a + b;
}

void bump(mut i32 x) {
    x += 1;  // a parameter changes only if it is `mut`, and only here
}

i32 main() uses io {
    io.print("{}", add(2, 3));
    return 0;
}
```

```text
5
```

There is no overloading: two functions of a module have two names.

### Effects

A function that reaches the outside world says so with `uses`:

| Effect | Covers |
|---|---|
| `io` | console, files, network, windows |
| `time` | reading the clock |
| `random` | random generators |
| `threads` | threads, channels, locks |

A function calls only what its own effects allow, and effects are never inferred. If a deep function starts to print, every caller up to `main` says it. That is the point: a function without `uses` cannot print, read a file or ask for the time.

<!-- error E0340 -->
```csharp
import std.io;

void greet(str name) {
    io.print("hello, {}", name);
}
```

The compiler refuses this function, and says which effect is missing. Here is the correct version:

<!-- check -->
```csharp
import std.io;

void greet(str name) uses io {
    io.print("hello, {}", name);
}
```

### Contracts

`requires` and `ensures` state what a function expects and gives. `result` names the value returned.

<!-- check -->
```csharp
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
```

They are checked in debug builds. `assert(condition)` is always checked.

## 5. Structs, Enums and `match`

### Structs

A `struct` is a value: it is copied when assigned or passed. A field changes only if it is `mut`, and only through something that is `mut` too.

<!-- run -->
```csharp
import std.io;

struct Rect {
    f64 width;
    f64 height;

    f64 area() {
        return width * height;  // the fields of `self`, without `self.`
    }

    Rect scaled(f64 by) {
        return Rect{ width: width * by, height: height * by };
    }
}

i32 main() uses io {
    Rect r = Rect{ width: 2.0, height: 3.0 };
    io.print("{}", r.scaled(2.0).area());
    return 0;
}
```

```text
24.0
```

### Enums and `match`

An `enum` can carry data. `match` is an expression, and it must cover every case: forget one and the program does not compile. `_` is the default case.

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
        Circle(r) => 3.14 * r * r,
        Rect(w, h) => w * h,
        Empty => 0.0,
    };
}

i32 main() uses io {
    Shape s = Rect(2.0, 5.0);
    io.print("{}", area(s));
    io.print("{}", area(Shape.Empty));
    return 0;
}
```

```text
10.0
0.0
```

The name of the enum can be left out where the type is known, as in `Shape s = Rect(2.0, 5.0);`.

## 6. Absence and Failure

Sosu has no null and no exceptions. What can be absent is `T?`, and what can fail is `T!E`. Both are in the signature, so you see them without opening the function.

### `T?`: a value or nothing

<!-- run -->
```csharp
import std.io;

i32? first_even(i32[] values) {
    for (i32 v in values) {
        if (v % 2 == 0) {
            return some(v);
        }
    }
    return none;
}

i32 main() uses io {
    mut i32[3] numbers = [3, 8, 5];
    match (first_even(numbers[0..3])) {
        some(n) => io.print("found {}", n),
        none => io.print("nothing"),
    }
    return 0;
}
```

```text
found 8
```

### `T!E`: a value or an error

`E` is an ordinary enum. `try` gives the value, or returns the error to the caller: the early return is written in the code.

<!-- run -->
```csharp
import std.io;
import std.text;

enum AgeError {
    NotANumber,
    TooOld,
}

i32!AgeError parse_age(str s) {
    return match (text.parse_i64(s)) {
        ok(n) => n > 150 ? err(AgeError.TooOld) : ok(n as i32),
        err(e) => err(AgeError.NotANumber),
    };
}

i32!AgeError next_age(str s) {
    i32 age = try parse_age(s);
    return ok(age + 1);
}

void show(i32!AgeError result) uses io {
    match (result) {
        ok(age) => io.print("next year: {}", age),
        err(e) => io.print("{}", e == AgeError.TooOld ? "too old" : "not a number"),
    }
}

i32 main() uses io {
    show(next_age("41"));
    show(next_age("forty"));
    show(next_age("200"));
    return 0;
}
```

```text
next year: 42
not a number
too old
```

A function that returns nothing but can fail returns `void!E`, and ends with `return ok();`:

<!-- check -->
```csharp
enum SaveError {
    Full,
}

void!SaveError save(i32 size) {
    if (size > 100) {
        return err(SaveError.Full);
    }
    return ok();
}
```

The result of a call that can fail must be used: you cannot ignore it by accident.

## 7. Memory

This is the chapter where Sosu differs most from what you know. There is no garbage collector, no reference counting and no global allocator. Memory comes from *arenas*.

### Arenas

An arena is a region of memory. You allocate in it, and you free it all at once.

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

    Point* p = arena.alloc(Point);  // zeroed memory
    p.x = 3;
    i32[] numbers = arena.alloc_slice(i32, 100);
    numbers[99] = p.x;
    io.print("{}", numbers[99]);
    return 0;
}
```

```text
3
```

`defer` runs a statement when the block ends, and it is how arenas are freed. `Arena.new` takes the size of its blocks: an arena grows when it needs to.

`alloc` gives zeroed memory, so the type cannot hold a pointer, which is never null. To build such a value, use a class (chapter 8) or `T*?`.

### Allocation is in the signature

A function that allocates receives an `Arena*`. So you can see it from the call, and the caller decides how long the memory lives.

<!-- run -->
```csharp
import std.io;
import std.text;

str shout(Arena* arena, str s) {
    return text.to_upper(arena, s);
}

i32 main() uses io {
    mut Arena arena = Arena.new(1 * KB);
    defer arena.free();
    io.print("{}", shout(&arena, "hello"));
    return 0;
}
```

```text
HELLO
```

> **Tip:** a good pattern is one arena per job: per request, per frame, per file read. Free it when the job ends, and forget about individual objects.

### An arena has one owner

An `Arena` is moved, not copied. After a move, the old variable cannot be used. To lend an arena, pass a pointer.

<!-- error E0356 -->
```csharp
i32 f() {
    mut Arena a = Arena.new(64);
    mut Arena b = a;
    a.free();
    b.free();
    return 0;
}
```

### What the compiler refuses

Sosu has no lifetimes in its types. The compiler follows where the memory of each arena goes, inside each function and through the summaries of the functions it calls, and it refuses a pointer that cannot be valid.

Using memory after its arena was freed (E0372):

<!-- error E0372 -->
```csharp
i32 f() {
    mut Arena a = Arena.new(64);
    i32* p = a.alloc(i32);
    a.free();
    return *p;
}
```

Returning memory of an arena that ends with the function (E0373):

<!-- error E0373 -->
```csharp
i32* f() {
    mut Arena a = Arena.new(64);
    defer a.free();
    return a.alloc(i32);
}
```

The usual fix is to receive the arena from the caller:

<!-- check -->
```csharp
i32* f(Arena* arena) {
    return arena.alloc(i32);
}
```

### `keeps`: what a function holds on to

Most parameters are only read during the call and need nothing. A parameter that stays beyond the call is marked `keeps`, so that the caller knows:

<!-- check -->
```csharp
class Tag {
    mut str name;

    void init(keeps str name) {
        self.name = name;
    }
}
```

Without `keeps`, this is refused (E0376). With it, a call that gives memory ending too soon is refused (E0375):

<!-- error E0375 -->
```csharp
struct Holder {
    mut i32*? kept;
}

void hold(Holder* h, keeps i32* value) {
    h.kept = some(value);
}

void f(Holder* h) {
    mut Arena a = Arena.new(64);
    defer a.free();
    hold(h, a.alloc(i32));
}
```

`keeps` covers what the parameter reaches, too: a function that stores `q.next` somewhere marks `q`. An `Arena*` is marked when memory allocated in it goes elsewhere. This is the form you will write most, since a constructor keeps its arena:

<!-- check -->
```csharp
class Log {
    Arena* arena;

    void init(keeps Arena* arena) {
        self.arena = arena;
    }
}
```

A method marks `self` after its parameters, with `keeps self`, when what `self` reaches goes elsewhere. A function type says it too: `fn(keeps Node*) -> void`.

### An arena in a field

A field given memory of an arena of the same object belongs to that arena. If a function frees the arena and makes a new one, it gives the field a new value too, or E0378 tells you:

<!-- check -->
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
        items = arena.alloc_slice(i32, 16);
    }
}
```

### What the compiler does not prove

> **Warning:** Sosu is not memory safe, by choice. Closing the last cases would take lifetimes in the types, or a check at run time in release builds. A program can have undefined behaviour without `unsafe`.

The main cases that pass:

- Two fields of one object, in two arenas: `a.p = a.q` is accepted, although `q` may be in the arena that ends first.
- A field of *another* object, kept behind a flag of your program.
- `unsafe` code, and whatever C does.

The full list is in [section 7.7 of the specification](SPEC.md). So test in debug: a debug build stops on a pointer used after the `free()` of its arena.

## 8. Classes and Interfaces

A `class` lives in an arena and is used through a pointer, never copied. `arena.new(T, ...)` allocates it and calls its `init`. A field changes in `init`, and elsewhere if it is `mut`.

Methods are not virtual unless they say so. `override` is mandatory, and it is an error if there is nothing to override. So the cost of a virtual call is visible.

<!-- run -->
```csharp
import std.io;

interface Named {
    str name();
}

class Animal : Named {
    i32 legs;

    void init(i32 legs) {
        self.legs = legs;
    }

    virtual str sound() {
        return "...";
    }

    override str name() {
        return "animal";
    }
}

class Dog : Animal {
    void init() {
        base.init(4);
    }

    override str sound() {
        return "woof";
    }
}

void describe(Animal* a) uses io {
    io.print("{} legs: {}", a.legs, a.sound());
}

i32 main() uses io {
    mut Arena arena = Arena.new(1 * KB);
    defer arena.free();
    Dog* rex = arena.new(Dog);
    Animal* a = rex as Animal*;
    describe(a);
    Named* n = a as Named*;
    io.print("{}", n.name());
    match (a as? Dog*) {
        some(d) => io.print("a dog"),
        none => io.print("something else"),
    }
    return 0;
}
```

```text
4 legs: woof
animal
a dog
```

A class has one parent and any number of interfaces: `class Dog : Animal, Walker`, with the parent first. There are no destructors: memory goes with the arena, and other resources are released with `defer`.

## 9. Generics and Lambdas

### Generics

Type parameters follow the name, and are inferred at the call. A constraint says what the code may do with them: `Ordered`, `Equal`, `Hash`, `Number`, `Integer`, `Send`.

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

struct Pair<A, B> {
    A first;
    B second;
}

i32 main() uses io {
    mut i32[3] scores = [3, 9, 4];
    io.print("{}", largest(scores[0..3]));
    Pair<str, i32> entry = Pair{ first: "age", second: 41 };
    io.print("{} {}", entry.first, entry.second);
    return 0;
}
```

```text
9
age 41
```

### Lambdas

`(parameters) => body` is a function value. A parameter can leave out its type when the expected type gives it. A lambda captures by copy; to change a variable, capture a pointer to it.

<!-- run -->
```csharp
import std.io;

i32 twice(fn(i32) -> i32 f, i32 x) {
    return f(f(x));
}

void each<T, uses E>(T[] items, fn(T) -> void uses E f) uses E {
    for (T x in items) {
        f(x);
    }
}

i32 main() uses io {
    i32 bonus = 10;
    io.print("{}", twice((x) => x + bonus, 1));
    mut i32[2] values = [1, 2];
    each(values[0..2], (v) => io.print("{}", v));
    mut i32 total = 0;
    i32* sum = &total;
    each(values[0..2], (v) => {
        *sum += v;
    });
    io.print("{}", total);
    return 0;
}
```

```text
21
1
2
3
```

`uses E` is an effect parameter: `each` has the effects of the function it receives. The first call uses `io`, the second nothing.

> **Note:** a closure cannot leave the function that made it, since what it captures would be gone (E0370). To keep one, make it in an arena: `arena.closure((x) => x + bonus)`.

A parameter of a lambda that stays beyond the call is marked as in a function: `(keeps Node* n) => list.push(n)`.

## 10. The Standard Library

The library is plain Sosu: `lib/std`, imported by module. Names keep their module: after `import std.text;`, you write `text.trim(s)`. The [reference](REFERENCE.md) lists it all.

### Vectors and maps

`vec.Vec<T>` and `map.HashMap<K, V>` are classes that grow in the arena they are given.

<!-- run -->
```csharp
import std.io;
import std.map;
import std.vec;

i32 main() uses io {
    mut Arena arena = Arena.new(4 * KB);
    defer arena.free();

    vec.Vec<i32>* numbers = vec.new(&arena);
    numbers.push(1);
    numbers.push(2);
    for (i32 n in numbers.items()) {
        io.print("{}", n);
    }

    map.HashMap<str, i32>* ages = map.new(&arena);
    ages.put("Ada", 36);
    match (ages.get("Ada")) {
        some(age) => io.print("Ada: {}", age),
        none => io.print("unknown"),
    }
    io.print("{}", ages.has("Bob"));
    return 0;
}
```

```text
1
2
Ada: 36
false
```

### Text

<!-- run -->
```csharp
import std.io;
import std.text;

i32 main() uses io {
    mut Arena arena = Arena.new(4 * KB);
    defer arena.free();
    str line = "  red, green, blue  ";
    str[] parts = text.split(&arena, text.trim(line), ", ");
    io.print("{}", parts.len);
    io.print("{}", text.join(&arena, parts, "/"));
    io.print("{}", text.contains(line, "green"));
    return 0;
}
```

```text
3
red/green/blue
true
```

### Files

Reading a file allocates, and can fail. The signature says both, and `uses io` says it touches the disk:

<!-- check -->
```csharp
import std.io;

void show(Arena* arena, str path) uses io {
    match (io.read_file(arena, path)) {
        ok(content) => io.print("{}", content),
        err(e) => io.print("cannot read {}", path),
    }
}
```

The other modules of `std`: `math`, `sort`, `time`, `os`, `fs`, `cli`, `log`, `json`, `thread`, `channel`. Beyond `std`, the packages of `packages/` give `image`, `audio`, `db` and `sqlite`, `crypto`, `compress`, `math.linear`, `i18n` and `testing`.

## 11. Packages and Tests

### Modules

Each file of `src/` is a module, and `pub` exports what others may use. Everything else is private.

```text
geometry/
  sosu.toml
  src/
    main.sosu
    shapes.sosu
```

```csharp
// src/shapes.sosu
pub f64 square(f64 side) {
    return side * side;
}
```

```csharp
// src/main.sosu
import std.io;
import geometry.shapes;

i32 main() uses io {
    io.print("{}", shapes.square(3.0));
    return 0;
}
```

An import starts with the name of the package, and it is used through its last name.

### Dependencies

A package with `src/lib.sosu` is a library. Dependencies come from a folder or from git, with a tag or a commit:

```toml
[dependencies]
shared = { path = "../shared" }
ui = { git = "https://git.example/sosu-ui", tag = "v0.1.0" }
```

`sosu.lock` pins the commit of each git dependency: keep it in version control.

> **Warning:** a dependency runs on your machine when you build it, since it can compile C files of its own. Add one only from a source you trust.

### Tests

A `test` block sits next to the code, and `sosuc test` runs them all. A failed `assert` on a comparison shows both sides.

<!-- test -->
```csharp
i32 square(i32 x) {
    return x * x;
}

test "a square" {
    assert(square(3) == 9);
    assert(square(-2) == 4);
}
```

The package `testing` adds comparisons of views, benchmarks with `sosuc test --bench`, data files and golden files.

## 12. Threads and Channels

Threads share no memory. A thread runs a function on a copy of a value, with an arena of its own, and channels carry copies between threads. Only values without pointers cross (the constraint `Send`): the compiler refuses the rest.

<!-- run -->
```csharp
import std.io;
import std.thread;
import std.channel;

struct Job {
    i64 from;
    i64 to;
    channel.Channel<i64> results;
}

i64 sum(Arena* arena, Job job) uses threads {
    mut i64 total = 0;
    for (i64 i in job.from..job.to) {
        total += i;
    }
    bool sent = job.results.send(total);
    return total;
}

i32 main() uses io, threads {
    mut Arena arena = Arena.new(16 * KB);
    defer arena.free();
    channel.Channel<i64> results = channel.new<i64>(4);
    auto a = thread.spawn(&arena, sum, Job{ from: 0, to: 100, results: results });
    auto b = thread.spawn(&arena, sum, Job{ from: 100, to: 200, results: results });
    io.print("{}", a.join() + b.join());
    results.free();
    return 0;
}
```

```text
19900
```

`thread.spawn` needs a function or a lambda without captures. `join` waits, and gives what the function returned.

> **Note:** interfaces stay on the main thread. A worker gives its results to the interface through a channel.

## 13. A Window

Windows come from three packages written in Sosu: `platform` (the window and its events, on SDL3), `render` (pixels, shapes, text) and `ui` (the widgets). They are in the `packages/` folder of the release.

On Windows, download SDL3 once:

```bash
powershell -ExecutionPolicy Bypass -File packages/platform/fetch-sdl3.ps1
```

On Linux and macOS, install SDL3 with your package manager.

A counter takes four files:

```text
counter/
  sosu.toml
  src/
    main.sosu
    state.sosu
    screen.ui.sosu
```

The manifest names the three packages. Adjust the paths to where you unpacked Sosu:

```toml
[package]
name = "counter"
version = "0.1.0"

[dependencies]
platform = { path = "../sosu/packages/platform" }
render = { path = "../sosu/packages/render" }
ui = { path = "../sosu/packages/ui" }
```

`src/state.sosu` holds the state of the program, and what its button does:

```csharp
pub class Counter {
    pub mut i32 count;

    void init() {
        count = 0;
    }

    pub void add() {
        count += 1;
    }
}
```

`src/screen.ui.sosu` describes the window. A `.ui.sosu` file is compiled with the program, and a change in it is checked like any other code. `plural:` and `count:` choose the form of a text, and fill `{count}`.

```csharp
import counter.state;

window Main(state.Counter* c) {
    title: "Counter"
    width: 320
    height: 200

    Column(gap: 12, padding: 24) {
        Title("Counter")
        Text("Clicked {count} time", plural: "Clicked {count} times", count: c.count)
        Button("Add one", on_click: c.add)
    }
}
```

`src/main.sosu` opens the window, gives its events to the widgets, and draws a frame when needed:

```csharp
import std.io;
import platform.app;
import render.canvas;
import ui.widgets;
import counter.screen;
import counter.state;

class App : app.Handler {
    mut widgets.Ui ui;
    state.Counter* counter;

    void init(keeps state.Counter* counter) {
        ui = widgets.Ui{ };
        self.counter = counter;
    }

    override bool on_event(app.Event e) uses io {
        match (e) {
            Quit => {
                return false;
            },
            MouseMove(x, y) => {
                ui.mouse_move(x, y);
            },
            Click(x, y, button) => {
                ui.mouse_move(x, y);
                ui.mouse_button(true);
            },
            Release(x, y, button) => {
                ui.mouse_move(x, y);
                ui.mouse_button(false);
            },
            _ => { },
        }
        return true;
    }

    override void on_frame(canvas.Canvas* surface) uses io {
        surface.clear(canvas.Color{ r: 30, g: 30, b: 46 });
        mut widgets.Frame f = widgets.begin(&ui, surface, 0.0, 0.0);
        screen.Main(&f, counter);
        f.finish();
    }
}

i32 main() uses io {
    mut Arena arena = Arena.new(4 * KB);
    defer arena.free();
    state.Counter* counter = arena.new(state.Counter);
    App* handler = arena.new(App, counter);
    widgets.Window window = screen.MainConfig();
    return app.run(app.Config{ title: window.title, width: window.width, height: window.height }, handler as app.Handler*);
}
```

Run it with `sosuc run`. The widgets are *immediate*: `on_frame` describes the whole interface at each frame, from the state, so there is nothing to keep in sync.

> **Tip:** a window parameter whose memory a widget binds, as `TextField(bind: form.name)` or `List(selected: form.chosen)`, is marked `keeps`: `window Main(keeps state.Form* form)`.

From here, `examples/form` adds text fields, and `examples/themes` adds themes. The widgets are described in [section 10 of the specification](SPEC.md).

## 14. Talking to C

Sosu calls C directly. Declare the functions in an `extern "C"` block. A call is `unsafe`, because the compiler cannot check C. Wrap it in a safe function, and the rest of the program does not need to know.

<!-- run -->
```csharp
import std.io;

extern "C" {
    f64 sqrt(f64 x);
}

f64 root(f64 x) requires x >= 0.0 {
    mut f64 value = 0.0;
    unsafe {
        value = sqrt(x);
    }
    return value;
}

i32 main() uses io {
    io.print("{}", root(16.0));
    return 0;
}
```

```text
4.0
```

`sosuc bindgen header.h -o file.sosu` writes these declarations from a C header. The `[link]` section of `sosu.toml` names the libraries, and C files of a package are compiled with it:

```toml
[link]
libs = ["sqlite3"]
c = ["vendor/thing.c"]
```

`unsafe { }` is the only place for pointer arithmetic, reinterpreting a pointer, and calling C. It turns the memory rules of chapter 7 off, so keep these blocks small.

## 15. Quick Reference

### Syntax

```text
i32 x = 1;                 constant variable
mut i32 x = 1;             changing variable
auto x = 1;                type of the value
i32 add(i32 a, i32 b)      function
void log(str s) uses io    function with an effect
i32!E f()                  can fail: ok(v), err(e), try
i32? f()                   can be absent: some(v), none
void f(keeps T* p)         p stays beyond the call
void m() keeps self        what self reaches stays beyond the call
struct P { i32 x; }        value, copied
enum E { A, B(i32 n) }     sum type
class C : Base, Iface { }  in an arena, through a pointer
interface I { void m(); }  signatures
T* p = &x;                 pointer, never null
T[N]  T[]  x[a..b]         array, view, part of one
x as f32                   conversion
(a) => a + 1               lambda
fn(i32) -> i32 uses io     function type
T f<T: Ordered>(T a)       generic
```

### Memory

```text
mut Arena a = Arena.new(1 * MB);    make an arena
defer a.free();                     free it at the end of the block
a.alloc(T)                          zeroed T, no pointers inside
a.alloc_slice(T, n)                 zeroed view of n elements
a.new(C, args)                      a class, through its init
```

### Errors you will meet first

| Code | Means | Usually |
|---|---|---|
| E0331 | the type is not the expected one | add `as`, or fix the type |
| E0340 | an effect is missing | add it to `uses` |
| E0356 | a value was moved | pass a pointer instead |
| E0372 | memory used after its `free()` | keep the arena alive, or give a new value |
| E0373 | returned memory ends with the function | receive an `Arena*` |
| E0374 | stored where it may outlive what it points to | use one arena |
| E0375 | given to `keeps` with memory that ends sooner | use memory that lives as long |
| E0376 | stored beyond the call without `keeps` | write `keeps` |
| E0378 | a field left in the old arena | give it a value after the new arena |

All of them are in [errors.md](errors.md).

### From C or C#

| You write | In Sosu |
|---|---|
| `p->x` | `p.x` |
| `x++` | `x += 1` |
| `NULL`, `null` | `T?` and `none` |
| `try / catch`, `errno` | `T!E`, `try`, `match` |
| `malloc / free`, `new`, GC | an arena |
| `switch` | `match`, exhaustive |
| `(float)x` | `x as f32` |
| `static`, globals | constants, and values passed down |
| overloads | two names |
