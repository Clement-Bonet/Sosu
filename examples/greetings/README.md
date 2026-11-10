# Greetings

An example of the package `i18n` of [Sosu](../../README.md). No window, no network, nothing to type.

The package `i18n` (spec §10.7) in a small task: the texts of a program in English, with the catalogs of `locales/` (the format of gettext) in French and Russian. For each of three users, `catalog.best` picks the language that suits them (French of Canada takes the French catalog, a language without a catalog keeps English); then a greeting with a name filled in, a number of messages in the plural form of each language (Russian has three), a text in two contexts (a button and a state), and a translation marked `fuzzy`, not checked by a translator yet, which shows the English text.

A program with a window asks the system for the languages of its user (`app.locales` of `platform`), and `ui` translates the texts of its `.ui.sosu` files by itself (`examples/workspace`).

Run it from the root of the repository:

```sh
sosuc run examples/greetings
```

Its output is checked by the tests (`tests/program_tests.c`, against `tests/examples/greetings.txt`).
