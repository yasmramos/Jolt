# Jolt for VS Code

Syntax highlighting for the [Jolt](../..) programming language (`.jolt`).

This is a small, dependency-free extension: it contributes a language id, a
TextMate grammar and an editor configuration. It has no runtime code, so it
loads instantly and needs no build step.

## What it highlights

- Keywords: `let`, `var`, `const`, `fn`, `struct`, `native`, `extern`,
  `import`, `if`, `else`, `while`, `for`, `in`, `switch`, `case`, `default`,
  `try`, `catch`, `defer`, `break`, `continue`, `return`.
- Constants: `true`, `false`, `nil`.
- Types: the primitive and FFI vocabulary (`int`, `float`, `double`, `string`,
  `bool`, `array`, `map`, `error`, `range`, `ptr`, `cstring`, `size_t`, the
  fixed-width integers, ...), the nullable marker `T?` and a user type in an
  annotation (`let p: Point`).
- Numbers: integers, doubles, `f`/`F` floats and `_` separators.
- Strings with `\n`, `\t`, `\xNN`, `\uNNNN` escapes and `${ ... }`
  interpolation (the embedded expression is highlighted as Jolt).
- Comments: `//` and nestable `/* ... */`.
- Imports: `import std.math;`, `import a.b.*;`, `import a.b as m;` and
  `import { x, y } from a.b;`.
- `extern "C" { fn ... }`, `extern fn ...;` and `native fn ...;` declarations.
- Builtins (`print`, `len`, `sqrt`, `error`, `pcall`, ...) and calls.
- Operators, including `**`, `??`, `?.`, `!!`, `..`, `...`, `=>`.

## Install (development)

Copy or symlink this folder into your VS Code extensions directory:

```sh
ln -s "$(pwd)/editors/vscode" ~/.vscode/extensions/jolt
```

Then reload the window. Open any `.jolt` file and pick "Jolt" as the language
mode if it is not detected automatically (the `.jolt` extension is registered).

Alternatively, press `F5` in VS Code with this folder open to launch an
Extension Development Host.

## Package a `.vsix` (optional)

```sh
npm install -g @vscode/vsce
cd editors/vscode
vsce package
code --install-extension jolt-0.1.0.vsix
```

## Files

| File | Purpose |
| --- | --- |
| `package.json` | Extension manifest: language id, file extension, grammar. |
| `language-configuration.json` | Comments, brackets, auto-closing pairs, folding. |
| `syntaxes/jolt.tmLanguage.json` | The TextMate grammar. |

## Notes

The grammar is intentionally standalone: it keeps its own keyword/type/builtin
lists rather than reading them from the compiler. When the language gains a
keyword or a builtin, update `syntaxes/jolt.tmLanguage.json` in the same change.
