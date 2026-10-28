# Error codes

Codes are stable (spec §7.5). Ranges: `E01xx` lexer, `E02xx` parser, `E03xx` semantic analysis,
`E04xx` packages and imports.

## Lexer

| Code | Message | Example |
|---|---|---|
| E0100 | invalid UTF-8 byte sequence | file saved as Latin-1 |
| E0101 | unexpected character | `#include`, `café` outside a string |
| E0102 | unterminated block comment | `/* ...` without `*/` |
| E0103 | unterminated string literal | `"abc` at the end of a line |
| E0104 | unterminated character literal | `'a` |
| E0105 | empty character literal | `''` |
| E0106 | character literal contains more than one character | `'ab'` |
| E0107 | unknown or invalid escape sequence | `"\q"`, `"\xFF"` |
| E0108 | invalid number literal (digits or suffix) | `0b102`, `12abc`, `0x`, `300ui8` |
| E0109 | integer literal is too large | above 2^64 - 1 |
| E0110 | `++` / `--` does not exist | `x++` |
| E0111 | `::` does not exist | `std::io` |
| E0112 | reserved keyword | `comptime` |
| E0113 | invalid unicode escape | `"\u{D800}"`, `"\u41"` |
| E0114 | leading zeros are not allowed | `007` |

## Parser

| Code | Message | Example |
|---|---|---|
| E0200 | expected X, found Y | `x = 1 }` (missing semicolon) |
| E0201 | expected an expression | `x = ;`, `auto x = if (a) {...}` |
| E0202 | expected a type | `x as 3` |
| E0203 | expected a declaration / global variables are not supported | `i32 x = 3;` outside a function |
| E0204 | comparison operators cannot be chained | `a < b < c` |
| E0205 | assignment is a statement, not an expression | `if (x = 1)`, `a = b = c;` |
| E0206 | braces are required | `if (c) x += 1;` |
| E0207 | variable has no initial value | `i32 x;` |
| E0208 | lambdas are not available in v0 | not given any more: lambdas are available (spec §5.5) |
| E0209 | only one variable can be declared at a time | `i32 a, b = 0;` |
| E0210 | `->` is not used to access members | `p->x` |
| E0211 | the array size belongs to the type | `i32 a[4]` instead of `i32[4] a` |
| E0212 | modifier not allowed here | `virtual` in a `struct`, `pub import` |
| E0213 | only classes can have a parent class or interfaces | `struct S : Base` |
| E0214 | expected a node in a `.ui.sosu` file | `42` among the nodes of a window |
| E0215 | invalid node or property in a `.ui.sosu` file | `Button("OK", on_tap: f)`, `TextField()` without `bind:`, `Text("a") { }`, a property after the nodes, a `TitleBar` that is not the first node of a window, an `Option` outside a `Dropdown`, a `MenuItem` outside a menu, `row:` outside a `Grid`, a cell given twice, a `Tab` outside `Tabs`, a `Leaf` outside a `Tree`, a `Dialog` inside another node or without `open:`, `List(count:)` without `item:`, a style property or state that does not exist, a theme that starts from no theme |
| E0216 | not supported yet in a `.ui.sosu` file | not given any more: `style`, `theme`, `align:` and the grids it was given for are supported (spec §10.7, §10.8) |
| E0217 | generic declaration not supported yet | a virtual method or a method of an interface with type parameters of its own, a generic class with a parent class, an interface or a `virtual` method, an effect parameter outside a function, a generic C function (spec §5.4) |
| E0218 | invalid `export` | `export "Rust" ...`, `export "C"` before something that is not a function (spec §9.2) |
| E0219 | too deeply nested (more than 512 levels in the parser: a parenthesis counts two, a term of a sum one) | 20 000 nested parentheses, a sum of 1 000 terms, `ui8*` with 600 stars (spec §12) |

## Semantic analysis

| Code | Message | Example |
|---|---|---|
| E0301 | name already declared, or a built-in type | `struct A {}` twice, `struct i32 {}` |
| E0302 | name already used by an import | `import a.util; import b.util;`, `import std.io; struct io {}` |
| E0303 | unknown type or module | `Pont p;` (did you mean `Point`?), `io.Socket s;`, `u8 b;` (now `ui8`) |
| E0304 | not a type | a function or a module used as a type |
| E0305 | private to its module | `shapes.Secret` without `pub struct Secret` |
| E0306 | `void` can only be a return type | `void x;`, `void f(void x)` |
| E0307 | the error type of `T!E` must be an enum | `i32!str` |
| E0308 | an array has at least one element, of a constant size | `i32[0]`, `ui8[NAME]` where `NAME` is not an integer constant (spec §11.6) |
| E0309 | a type contains itself by value | `struct A { A a; }` |
| E0310 | invalid parent class or interface | `class C : SomeStruct`, the parent class not first |
| E0311 | inheritance cycle | `class A : B {}` and `class B : A {}` |
| E0312 | non-exhaustive match | `match` on `Shape` without `Empty` nor `_` |
| E0313 | `override` with nothing to override | |
| E0314 | redefinition without `override` | `void f()` in a subclass when the parent has `virtual void f()` |
| E0315 | overriding a method that is not virtual | |
| E0316 | override with a different signature | `override void f(i64 x)` for `virtual void f(i32 x)` |
| E0317 | override adds an effect | `override void f() uses io` for `virtual void f()` |
| E0318 | interface method not implemented | `class C : Drawable {}` without `draw` |
| E0319 | declared twice (field, method, variant, parameter, interface) | `void f(i32 a, i32 a)` |
| E0320 | unknown effect | `uses disk` |
| E0321 | class or interface used by value | `Widget w;` instead of `Widget* w;` |
| E0330 | unknown name | `cuont` (did you mean `count`?) |
| E0331 | type mismatch | `i32 b = a;` with `a: i64`, `f32 x = 3;` |
| E0332 | number literal out of range | `ui8 x = 256;` |
| E0333 | not a value | a type, a module or a method used as a value |
| E0334 | invalid operands | `i32 + i64`, `true + 1`, indexing or slicing a `str`, `-x` on a `ui32` or a `uf32` |
| E0335 | cannot assign | a variable or a field that is not `mut`, `&a` or `a[0..2]` of an array that is not `mut`, `v[0..2] = w` |
| E0336 | wrong number of arguments or variant fields | `g(1, 2)` for `g(i32 a)`, `(x, y) => x` given to a `fn(i32) -> i32` |
| E0337 | no such field, method or variant | `p.y` on a struct without `y`, `.copy()` on a value without an `Arena` |
| E0338 | not a function | `x()` with `x: i32`, `Point(...)` |
| E0339 | invalid conversion | `3.0 as bool`, `as?` to a type that is not a subclass |
| E0340 | missing effect | calling a `uses io` function from a function without `uses io`, `fn(i32) -> void g = (x) => io.print("{}", x);` |
| E0341 | unsafe operation outside `unsafe` | calling C, pointer arithmetic, `p as ui8*` |
| E0342 | type cannot be inferred | `auto x = none;`, `auto r = ok(1);` |
| E0343 | invalid `match` | unknown case, case matched twice, unreachable arm, wrong number of bindings |
| E0344 | `break`, `continue` or `return` out of place | `break` outside a loop, `return` in `defer` |
| E0345 | invalid `try` | `try` on a value that is not `T!E`, or in a function with another error type |
| E0346 | missing or unexpected return value | a path without `return`, `return 1;` in a `void` function |
| E0347 | name already used by a local | `i32 x` when a parameter is named `x` |
| E0348 | expression without effect, or unused result | `x == 1;`, a `T!E` call whose result is ignored |
| E0349 | `self`, `base` or `result` out of place | `self` outside a method, `result` in a function returning nothing |
| E0350 | invalid struct literal | missing or unknown field |
| E0351 | invalid `io.print` | format and values do not match, value that cannot be printed |
| E0352 | invalid range or loop | `auto r = 0..10;`, `for (char c in s)`, `v[0..=2]` |
| E0353 | address or view of a temporary value | `&g()`, `g()[0..2]` when `g` returns an array, `&s.f` for a field of function type (spec §5.5) |
| E0354 | invalid arena allocation | `a.alloc(3)`, `a.alloc(Node)` with a pointer inside, `a.alloc(SomeClass)` |
| E0355 | wrong number of elements in an array literal | `i32[3] a = [1, 2];` |
| E0356 | use of a moved value, or invalid move | `Arena b = a;` then `a.free()`, a move inside a loop, out of a field |
| E0360 | generic function: arguments not inferred, or used as a value | `auto x = zero();` (write `zero<i32>()`), `auto g = largest;` (spec §5.4) |
| E0361 | wrong type arguments | `Pair p` for a `Pair<A, B>`, `Pair<i32>`, `Point<i32>` for a type that is not generic |
| E0362 | a type argument does not fit its parameter | `largest(flags)` with `bool` for `<T: Ordered>`, `Vec<Arena>` (a resource), `greet(3)` for `<T: Named>`, `channel.new<str>(4)` (a `str` is not `Send`, spec §5.6) |
| E0364 | unknown constraint | `<T: Sortable>`; the constraints are `Equal`, `Ordered`, `Number`, `Integer`, `Hash`, `Send` and interfaces |
| E0365 | invalid C name of an export | two exports named `lib_open`, `export "C" void int()`, a name starting with `sosu_`, a generic export (spec §9.2) |
| E0366 | a type that cannot cross to C in an export | `i32?`, `T!E`, an enum with data, `Arena` by value, an interface, an array by value (spec §9.2) |
| E0367 | the value of a constant is not computed when the program is compiled | `i32 A = f();`, `ui8 A = 200 + 100;` (does not fit), `i32 A = B;` and `i32 B = A;` (a cycle), a division by zero (spec §11.6) |
| E0368 | a constant of a type other than a number, `bool`, `char` or `str` | `i32[2] PAIR = [1, 2];` (spec §11.6) |
| E0369 | invalid lambda | `auto g = (x) => x;` (the type of `x` is unknown), `n = x` in a lambda that captured `n`, a captured `Arena` (spec §5.5) |
| E0370 | a lambda that captures variables leaves its function | `return (i32 x) => x + k;`, `S{ h: (x) => x + k }`, `g = (x) => x + k;`, a C function that takes a function used as a value (spec §5.5) |
| E0371 | a returned pointer or view would point into the function | `return &x;` on a local or a parameter, `return &p.v;` on a local struct, `return a[0..2];` on a local array, also in `some(...)`, a literal or `?:` (spec §7.7); a pointer first kept in a variable is not followed |

## Code generation

| Code | Message | Example |
|---|---|---|
| E0501 | (no longer used: classes and interfaces are generated since arenas exist) | |
| E0502 | no valid `main` | `sosuc run` on a library, or `i32 main(i32 x)` |

## Packages and imports

| Code | Message | Example |
|---|---|---|
| E0401 | invalid sosu.toml syntax | `name "app"`, unterminated string, `name = 3` |
| E0402 | unknown section or key | `[tools]`, `[link.beos]`, `author = "..."`, `branch = "main"` in a dependency |
| E0403 | missing section or key | no `[package]`, no `name` or `version` |
| E0404 | invalid dependency | `x = "../x"`, no source, `path` and `git` together, `git` without `tag` or `rev`, a `"` or `$` in the URL or tag |
| E0405 | cannot fetch a git dependency | wrong URL, no such tag or rev, `git` not installed |
| E0406 | the package is not named like its dependency key | `geo = { path = "../geometry" }` |
| E0407 | circular dependency between packages | `a` depends on `b`, which depends on `a` |
| E0408 | duplicate key, dependency or section | `name` written twice, `[link]` twice |
| E0409 | invalid or reserved package name | `my-app`, a package named `std` |
| E0410 | unknown package | `import ui.button;` without `ui` in [dependencies] |
| E0411 | module not found | `import app.missing;` without `src/missing.sosu` |
| E0412 | the import does not name a module | `import std;` |
| E0413 | two different packages have the same name | two dependencies both named `shared` |
| E0414 | the standard library was not found | `lib/std` missing, `SOSU_HOME` wrong |
| E0415 | no package at the dependency path | `x = { path = "../nowhere" }` |
| E0416 | a value of sosu.toml has a character it cannot have | a quote, `` ` ``, `$`, `%` or a control character, which would end the quotes of a command or be expanded in them: `libs = ["m\" & calc"]` (the `name` of [app], a text shown, may have them) |
