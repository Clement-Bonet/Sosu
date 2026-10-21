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
| E0108 | invalid number literal (digits or suffix) | `0b102`, `12abc`, `0x`, `300u8` |
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
| E0208 | lambdas are not available in v0 | `(i32 a) => a + 1` |
| E0209 | only one variable can be declared at a time | `i32 a, b = 0;` |
| E0210 | `->` is not used to access members | `p->x` |
| E0211 | the array size belongs to the type | `i32 a[4]` instead of `i32[4] a` |
| E0212 | modifier not allowed here | `virtual` in a `struct`, `pub import` |
| E0213 | only classes can have a parent class or interfaces | `struct S : Base` |

## Semantic analysis

| Code | Message | Example |
|---|---|---|
| E0301 | name already declared, or a built-in type | `struct A {}` twice, `struct i32 {}` |
| E0302 | name already used by an import | `import a.util; import b.util;`, `import std.io; struct io {}` |
| E0303 | unknown type or module | `Pont p;` (did you mean `Point`?), `io.File f;` |
| E0304 | not a type | a function or a module used as a type |
| E0305 | private to its module | `shapes.Secret` without `pub struct Secret` |
| E0306 | `void` can only be a return type | `void x;`, `void f(void x)` |
| E0307 | the error type of `T!E` must be an enum | `i32!str` |
| E0308 | an array has at least one element | `i32[0]` |
| E0309 | a type contains itself by value | `struct A { A a; }` |
| E0310 | invalid parent class or interface | `class C : SomeStruct`, the parent class not first |
| E0311 | inheritance cycle | `class A : B {}` and `class B : A {}` |
| E0312 | non-exhaustive match (reserved, spec §7.5) | |
| E0313 | `override` with nothing to override | |
| E0314 | redefinition without `override` | `void f()` in a subclass when the parent has `virtual void f()` |
| E0315 | overriding a method that is not virtual | |
| E0316 | override with a different signature | `override void f(i64 x)` for `virtual void f(i32 x)` |
| E0317 | override adds an effect | `override void f() uses io` for `virtual void f()` |
| E0318 | interface method not implemented | `class C : Drawable {}` without `draw` |
| E0319 | declared twice (field, method, variant, parameter, interface) | `void f(i32 a, i32 a)` |
| E0320 | unknown effect | `uses disk` |
| E0321 | class or interface used by value | `Widget w;` instead of `Widget* w;` |

## Packages and imports

| Code | Message | Example |
|---|---|---|
| E0401 | invalid sosu.toml syntax | `name "app"`, unterminated string, `name = 3` |
| E0402 | unknown section or key | `[tools]`, `author = "..."`, `branch = "main"` in a dependency |
| E0403 | missing section or key | no `[package]`, no `name` or `version` |
| E0404 | invalid dependency | `x = "../x"`, no source, `path` and `git` together, `git` without `tag` or `rev` |
| E0405 | git dependencies are not supported yet | `ui = { git = "...", tag = "v1" }` |
| E0406 | the package is not named like its dependency key | `geo = { path = "../geometry" }` |
| E0407 | circular dependency between packages | `a` depends on `b`, which depends on `a` |
| E0408 | duplicate key or dependency | `name` written twice |
| E0409 | invalid or reserved package name | `my-app`, a package named `std` |
| E0410 | unknown package | `import ui.button;` without `ui` in [dependencies] |
| E0411 | module not found | `import app.missing;` without `src/missing.sosu` |
| E0412 | the import does not name a module | `import std;` |
| E0413 | two different packages have the same name | two dependencies both named `shared` |
| E0414 | the standard library was not found | `lib/std` missing, `SOSU_HOME` wrong |
| E0415 | no package at the dependency path | `x = { path = "../nowhere" }` |
