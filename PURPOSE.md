# Why Sosu exists

Sosu is a native, low-level language built around one idea: **you should know what a function does by reading its signature.**

## Why I started it

I started Sosu to fill a gap and take the best of C, C# and Java.

## The problem

In C or C++, a signature gives the types of the parameters and the result. It does not say whether the function allocates, reads a file, asks for the time or can fail. To find out, you open its body, then the bodies of what it calls. In C#, the garbage collector and exceptions make the code simple to write, but they also hide when memory is used and which failures can arrive.

Most of the work on an existing program is reading it. The cost of what a signature hides is paid every time someone reads the code.

## What Sosu changes

- **Effects are in the signature.** A function that prints says `uses io`. The compiler checks it, along with `time`, `random` and `threads`.
- **Failure and absence are in the types.** `T!E` can fail and `T?` can be absent. There are no exceptions and no null pointers outside `unsafe`.
- **Allocations are visible.** What allocates receives an `Arena*`. There is no garbage collector, no reference counting and no global allocator.
- **A piece of code can be read alone.** There are no implicit conversions, no overloading, no macros and no hidden control flow.
- **It stays familiar.** The syntax is meant to be readable by a C++ or C# developer.
- **It runs where C runs.** `sosuc` is written in C99 with libc only, generates C, and calls C libraries directly.

## An example

In C, nothing in this signature says that the function writes to the screen:

```c
void greet(const char *name) {
    printf("hello, %s\n", name);
}
```

In Sosu, the signature says it, and the compiler refuses the function without `uses io`:

```csharp
void greet(str name) uses io {
    io.print("hello, {}", name);
}
```

## Why not an existing language

Most of my career is in C#, with some C++. I have not used Zig, Odin or Rust in practice, so I do not compare Sosu with them here.

- **C#** is the syntax Sosu stays close to. But it needs a garbage collector, a method does not say which exceptions it throws, and its nullable annotations give warnings while null still exists at run time.
- **C++** gives control over memory, but much of what a line does is out of sight: implicit conversions, overloads, copies, templates and exceptions.
- **C** is small and runs everywhere, which is why Sosu generates it. But a failure is a return value that can be ignored, a pointer can be null, and bounds are not checked.

## What it costs

What makes the code simple to read makes it longer to write.

- **Signatures change.** A function that starts to print now says `uses io`, and so does every function that calls it, up to `main`. A function that starts to allocate needs an `Arena*`, which its callers must provide.
- **Memory is your job.** You choose which arena owns what and when it is freed, and you mark with `keeps` a parameter that is stored beyond the call.
- **More is spelled out.** No overloading means a different name for each function, and no implicit conversion means more `as`. Failures are handled at each call, with `try` or `match`.
- **Building takes two steps.** `sosuc` is fast, but the C compiler that follows adds its own time.
- **There is little around it.** Sosu is young: few libraries, and no outside users to learn from.

## What comes first

When goals conflict, these are the choices Sosu makes today.

1. **Reading comes before writing.** A longer signature is accepted if it saves opening the body.
2. **Simple signatures come before complete guarantees.** There are no lifetimes in the types: the regions catch less than lifetimes would, which is why Sosu is not memory safe, and does not aim to be.
3. **Speed comes before some checks.** Bounds stay checked in release, unless `--checks=off` is given, but overflows and contracts are not checked by default.
4. **Reaching every platform comes before independence.** Sosu depends on a C compiler, because it brings the same code to every system.

## Who it is for

Sosu is for developers who want a native program without a garbage collector, and who want to read code without having to trust what it hides. They may come from C, C++ or C#.

## What it can be used for

With version 1.0, many sorts of applications will be possible:
* Windows
* Linux
* macOS
* Android
* iOS, once it has run on an iPhone

## What it does not claim

- **It is not ready for production.** Sosu is young (0.x), the language can still change, and it has no outside users yet.
- **It is not memory safe.** The regions refuse a pointer that outlives its arena or its function, but they do not follow `unsafe` or C, and without lifetimes they cannot prove every case. A program can still have undefined behaviour. What is not checked is listed in [spec §7.7](docs/SPEC.md).
- **It does not replace Rust today.**
- **It does not remove the C compiler.** `sosuc` needs one to produce a program.

## How the idea is tested

A language is only as good as what can be written in it, so Sosu is tested on real programs: [seven desktop applications](showcase), among them a spreadsheet, a notes application in Markdown, a drum machine and a pixel art editor. They are drawn by Sosu's own interface toolkit, have their own tests, and show where the language falls short.

I designed Sosu. Much of the compiler, the standard library and the documentation was written with an AI coding assistant (Claude), under my direction. The [README](README.md#how-sosu-is-made) describes how, and the [roadmap](docs/ROADMAP.md) publishes the time and tokens measured.

## Where it is going

The next goal is version 1.0, whose content and order are in [spec §13.1](docs/SPEC.md).

## What I hope for

I hope that, one day, people will use Sosu to build real applications.

## Try it

You can try the language in the [playground](https://clement-bonet.github.io/Sosu/): it checks a program and shows the generated C, but does not run it.

I will be happy to [discuss it](https://github.com/Clement-Bonet/Sosu/discussions) with you.
