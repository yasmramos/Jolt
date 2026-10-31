# Jolt Language Reference

Language version: **0.1.0**

This guide describes the syntax and semantics of Jolt as implemented by the interpreter included in this repository.

## Table of contents

1. [Execution model](#1-execution-model)
2. [Lexical structure](#2-lexical-structure)
3. [Data types](#3-data-types)
4. [Variables and scope](#4-variables-and-scope)
5. [Operators](#5-operators)
6. [Statements](#6-statements)
7. [Functions and closures](#7-functions-and-closures)
8. [Arrays](#8-arrays)
9. [Strings](#9-strings)
10. [Maps](#10-maps)
11. [Ranges](#11-ranges)
12. [Standard library](#12-standard-library)
13. [Errors and diagnostics](#13-errors-and-diagnostics)
14. [REPL](#14-repl)
15. [Command line](#15-command-line)
16. [Known limitations](#16-known-limitations)
17. [Complete example](#17-complete-example)

---

## 1. Execution model

A Jolt program is a UTF-8 text file (extension `.jolt`). The interpreter processes it in three stages:

1. **Lexer**: turns the text into tokens.
2. **Parser**: builds an AST with a recursive-descent parser and operator precedence.
3. **Interpreter**: walks the AST and evaluates the statements in a lexical environment.

The program runs from top to bottom in the global scope. Simple statements end with `;`. Statements with a block (`if`, `while`, `for`, `fn`) close with `}` and optionally accept a `;` after it.

```jolt
let a = 1;
if (a == 1) {
  print("one");
}; // the ';' is optional after the block
```

A file may start with a `#!...` line (shebang); the lexer ignores it.

---

## 2. Lexical structure

### 2.1 Comments

```jolt
// Line comment

/* Block comment.
   /* Block comments can be nested. */
*/
```

`//` always starts a comment; there is no integer division operator.

### 2.2 Identifiers and reserved words

Identifiers start with an ASCII letter or `_`, and continue with ASCII letters, digits or `_`. They are case-sensitive.

Reserved words:

```text
break  continue  else  false  fn  for  if  in  let
nil    return true  var     while
```

They cannot be used as names of variables, parameters or unquoted map keys.

### 2.3 Numbers

| Form | Example | Value |
| --- | --- | --- |
| Decimal | `123` | `123` |
| Separators | `1_000_000` | `1000000` |
| Hexadecimal | `0x2A` | `42` |
| Binary | `0b101010` | `42` |
| Octal | `0o52` | `42` |
| Float | `1.5`, `0.25` | `1.5`, `0.25` |
| Scientific notation | `1.5e3`, `2E-2` | `1500.0`, `0.02` |

- Integers are 64-bit signed. An out-of-range literal is a compile error.
- The `-` sign is a unary operator, not part of the literal.
- A dot is only part of a float when followed by a digit: `1.` is not a valid float; write `1.0`.
- Underscores are ignored inside digits: `1_0` is equivalent to `10`.

### 2.4 Strings

Strings are written between double quotes. There are no single quotes or raw strings. They may span several lines:

```jolt
let greeting = "Hello";
let text = "first line
second line";
```

Escape sequences:

| Escape | Meaning |
| --- | --- |
| `\n` | Newline |
| `\t` | Tab |
| `\r` | Carriage return |
| `\0` | Null byte |
| `\\` | Backslash |
| `\"` | Double quote |
| `\$` | Literal dollar sign |
| `\xNN` | Hexadecimal byte (two digits) |
| `\uXXXX` | Unicode code point (four digits) |

### 2.5 Interpolation

Inside a string, `${expression}` evaluates an expression and converts it to text:

```jolt
let name = "Ada";
let age = 36;
print("${name} is ${age} years old");
print("2 + 3 = ${2 + 3}");
print("result: ${ {a: 1}.a }");
```

Interpolated expressions may contain strings and nested braces:

```jolt
print("nested: ${"inner ${1 + 1}"}");
```

To write a literal `${`, escape the dollar sign:

```jolt
print("\${not evaluated}"); // ${not evaluated}
```

Strings are UTF-8 and **immutable**. Length and index operations work on code points, not bytes.

---

## 3. Data types

| Type | Values | Notes |
| --- | --- | --- |
| `nil` | `nil` | Absence of a value. |
| `bool` | `true`, `false` | Logical values. |
| `int` | 64-bit signed integers. | Arithmetic wraps around on overflow. |
| `float` | IEEE 754 double-precision floating-point numbers. | Can be `inf`, `-inf` or `nan` as the result of operations. |
| `string` | UTF-8 text. | Immutable. |
| `array` | Ordered sequence of values. | Mutable and by reference. |
| `map` | Collection of string keys. | Preserves insertion order. Mutable and by reference. |
| `function` | Native or user-defined functions. | First-class citizens. |
| `range` | Interval of integers. | Immutable. |

The `type(value)` function returns the name of the type as a string.

### 3.1 Truthiness

Only two values are falsy:

```jolt
false
nil
```

Everything else is truthy, including `0`, `0.0`, `""`, `[]` and `{}`.

```jolt
if (0) {
  print("0 is truthy in Jolt");
}
```

### 3.2 Equality

- `int` and `float` are compared numerically: `1 == 1.0` is `true`.
- Strings are compared by content.
- Arrays and maps are compared **structurally**, in depth.
- Ranges compare start, end and inclusiveness.
- Functions are compared by identity (the very same function).
- Values of different types (except `int`/`float`) are not equal: `0 == false` is `false`.

### 3.3 Mutability and references

Assigning an array or a map to another variable **does not copy it**: both variables point to the same value.

```jolt
let a = [1];
let b = a;
b.push(2);
print(a); // [1, 2]
```

For a shallow copy use `.clone()`. Strings are immutable, so they behave like values.


---

## 4. Variables and scope

### 4.1 Declaration

```jolt
var counter;           // initialized to nil
var name = "Jolt";     // mutable binding, initializer optional
let LIMIT = 10;        // immutable binding, initializer required
```

`var` declares a mutable variable. `let` declares an immutable binding: it requires an initializer and assigning another value to it later is rejected (at runtime in the interpreter, at compile time in the bytecode compiler). Use `let` by default for values that never change, and `var` only when reassignment is needed.

### 4.2 Block scope

Every block `{ ... }` creates a new scope. Variables are resolved from the current scope up through the parents.

```jolt
let x = 1;
{
  let x = 2;   // shadows the outer x
  print(x);    // 2
}
print(x);      // 1
```

Redeclaring a name in the same block replaces the previous definition.

### 4.3 Assignment

Assignment looks the variable up in the current scope and, if it does not exist, in the parent scopes. It does not create new variables:

```jolt
let x = 1;
{
  x = 2; // modifies the outer x
}
print(x); // 2

y = 3; // error: undefined variable 'y'
```

### 4.4 Function declarations

`fn name(...) { ... }` declarations are hoisted within their block, so they can be called before the line where they appear. This allows recursion and mutual recursion.

`var` and `let` declarations are **not** hoisted: using a variable before its declaration produces an error.

---

## 5. Operators

### 5.1 Precedence

From highest to lowest precedence:

| Precedence | Operators | Associativity |
| --- | --- | --- |
| 1 | `()` `[]` `.` | left |
| 2 | `**` | right |
| 3 | `!` `-` `+` `~` (unary) | right |
| 4 | `*` `/` `%` | left |
| 5 | `+` `-` | left |
| 6 | `..` `..=` | left |
| 7 | `<<` `>>` | left |
| 8 | `<` `<=` `>` `>=` `in` | left |
| 9 | `==` `!=` | left |
| 10 | `&` | left |
| 11 | `^` | left |
| 12 | `\|` | left |
| 13 | `&&` | left |
| 14 | `\|\|` | left |
| 15 | `?:` | right |
| 16 | `=` `+=` `-=` `*=` `/=` `%=` `**=` | right |

Notes:

- `**` is right-associative: `2 ** 3 ** 2` is `2 ** (3 ** 2) = 512`.
- Unary minus has lower precedence than `**`: `-2 ** 2` is `-(2 ** 2) = -4`.
- Ranges have higher precedence than shifts and comparisons: `2 in 1..5` is equivalent to `2 in (1..5)`.
- Comparisons do not chain: `1 < 2 < 3` tries to compare a boolean with an integer and produces an error.

### 5.2 Arithmetic

| Operator | Description |
| --- | --- |
| `+` | Addition of numbers, concatenation of strings or arrays. |
| `-` | Subtraction. |
| `*` | Multiplication. |
| `/` | Division. |
| `%` | Remainder. |
| `**` | Power. |

Rules:

- If both operands are `int`, `+ - * / %` produce `int`.
- Integer division truncates toward zero: `7 / 2 == 3`, `-7 / 2 == -3`.
- The integer remainder takes the sign of the dividend: `-7 % 3 == -1`.
- If at least one operand is `float`, the result is `float`.
- Dividing or taking the remainder with a zero divisor is a runtime error, for floats too.
- `**` returns `int` if base and exponent are integers and the exponent is not negative; otherwise it uses `std::pow` and returns `float`.
- `int` overflow wraps around (two's complement).
- `+` concatenates two strings and also two arrays (creating a new array).

```jolt
assert_eq(7 / 2, 3);
assert_eq(7 / 2.0, 3.5);
assert_eq(2 ** 10, 1024);
assert_eq("a" + "b", "ab");
assert_eq([1] + [2], [1, 2]);
```

### 5.3 Comparison

`<`, `<=`, `>`, `>=` work with:

- two numbers (`int` or `float`, mixed);
- two strings (lexicographic order by UTF-8 bytes);
- two booleans (`false < true`).

Comparing incompatible types is an error.

### 5.4 Equality

`==` and `!=` use the structural equality described in section 3.2.

### 5.5 Logical

| Operator | Description |
| --- | --- |
| `!` | Logical negation; always returns `bool`. |
| `&&` | Logical AND with short-circuiting. |
| `\|\|` | Logical OR with short-circuiting. |

`&&` and `||` **return one of their operands**, not a boolean:

```jolt
assert_eq(nil || "default", "default");
assert_eq("value" || "default", "value");
assert_eq(false && 123, false);
assert_eq(true && 123, 123);
assert_eq(nil && 123, nil);
```

The right operand is only evaluated if needed.

### 5.6 Bitwise

| Operator | Description |
| --- | --- |
| `&` | Bitwise AND. |
| `\|` | Bitwise OR. |
| `^` | Bitwise XOR. |
| `~` | One's complement (unary). |
| `<<` | Left shift. |
| `>>` | Arithmetic (signed) right shift. |

They only accept `int`. Shifting requires a count between 0 and 63.

```jolt
assert_eq(6 & 3, 2);
assert_eq(6 | 3, 7);
assert_eq(1 << 4, 16);
assert_eq(256 >> 3, 32);
assert_eq(~0, -1);
```

### 5.7 Membership

The `in` operator tests membership:

| Right side | Behavior |
| --- | --- |
| array | Structural equality with some element. |
| map | The left side must be a string; checks whether the key exists. |
| string | The left side must be a string; checks whether it is a substring. |
| range | The left side must be an integer; checks whether it lies within the range. |

```jolt
assert_eq(2 in [1, 2, 3], true);
assert_eq("a" in {a: 1}, true);
assert_eq("ell" in "hello", true);
assert_eq(5 in 1..10, true);
```

### 5.8 Ternary

```jolt
let label = age >= 18 ? "adult" : "minor";
```

Only the chosen branch is evaluated. It can be nested:

```jolt
let size = n > 100 ? "large" : n > 10 ? "medium" : "small";
```

### 5.9 Assignment

Assignment is an expression and returns the assigned value:

```jolt
var x = 0;
let y = (x = 5);
assert_eq(x, 5);
assert_eq(y, 5);

var a = 0;
var b = 0;
let c = (a = b = 3); // right-associative
```

Compound operators:

```jolt
x += 1;
x -= 1;
x *= 2;
x /= 2;
x %= 3;
x **= 2;
```

They also work on indexes and members:

```jolt
let data = [1, 2];
data[0] += 10;
data[0] *= 2;

let person = {age: 36};
person.age += 1;
person["age"] *= 2;
```

There are no `++` or `--`; use `+= 1` and `-= 1`.

---

## 6. Statements

### 6.1 Expression statements

An expression followed by `;` is evaluated and its result is discarded:

```jolt
1 + 2;
print("hello");
```

### 6.2 Blocks

```jolt
{
  let temporary = 1;
  print(temporary);
}
```

Blocks create a new scope. An empty `;` statement is also valid.

### 6.3 `if` / `else`

The condition goes in parentheses and the body **must** be in braces:

```jolt
if (age >= 18) {
  print("adult");
} else if (age >= 13) {
  print("teenager");
} else {
  print("minor");
}
```

The brace-less form (`if (x) print(x);`) is not supported.

### 6.4 `while`

```jolt
var i = 0;
while (i < 5) {
  print(i);
  i += 1;
}
```

### 6.5 Classic `for`

```jolt
for (var i = 0; i < 5; i += 1) {
  print(i);
}
```

Any of the three clauses may be omitted:

```jolt
for (;;) {
  break;
}
```

The variable declared in the initializer lives in a scope of its own for the loop. The body additionally creates a new scope on every iteration.

### 6.6 `for ... in`

Syntax, without parentheses:

```jolt
for value in [10, 20, 30] {
  print(value);
}
```

You can iterate over:

- **arrays**: in order, by value;
- **strings**: by code point, each one as a one-character string;
- **maps**: by key, in insertion order;
- **ranges**: ascending or descending depending on the bounds.

```jolt
for c in "abc" {
  print(c.upper());
}

for key in {a: 1, b: 2} {
  print(key);
}

for n in 1..=3 {
  print(n);
}
```

The iterable is evaluated only once. For arrays and maps, a snapshot of their elements or keys is taken when the loop starts. Ranges are iterated lazily: no intermediate array is created, so `for i in 0..1000000000 { ... }` runs in constant memory. The iteration variable gets a fresh binding on every turn, so closures created inside the body capture different values.

### 6.7 `break` and `continue`

```jolt
for n in 0..10 {
  if (n % 2 == 0) {
    continue;
  }
  if (n > 7) {
    break;
  }
  print(n);
}
```

They only affect the innermost loop. They cannot cross a function boundary: a `break` inside a function that is not in a loop is an error, even if the function is called from inside a loop.

### 6.8 `return`

```jolt
fn maximum(a, b) {
  if (a > b) {
    return a;
  }
  return b;
}
```

`return;` returns `nil`. Using `return` outside a function is an error.

### 6.9 Function declarations

```jolt
fn name(parameter1, parameter2) {
  // body
  return value;
}
```

They can be declared in the global scope or inside a block. They are hoisted within their block. See section 7.

---

## 7. Functions and closures

### 7.1 Declarations and calls

```jolt
fn add(a, b) {
  return a + b;
}

print(add(2, 3)); // 5
```

The number of arguments must match the number of parameters exactly. There are no default parameters or variadic arguments in user functions (native functions such as `print` are variadic).

Parameters are local. Arrays and maps are passed by reference; reassigning the parameter inside the function does not change the caller's variable.

```jolt
fn append(arr) {
  arr.push(1); // visible outside
}

fn replace(arr) {
  arr = [9];   // only changes the local parameter
}
```

### 7.2 Anonymous functions and lambdas

A function is a value:

```jolt
let double = fn(x) {
  return x * 2;
};

let triple = fn(x) => x * 3;

let anonymous = fn() {
  print("hello");
};
```

The `fn(params) => expression` form is sugar for a function that returns that expression.

They can also be passed as arguments:

```jolt
let squares = [1, 2, 3].map(fn(x) => x * x);
```

### 7.3 Self-named functions

A lambda can have a name so that it can call itself recursively:

```jolt
let power = fn power_of(base, exp) {
  if (exp == 0) {
    return 1;
  }
  return base * power_of(base, exp - 1);
};
```

The name only exists inside the function body, not in the outer scope.

### 7.4 Closures

Functions capture the environment where they are created and keep it alive:

```jolt
fn make_counter() {
  var n = 0;
  return fn() {
    n += 1;
    return n;
  };
}

let counter = make_counter();
print(counter()); // 1
print(counter()); // 2
```

Capture is by reference: if the captured variable changes after the function is created, the function sees the new value.

```jolt
fn example() {
  var state = 1;
  let read = fn() => state;
  state = 2;
  return read;
}

print(example()()); // 2
```

### 7.5 Recursion and depth

Recursion works both in declared functions and in named lambdas. The interpreter limits the call depth to **2000**; exceeding it produces a runtime error.

```jolt
fn factorial(n) {
  if (n <= 1) {
    return 1;
  }
  return n * factorial(n - 1);
}
```

---

## 8. Arrays

### 8.1 Literals and indexes

```jolt
let empty = [];
let numbers = [1, 2, 3,];
```

A trailing comma is allowed. Indexes are integers; negative ones count from the end:

```jolt
assert_eq(numbers[0], 1);
assert_eq(numbers[-1], 3);
```

An out-of-range index is a runtime error. Arrays are mutable:

```jolt
numbers[0] = 10;
numbers.push(4);
```

### 8.2 Members

`array.length` returns the number of elements. `len(array)` does the same.

### 8.3 Array methods

| Method | Description |
| --- | --- |
| `push(value, ...)` | Appends one or more values at the end and returns the array. |
| `pop()` | Removes and returns the last element; `nil` if empty. |
| `shift()` | Removes and returns the first element; `nil` if empty. |
| `unshift(value)` | Inserts a value at the start and returns the array. |
| `insert(index, value)` | Inserts at `index` (negatives allowed) and returns the array. |
| `remove_at(index)` | Removes and returns the element at `index`. |
| `slice(start?, end?)` | Returns a new array with the range `[start, end)`. |
| `concat(other)` | Returns a new array with both arrays. |
| `join(separator?)` | Joins the elements into a string. |
| `sort(comparator?)` | Sorts the array in place and returns it. |
| `sorted(comparator?)` | Returns a sorted copy. |
| `reversed()` | Returns a reversed copy. |
| `contains(value)` | Checks structural equality. |
| `index_of(value)` | Returns the first index or `-1`. |
| `map(fn)` | Returns an array with the results of `fn(element)`. |
| `filter(fn)` | Returns the elements for which `fn` is truthy. |
| `reduce(fn, initial?)` | Accumulates with `fn(accumulator, element)`. |
| `each(fn)` | Calls `fn(element)` for each element and returns the array. |
| `find(fn)` | Returns the first element that satisfies `fn`, or `nil`. |
| `any(fn)` | `true` if any element satisfies `fn`. |
| `all(fn)` | `true` if all elements satisfy `fn`. |
| `sum()` | Sum of an array of numbers. |
| `clear()` | Empties the array and returns it. |
| `clone()` | Shallow copy. |

Details:

- `slice` clamps the indexes to the size of the array and accepts negatives. If `end < start`, it returns an empty array.
- `sort` and `sorted` use ascending order by default. The comparator receives `(a, b)` and must return `true` if `a` should come before `b`; the sort is stable.
- `reduce` without an initial value uses the first element as the accumulator and fails if the array is empty.
- `sum` requires all elements to be numbers; it returns `int` if all are integers and `float` otherwise.
- `clone` is shallow: nested arrays and maps are shared.

```jolt
let nums = [3, 1, 2];
assert_eq(nums.sorted(), [1, 2, 3]);
assert_eq(nums.sorted(fn(a, b) => a > b), [3, 2, 1]);
assert_eq(nums.map(fn(n) => n * 2), [6, 2, 4]);
assert_eq(nums.reduce(fn(a, n) => a + n, 0), 6);
```

---

## 9. Strings

Strings are immutable and indexed by code point:

```jolt
let s = "áé";
assert_eq(s.length, 2);
assert_eq(s[0], "á");
assert_eq(s[-1], "é");
```

An out-of-range index is an error. You cannot assign to a string index.

There is no slicing syntax (`s[1:3]`); use `substr`, `split`, etc.

### 9.1 String methods

| Method | Description |
| --- | --- |
| `upper()` | Converts ASCII letters to uppercase. |
| `lower()` | Converts ASCII letters to lowercase. |
| `trim()` | Removes ASCII whitespace at the start and end. |
| `trim_start()` | Removes ASCII whitespace at the start. |
| `trim_end()` | Removes ASCII whitespace at the end. |
| `split(separator?)` | With no argument splits on whitespace; with `""` splits into characters. |
| `lines()` | Splits on `\n` line breaks. |
| `replace(search, replacement)` | Replaces all non-overlapping occurrences. |
| `replace_first(search, replacement)` | Replaces the first occurrence. |
| `starts_with(prefix)` | Checks the prefix. |
| `ends_with(suffix)` | Checks the suffix. |
| `contains(text)` | Checks whether it contains the substring. |
| `index_of(text)` | Code-point index of the first occurrence, or `-1`. |
| `substr(start, length?)` | Substring from `start` (negatives allowed). |
| `chars()` | Array of one-character strings. |
| `repeat(n)` | Repeats the string `n` times. |
| `reversed()` | Reverses the code points. |
| `pad_start(width, fill?)` | Pads on the left up to `width`. |
| `pad_end(width, fill?)` | Pads on the right up to `width`. |

Details:

- `upper` and `lower` only transform `A-Z` and `a-z`; the rest of Unicode is unchanged.
- `trim*` considers ASCII whitespace (`std::isspace`), not every Unicode space.
- `lines` does not add an empty trailing line if the string ends with `\n`.
- `index_of` searches bytes but returns the index in code points.
- `substr` counts code points; a negative `length` produces an empty string.
- `pad_start` and `pad_end` pad by code points and trim the fill to the exact width.
- `repeat` has a 64 MB limit on the result.

```jolt
assert_eq("  hello  ".trim(), "hello");
assert_eq("a,b,c".split(","), ["a", "b", "c"]);
assert_eq("abc".substr(1, 2), "bc");
assert_eq("5".pad_start(3, "0"), "005");
```

---

## 10. Maps

### 10.1 Literals

Keys in a literal can be identifiers, string literals, scalar literals (`int`, `true`, `false`, `nil`) or computed expressions in brackets:

```jolt
let person = {
  name: "Ada",
  age: 36,
  "key with spaces": true,
};

// Scalar literals are canonicalized to their string form.
let coded = {1: "one", true: "yes", nil: "nothing"};
assert_eq(coded["1"], "one");

// Computed keys use the [expr] syntax and are evaluated at runtime.
let n = 2;
let dyn = {[n * 10]: "twenty", ["a" + n]: "a2"};
assert_eq(dyn["20"], "twenty");
```

All map keys are strings internally: scalar and computed keys are normalized with the value's canonical string form (for example `1` becomes `"1"`), so lookups always use strings. If a key is repeated, the last value wins, but the position of its first appearance is kept.

### 10.2 Access and assignment

```jolt
assert_eq(person.name, "Ada");
assert_eq(person["age"], 36);

person.age = 37;
person["city"] = "London";
```

Accessing a missing key returns `nil` (both with `.` and with `[]`). Keys must be strings; using another type as an index is an error.

Maps preserve insertion order. Reassigning an existing key does not move it.

### 10.3 Members and collisions with methods

`map.length` returns the number of keys, unless a `"length"` key exists, in which case its value is returned. Likewise, a key named like a method (for example `"keys"`) takes priority over the method.

### 10.4 Map methods

| Method | Description |
| --- | --- |
| `keys()` | Array of keys in insertion order. |
| `values()` | Array of values in insertion order. |
| `entries()` | Array of `[key, value]` pairs. |
| `has(key)` | `true` if the key exists. |
| `get(key, default?)` | Value of the key, or `default`/`nil`. |
| `set(key, value)` | Assigns and returns the map. |
| `delete(key)` | Removes the key; returns `true` if it existed. |
| `clear()` | Empties the map and returns it. |
| `merge(other)` | Copies the keys of `other` into this map and returns it. |
| `clone()` | Shallow copy. |

```jolt
let m = {a: 1};
m.set("b", 2);
assert_eq(m.keys(), ["a", "b"]);
assert_eq(m.entries(), [["a", 1], ["b", 2]]);
assert_eq(m.get("z", 0), 0);
assert_eq(m.delete("a"), true);
assert_eq(m, {b: 2});
```

Map equality does not depend on order: two maps with the same keys and values are equal.

Iterating a map produces its keys:

```jolt
for key in {a: 1, b: 2} {
  print(key);
}
```

---

## 11. Ranges

### 11.1 Syntax

| Expression | Meaning |
| --- | --- |
| `a..b` | From `a` up to `b`, excluding `b`. |
| `a..=b` | From `a` up to `b`, including `b`. |

The bounds must be integers. A range can be ascending or descending:

```jolt
assert_eq((1..4).to_array(), [1, 2, 3]);
assert_eq((1..=3).to_array(), [1, 2, 3]);
assert_eq((3..0).to_array(), [3, 2, 1]);
assert_eq((3..=0).to_array(), [3, 2, 1, 0]);
```

`5..5` is empty; `5..=5` contains one element.

### 11.2 Operations

- `.length` / `len(range)`: number of elements.
- `.to_array()`: converts to an array (limit of 10,000,000 elements).
- `.contains(value)`: same as `value in range`.
- Equality compares start, end and inclusiveness: `1..3 != 1..=3`.

The `range(end)` function produces `0..end`, and `range(start, end)` produces `start..end` (always exclusive).

Range values are lightweight: they only store the bounds and the inclusiveness flag. Iterating a range with `for ... in` is lazy (constant memory); operations that need all elements at once, such as `to_array()` or passing the range to `map`/`filter`-style builtins, materialize them and are capped at 10,000,000 elements.

```jolt
assert_eq(2 in 1..5, true);
assert_eq(5 in 1..5, false);
assert_eq(5 in 1..=5, true);
assert_eq(len(5..0), 5);
```

---

## 12. Standard library

### 12.1 Console and input

| Function | Description |
| --- | --- |
| `print(value, ...)` | Prints the values separated by spaces and appends `\n`. |
| `write(value, ...)` | Like `print`, but without the trailing newline. |
| `eprint(value, ...)` | Like `print`, but to the error output. |
| `input(prompt?)` | Reads a line; returns `nil` at the end of the input. |

### 12.2 Types and conversion

| Function | Description |
| --- | --- |
| `type(value)` | Name of the type as a string. |
| `str(value)` | Text representation. |
| `repr(value)` | Representation with strings in quotes. |
| `int(value)` | Converts to integer (truncates floats). |
| `float(value)` | Converts to float. |
| `bool(value)` | Applies Jolt's truthiness. |

`int(string)` accepts the `strtoll` format with base 0 (decimal, `0x...`, `0...` octal) and trims whitespace. `float(string)` uses `strtod`. Converting an empty or badly formatted string is an error.

`str` of an array or map uses the representation of its elements, with inner strings in quotes:

```jolt
assert_eq(str(42), "42");
assert_eq(str([1, "a"]), "[1, \"a\"]");
assert_eq(repr("a"), "\"a\"");
```

### 12.3 Math

| Function | Description |
| --- | --- |
| `abs(n)` | Absolute value. |
| `floor(n)` | Rounds down. |
| `ceil(n)` | Rounds up. |
| `round(n)` | Rounds to the nearest integer (halves away from zero). |
| `sqrt(n)` | Square root; error if `n < 0`. |
| `pow(base, exp)` | Power as `float`. |
| `min(a, ...)` | Minimum of at least one argument. |
| `max(a, ...)` | Maximum of at least one argument. |
| `clamp(n, minimum, maximum)` | Limits `n` to the interval. |

`floor`, `ceil` and `round` return the same `int` if they receive an `int`, and a `float` if they receive a `float`.

### 12.4 Randomness and time

| Function | Description |
| --- | --- |
| `random()` | `float` in `[0, 1)`. |
| `random(high)` | `int` in `[0, high)`. |
| `random(low, high)` | `int` in `[low, high)`. |
| `seed(n)` | Reseeds the generator. |
| `clock()` | Seconds elapsed since the process started. |

### 12.5 Utilities

| Function | Description |
| --- | --- |
| `len(value)` | Length of a string, array, map or range. |
| `chr(code)` | Unicode character from a code point. |
| `ord(character)` | Code point of a one-character string. |
| `range(end)` | Range `0..end`. |
| `range(start, end)` | Range `start..end`. |
| `args()` | Script arguments as an array of strings. |

### 12.6 Files

| Function | Description |
| --- | --- |
| `read_file(path)` | Returns the contents of a file as a string. |
| `write_file(path, contents)` | Writes the contents (exact binary). |
| `file_exists(path)` | Checks whether the file can be opened. |

### 12.7 Assertions and control

| Function | Description |
| --- | --- |
| `assert(condition, message?)` | Fails if the condition is falsy. |
| `assert_eq(expected, actual, message?)` | Fails if they are not equal. |
| `panic(message?)` | Raises a runtime error. |
| `exit(code?)` | Ends the process with the given code (0 by default). |

`assert_eq` follows the order `expected, actual` and adds both values to the error message.

---

## 13. Errors and diagnostics

Jolt distinguishes two kinds of errors:

- **Compile error**: detected by the lexer or the parser (for example, a missing `;` or a malformed literal).
- **Runtime error**: detected by the interpreter (for example, an undefined variable, a division by zero or an out-of-range index).

Both are shown with the location and a code snippet:

```text
script.jolt:3:9: runtime error: division by zero
    |
  3 | let x = 1 / 0;
    |         ^
```

Runtime errors also include a stack trace with the active functions.

Process exit codes:

| Code | Meaning |
| --- | --- |
| `0` | Success. |
| `64` | Incorrect command-line usage. |
| `65` | Compile error. |
| `66` | The file could not be read. |
| `70` | Runtime error. |
| other | Code given to `exit(n)`. |

`jolt -c file.jolt` only checks the lexer and the parser; it does not run the program.

---

## 14. REPL

Running `jolt` with no arguments starts the REPL. Features:

- Expressions are evaluated and shown with `repr`; `nil` is not printed.
- `_` holds the last evaluated value.
- The `...>` prompt means continuation; a blank line cancels the incomplete input.
- The trailing `;` of each entry may be omitted.
- Definitions persist until `:clear`.

Commands:

| Command | Description |
| --- | --- |
| `:help`, `:?` | Help. |
| `:quit`, `:q`, `:exit` | Quit. |
| `:clear` | Delete the user definitions. |
| `:load <file>` | Run a script in the current session. |
| `:vars` | List the user's global variables. |

Example:

```text
jolt> let x = 10
jolt> let double = fn(n) => n * 2
jolt> double(x)
20
jolt> _
20
jolt> :vars
  _: int
  double: function
  x: int
```

---

## 15. Command line

```text
jolt                      starts the REPL
jolt <file.jolt>          runs a script
jolt -e "<code>"          runs code from the command line
jolt -c <file.jolt>       checks the syntax without running
```

| Option | Description |
| --- | --- |
| `-h`, `--help` | Help. |
| `-v`, `--version` | Version. |
| `-e`, `--eval <code>` | Code to run. The remaining arguments go to the script. |
| `-c`, `--check` | Syntax check only. |
| `--` | End of options; the next argument is the file. |

Options must come before the file. Everything after the file is considered an argument of the script and is obtained with `args()`.

```sh
jolt program.jolt one two
```

```jolt
for argument in args() {
  print(argument);
}
```

---

## 16. Known limitations

- Dynamic typing. The `jolt` executable currently runs programs by walking the AST; the bytecode compiler and the VM do not cover the whole language yet (PHASE 6). There is no JIT.
- No classes, structs, interfaces or inheritance.
- No `try`/`catch` or exceptions that can be caught from the language.
- No modules or `import`. In the REPL you can use `:load`.
- No `switch`, `do`/`while`, `++`, `--`, default parameters or variadic arguments in user functions.
- Map keys are always strings. Literals accept identifier, string, scalar (`int`, `true`, `false`, `nil`) and computed `[expr]` keys; non-string keys are canonicalized to their string form.
- The bodies of `if`, `while` and `for` require braces.
- `for ... in` does not accept parentheses around the variable and the iterable.
- No slicing syntax for strings or arrays; methods such as `substr` and `slice` are used.
- `upper` and `lower` only work with ASCII.
- Maximum call depth: 2000.
- Memory is managed with reference counting; cyclic references are not freed.
- Sorting with custom comparators relies on the comparator defining a strict order.

---

## 17. Complete example

```jolt
// Counts words and shows the most frequent ones.

fn count_words(text) {
  let counts = {};
  for word in text.lower().split() {
    counts[word] = counts.get(word, 0) + 1;
  }
  return counts;
}

let text = "one two two three three three four four four four";
let counts = count_words(text);

let pairs = counts.entries();
pairs.sort(fn(a, b) => a[1] > b[1]);

for pair in pairs {
  print("${pair[0]}: ${pair[1]}");
}
```

Output:

```text
four: 4
three: 3
two: 2
one: 1
```
