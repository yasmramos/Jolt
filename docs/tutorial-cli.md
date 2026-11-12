# Tutorial: a native CLI tool in Jolt

This tutorial builds a small command-line tool with Jolt and ships it as a
**self-contained native binary**: no interpreter, no runtime to install, no
shared libraries. It is the use case Jolt is designed for — a fast, small,
predictable CLI — and it exercises the whole pipeline (lexer → parser →
compiler → bytecode → AOT backend).

The numbers below were measured on Linux x86-64 (Debian, g++ 14, `-O2`) from
the tree at the time of writing. Re-measure on your machine before quoting
them.

---

## 1. Why AOT, and what it buys you

`jolt <file.jolt>` compiles to bytecode and runs it on the register VM. That is
the **development engine**: fast iteration, a REPL, and the JIT for hot loops.
`jolt build <file.jolt>` compiles the same source all the way to **machine code**
and writes a static executable. That is the **product**.

| Property | `jolt` (dev engine) | `jolt build` (AOT) |
| --- | --- | --- |
| Needs Jolt installed to run | yes | **no** |
| Runtime in the binary | VM + JIT | embedded runtime only |
| Startup | ~1.7 ms (VM boot) | **~0.5 ms** |
| Hello-world binary | — | **24 KB** |
| Distribution | ship the `.jolt` + Jolt | ship **one file** |

The AOT binary links **no libc and no libstdc++**: it talks to the kernel
through direct syscalls and carries its own bump allocator and value runtime.
`ldd` reports *"not a dynamic executable"*.

For comparison, a hello-world compiled with `gcc -static` against glibc is
about **740 KB**; the Jolt binary is roughly **30× smaller** because it drags in
no C runtime.

---

## 2. A real tool: word frequency

Here is a complete program — `wordcount.jolt` (it is also in `examples/`). It
counts words, computes percentages and prints a report. It uses functions,
arrays, maps, `for-in`, the key/value form of `for-in`, `in`, indexing and
string interpolation:

```jolt
fn tally(words) {
  var counts = {};
  for w in words {
    if (w in counts) {
      counts[w] = counts[w] + 1;
    } else {
      counts[w] = 1;
    }
  }
  return counts;
}

fn report(counts, total) {
  print("word frequency report");
  print("total words:", total);
  for k, v in counts {
    var pct = (v * 100) / total;
    print(" ", k, v, pct);
  }
}

var words = ["red", "blue", "red", "green", "blue", "red"];
report(tally(words), words.length);
```

Run it with the development engine:

```sh
$ jolt wordcount.jolt
word frequency report
total words: 6
  red 3 50
  blue 2 33
  green 1 16
```

Now build it:

```sh
$ jolt build wordcount.jolt
$ ./wordcount
word frequency report
total words: 6
  red 3 50
  blue 2 33
  green 1 16
```

Same output, byte for byte. The difference is that `./wordcount` is a native
binary you can copy to a machine with no Jolt, no Python and no shared
libraries:

```sh
$ ls -l wordcount
-rwxr-xr-x 1 you you 26K wordcount
$ ldd wordcount
        not a dynamic executable
```

---

## 3. What the AOT backend supports today

`jolt build` is complete for the **core of the language**. A program that stays
inside this set compiles to native code:

- literals: `nil`, `true`/`false`, integers, floats/doubles, strings;
- arithmetic (`+ - * / % **`), bitwise (`& | ^ << >>`), unary (`- ! ~`) and
  every comparison;
- `if`/`else`, `while`, classic `for`, `for-in` (including `for k, v in map`),
  `break`, `continue`, ternary;
- functions, recursion, mutual recursion, closures and lambdas that capture
  their environment;
- arrays and maps as **data**: `[...]`, `{...}`, indexing (`xs[i]`, `xs[-1]`),
  members (`m.name`), `in` for arrays, maps and strings;
- `print`, `write` and `eprint` (the builtins the embedded runtime provides);
- string interpolation.

A few things the development engine offers are **not** in the AOT backend yet.
`jolt build` reports them as a lowering error instead of emitting a broken
binary — for example `args()`, `exit()`, `type()`, `len()`, `range()` and the
array/map/string methods (`concat`, `slice`, `sorted`, `keys`, `upper`, …):

```sh
$ echo 'print(range(3));' > r.jolt && jolt build r.jolt
lowering: global 'range' is read but never assigned
```

That is a deliberate design choice (§3 of `AGENT.md`: no VM-only features), not
a crash: the program is rejected at compile time, never mis-compiled. Until the
rest of the standard library is lowered, keep `args()` and file I/O in the
development engine, or pass data in as literals.

---

## 4. Development speed vs. product speed

The development engine runs the same bytecode on the VM, with the JIT
specialising integer leaf functions. A tight integer loop shows the three
speeds on the same source (`n = 30_000_000`):

| Engine | Time |
| --- | --- |
| `jolt --no-jit` (pure VM) | ~6.2 s |
| `jolt` (VM + JIT) | **~0.27 s** |
| `jolt build` then `./prog` (AOT) | ~0.62 s |

The JIT wins on a pure integer loop because it compiles exactly that
specialisation (unboxed `int`). The AOT binary is **general** — boxed values,
full dynamic semantics — so it is slower than a specialised JIT path here, but
it is still about **10× faster than the plain VM**, starts in half a
millisecond, and is the only one of the three that runs with no Jolt installed.

Use the JIT while you develop; ship the AOT binary.

---

## 5. Startup matters for a CLI

A CLI tool is launched over and over — from a shell, a script, a Makefile, a
CI job. Startup latency is a feature. Measured over 100 launches:

| Tool | Per-launch |
| --- | --- |
| `./wordcount` (Jolt AOT) | **~0.5 ms** |
| `python3 -c "print('hi')"` | ~12.5 ms |

A Jolt binary starts in well under a millisecond because there is nothing to
initialise: no interpreter, no GC heap, no shared-library resolution. It maps
its few kilobytes, runs, and exits.

---

## 6. Distribution

Because the binary is static and self-contained, "installing" your tool is
copying one file:

```sh
$ jolt build wordcount.jolt -o /usr/local/bin/wordcount
$ scp wordcount server:/usr/local/bin/
```

No `jolt` on the target, no matching runtime version, no `LD_LIBRARY_PATH`, no
container needed just to run a program. That is the model of `AGENT.md` §2:
`joltc` produces a self-contained native binary; end users never install Jolt
or any runtime.

---

## 7. Where to go next

- `docs/language.md` — the full language reference.
- `AGENT.md` §4 (PHASE 7) — the AOT backend design and its sub-slices.
- `AGENT.md` §4 (PHASE 8) — the JIT, development engine only.
- `examples/` — runnable programs (`collections.jolt`, `closures.jolt`, …).
