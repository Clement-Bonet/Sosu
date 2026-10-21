# Error codes

Codes are stable (spec §7.5). Ranges: `E01xx` lexer, `E02xx` parser, `E03xx` semantic analysis.

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
