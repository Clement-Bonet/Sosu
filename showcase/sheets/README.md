# Sosu Sheets

A spreadsheet written in [Sosu](https://github.com/Clement-Bonet/Sosu): a sheet of 100 columns by 2,000 rows, a formula engine (about 120 functions), formats, colours, 100 levels of undo, and `.sheet`, `.ods` and `.csv` files.

![Sosu Sheets](screenshot.png)

## Build

From this folder, with `sosuc` built at the root of the repository:

```
..\..\packages\platform\fetch-sdl3.ps1   # once, on Windows
..\..\build\sosuc.exe test .             # 18 tests: engine, editor, files, ZIP/XML, OpenDocument
..\..\build\sosuc.exe run .
```

To run `build\sheets.exe` directly, after `sosuc build .`, copy `assets\` to `build\assets`. `build\sheets.exe --snapshot out.png` writes the window, with a demonstration sheet, in a picture.

## Typing

- Typing in a cell replaces its content; **F2** or a double click edits it; **Enter** confirms and goes down (Shift: up), **Tab** confirms and goes right, **Escape** cancels.
- A formula starts with `=`. While typing one, a click on a cell inserts its reference (`A1`, `$A$1`, `A1:B5`).
- An apostrophe forces text (`'007`). Numbers, percentages (`12%`), dates `yyyy-mm-dd`, `TRUE` and `FALSE` are recognized.
- Errors: `#DIV/0!`, `#REF!`, `#NAME?`, `#VALUE!`, `#N/A`, `#NUM!`, `#CIRC!` (circular reference).

## Shortcuts

| Key | Action |
|---|---|
| Arrows, Shift+arrows | move, extend the selection |
| Ctrl+arrows | go to the edge of the block of data |
| Home, Ctrl+Home, Ctrl+End, PgUp/PgDn | navigation |
| Ctrl+A | select all |
| Ctrl+C / X / V | copy, cut, paste (through the system clipboard, pasting from Excel works); relative references are shifted |
| Ctrl+D / Ctrl+R | fill down / right |
| Ctrl+Z / Ctrl+Y | undo / redo |
| Ctrl+B / I / U | bold, italic, underline |
| Ctrl+G | go to a cell (or a click on the name box) |
| Ctrl+F, F3 | find, find next |
| Ctrl+N / O / S | new, open, save |
| Delete | clear the content |

With the mouse: click and drag to select, click a column or row header to select it, drag the edge of a header to resize a column, the wheel to scroll (horizontally with sideways scrolling).

## Toolbar

Bold, italic, underline, alignment (left, centre, right), formats (General, Number, %, Currency €, Scientific, Date, Text), decimals + and -, text and fill colour (each click goes to the next colour), sort A to Z and Z to A (by the active column, on the selection or on the whole used area), insert and delete rows and columns (the references of formulas are rewritten), find. The status bar shows the Count, Sum, Average, Min and Max of the selection.

## Functions

- **Math**: SUM, PRODUCT, ABS, ROUND, ROUNDUP, ROUNDDOWN, INT, MOD, POWER, SQRT, EXP, LN, LOG, LOG10, SIGN, CEILING, FLOOR, TRUNC, PI, RAND, RANDBETWEEN, SIN, COS, TAN, ASIN, ACOS, ATAN, ATAN2, DEGREES, RADIANS, FACT, GCD, LCM, QUOTIENT, SUMPRODUCT, SUMSQ...
- **Statistics**: AVERAGE, MEDIAN, MODE, MIN, MAX, COUNT, COUNTA, COUNTBLANK, LARGE, SMALL, RANK, STDEV, VAR...
- **Conditions**: IF, IFERROR, IFNA, AND, OR, NOT, XOR, SUMIF, SUMIFS, COUNTIF, COUNTIFS, AVERAGEIF...
- **Text**: LEN, LEFT, RIGHT, MID, UPPER, LOWER, PROPER, TRIM, CONCAT, CONCATENATE, TEXTJOIN, FIND, SEARCH, SUBSTITUTE, REPLACE, REPT, TEXT, VALUE, EXACT, CHAR, CODE...
- **Lookup**: VLOOKUP, HLOOKUP, INDEX, MATCH, CHOOSE, ROW, COLUMN, ROWS, COLUMNS...
- **Dates**: TODAY, NOW, DATE, YEAR, MONTH, DAY, WEEKDAY, DAYS...
- Operators: `+ - * / ^ & = <> < <= > >=`, postfix `%`, ranges `A1:B5`, absolute references `$A$1`.

## Files

- `.sheet`: the native text format (contents, formats, colours, column widths).
- `.ods` (OpenDocument): read and written, one sheet. Kept: values, formulas (`of:=SUM([.A1:.B2])`), bold, italic, underline, alignment, formats, colours, widths. Lost: other sheets, charts, conditional formatting. The ZIP container (`zip.sosu`), the XML parser (`xml.sosu`) and the conversion (`ods.sosu`) are in `src`.
- `build\sheets.exe --convert input output` converts a file (`.csv`, `.sheet` or `.ods` to `.sheet` or `.ods`).
- `.csv`: import (separator `,` or `;`, quotes) and export. A file dropped on the window is opened.

## Limits

One sheet only, no charts, no array formulas, no conditional formatting. Everything is calculated again at each change.

The interface was tried in a window on Windows, and checked with pictures (`--snapshot`) and by the tests. It was not tried on Linux and macOS.
