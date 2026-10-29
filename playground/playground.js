// The page of the playground: an editor, and sosuc compiled to WebAssembly (sosuc.js, built by
// `make playground` from src/playground/playground.c).
"use strict";

const EXAMPLES = {
    "Shapes and errors (README)": `import std.io;
import std.text;

enum Shape {
    Circle(f64 radius),
    Rect(f64 width, f64 height),
    Square(f64 side),
}

f64 area(Shape shape) {
    return match (shape) {
        Circle(r) => 3.14159 * r * r,
        Rect(w, h) => w * h,
        Square(s) => s * s,
    };
}

// No exceptions and no null: failure is in the type (\`T!E\`), absence too (\`T?\`). What allocates
// receives an arena.
i64!text.ParseError total(Arena* arena, str line) {
    mut i64 sum = 0;
    for (str word in text.words(arena, line)) {
        sum += try text.parse_i64(word);
    }
    return ok(sum);
}

// Effects are written in the signature: \`main\` prints, so it \`uses io\`.
i32 main() uses io {
    mut Arena arena = Arena.new(6 * KB);  // no hidden allocation: memory comes from arenas
    defer arena.free();
    io.print("{}", area(Circle(2.0)));
    io.print("{}", area(Rect(3.0, 4.0)));
    io.print("{}", area(Square(5.0)));
    match (total(&arena, "1 2 39")) {
        ok(n) => io.print("total: {}", n),
        err(e) => io.print("not a number"),
    }
    return 0;
}
`,
    "Effects: a missing \`uses io\`": `import std.io;

// \`greet\` prints but its signature does not say so: sosuc refuses it.
// Add \`uses io\` after the parameters and compile again.
void greet(str name) {
    io.print("hello, {}", name);
}

i32 main() uses io {
    greet("Sosu");
    return 0;
}
`,
};

const $ = (id) => document.getElementById(id);
const source = $("source"), output = $("output"), status = $("status");
let sosu = null;

// The code is kept in the fragment of the URL: a link shares it, nothing is sent to a server.
function encode(text) {
    return btoa(String.fromCharCode(...new TextEncoder().encode(text)));
}
function decode(hash) {
    try {
        return new TextDecoder().decode(Uint8Array.from(atob(hash), (c) => c.charCodeAt(0)));
    } catch {
        return null;
    }
}

function show(kind, text, message) {
    output.textContent = text;
    output.className = kind === "e" ? "error" : "";
    status.textContent = message;
    $("copy").disabled = kind !== "c";
}

// Runs sosu_playground_compile or sosu_playground_format. A fatal error of the compiler (exit, abort) leaves the module
// unusable: it is loaded again for the next call.
function call(name) {
    try {
        const pointer = sosu.ccall(name, "number", ["string"], [source.value]);
        const out = sosu.UTF8ToString(pointer);
        sosu._free(pointer);
        return out;
    } catch (e) {
        load();
        return "esosuc stopped: " + (e && e.message ? e.message : e) + "\n";
    }
}

function compile() {
    if (!sosu) return;
    const started = performance.now();
    const out = call("sosu_playground_compile");
    const ms = Math.round(performance.now() - started);
    if (out[0] === "c") show("c", out.slice(1), `Generated C99 · ${ms} ms`);
    else show("e", out.slice(1), "The program has errors");
}

function format() {
    if (!sosu) return;
    const out = call("sosu_playground_format");
    if (out[0] === "c") {
        source.value = out.slice(1);
        compile();
    } else {
        show("e", out.slice(1), "Cannot format: the file does not parse");
    }
}

function load() {
    sosu = null;
    $("compile").disabled = $("format").disabled = true;
    createSosu().then((module) => {
        sosu = module;
        $("compile").disabled = $("format").disabled = false;
        compile();
    }, (e) => show("e", String(e), "The compiler did not load"));
}

const select = $("example");
for (const name of Object.keys(EXAMPLES)) select.add(new Option(name, name));
select.addEventListener("change", () => {
    source.value = EXAMPLES[select.value];
    history.replaceState(null, "", location.pathname);
    compile();
});

source.value = (location.hash.length > 1 && decode(location.hash.slice(1))) || EXAMPLES[select.value];

source.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
        e.preventDefault();
        compile();
    } else if (e.key === "Tab" && !e.shiftKey) {
        e.preventDefault();
        document.execCommand("insertText", false, "    ");
    }
});

$("compile").addEventListener("click", compile);
$("format").addEventListener("click", format);
$("share").addEventListener("click", async () => {
    history.replaceState(null, "", "#" + encode(source.value));
    try {
        await navigator.clipboard.writeText(location.href);
        status.textContent = "Link copied";
    } catch {
        status.textContent = "Link in the address bar";
    }
});
$("copy").addEventListener("click", async () => {
    try {
        await navigator.clipboard.writeText(output.textContent);
        status.textContent = "C copied";
    } catch {
        status.textContent = "Select the C to copy it";
    }
});

load();
