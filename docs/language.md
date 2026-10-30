# Referencia del lenguaje Jolt

Versión del lenguaje: **0.1.0**

Esta guía describe la sintaxis y la semántica de Jolt tal como las implementa el intérprete incluido en este repositorio.

## Índice

1. [Modelo de ejecución](#1-modelo-de-ejecución)
2. [Estructura léxica](#2-estructura-léxica)
3. [Tipos de datos](#3-tipos-de-datos)
4. [Variables y ámbito](#4-variables-y-ámbito)
5. [Operadores](#5-operadores)
6. [Sentencias](#6-sentencias)
7. [Funciones y closures](#7-funciones-y-closures)
8. [Arrays](#8-arrays)
9. [Cadenas](#9-cadenas)
10. [Mapas](#10-mapas)
11. [Rangos](#11-rangos)
12. [Biblioteca estándar](#12-biblioteca-estándar)
13. [Errores y diagnóstico](#13-errores-y-diagnóstico)
14. [REPL](#14-repl)
15. [Línea de comandos](#15-línea-de-comandos)
16. [Limitaciones conocidas](#16-limitaciones-conocidas)
17. [Ejemplo completo](#17-ejemplo-completo)

---

## 1. Modelo de ejecución

Un programa Jolt es un archivo de texto UTF-8 (extensión `.jolt`). El intérprete lo procesa en tres fases:

1. **Lexer**: convierte el texto en tokens.
2. **Parser**: construye un AST con un parser recursivo y precedencia de operadores.
3. **Intérprete**: recorre el AST y evalúa las sentencias en un entorno léxico.

El programa se ejecuta de arriba abajo en el ámbito global. Las sentencias simples terminan en `;`. Las sentencias con bloque (`if`, `while`, `for`, `fn`) cierran con `}` y aceptan opcionalmente un `;` después.

```jolt
let a = 1;
if (a == 1) {
  print("uno");
}; // el ';' es opcional después del bloque
```

Un archivo puede empezar con una línea `#!...` (shebang); el lexer la ignora.

---

## 2. Estructura léxica

### 2.1 Comentarios

```jolt
// Comentario de línea

/* Comentario de bloque.
   /* Los comentarios de bloque se pueden anidar. */
*/
```

`//` siempre inicia un comentario; no existe un operador de división entera.

### 2.2 Identificadores y palabras reservadas

Los identificadores empiezan por una letra ASCII o `_`, y continúan con letras ASCII, dígitos o `_`. Distinguen mayúsculas y minúsculas.

Palabras reservadas:

```text
break  const  continue  else  false  fn  for  if  in  let
nil    return true      while
```

No se pueden usar como nombres de variables, parámetros ni claves de mapa sin comillas.

### 2.3 Números

| Forma | Ejemplo | Valor |
| --- | --- | --- |
| Decimal | `123` | `123` |
| Separadores | `1_000_000` | `1000000` |
| Hexadecimal | `0x2A` | `42` |
| Binario | `0b101010` | `42` |
| Octal | `0o52` | `42` |
| Flotante | `1.5`, `0.25` | `1.5`, `0.25` |
| Notación científica | `1.5e3`, `2E-2` | `1500.0`, `0.02` |

- Los enteros son de 64 bits con signo. Un literal fuera de rango es un error de compilación.
- El signo `-` es un operador unario, no parte del literal.
- Un punto solo forma parte de un flotante si va seguido de un dígito: `1.` no es válido como flotante; escribe `1.0`.
- Los guiones bajos se ignoran dentro de los dígitos: `1_0` equivale a `10`.

### 2.4 Cadenas

Las cadenas se escriben entre comillas dobles. No hay comillas simples ni cadenas crudas. Pueden ocupar varias líneas:

```jolt
let saludo = "Hola";
let texto = "primera línea
segunda línea";
```

Secuencias de escape:

| Escape | Significado |
| --- | --- |
| `\n` | Nueva línea |
| `\t` | Tabulador |
| `\r` | Retorno de carro |
| `\0` | Byte nulo |
| `\\` | Barra invertida |
| `\"` | Comilla doble |
| `\$` | Signo de dólar literal |
| `\xNN` | Byte hexadecimal (dos dígitos) |
| `\uXXXX` | Punto de código Unicode (cuatro dígitos) |

### 2.5 Interpolación

Dentro de una cadena, `${expresión}` evalúa una expresión y la convierte a texto:

```jolt
let nombre = "Ada";
let edad = 36;
print("${nombre} tiene ${edad} años");
print("2 + 3 = ${2 + 3}");
print("resultado: ${ {a: 1}.a }");
```

Las expresiones interpoladas pueden contener cadenas y llaves anidadas:

```jolt
print("anidado: ${"interior ${1 + 1}"}");
```

Para escribir literalmente `${`, escapa el dólar:

```jolt
print("\${no evaluado}"); // ${no evaluado}
```

Las cadenas son UTF-8 y **inmutables**. Las operaciones de longitud e índice trabajan con puntos de código, no con bytes.

---

## 3. Tipos de datos

| Tipo | Valores | Notas |
| --- | --- | --- |
| `nil` | `nil` | Ausencia de valor. |
| `bool` | `true`, `false` | Valores lógicos. |
| `int` | Enteros de 64 bits con signo. | Aritmética con desbordamiento envolvente. |
| `float` | Números de coma flotante IEEE 754 de doble precisión. | Puede ser `inf`, `-inf` o `nan` como resultado de operaciones. |
| `string` | Texto UTF-8. | Inmutable. |
| `array` | Secuencia ordenada de valores. | Mutable y por referencia. |
| `map` | Colección de claves de cadena. | Conserva el orden de inserción. Mutable y por referencia. |
| `function` | Funciones nativas o definidas por el usuario. | Ciudadanas de primera clase. |
| `range` | Intervalo de enteros. | Inmutable. |

La función `type(valor)` devuelve el nombre del tipo como cadena.

### 3.1 Verdad

Solo dos valores son falsos:

```jolt
false
nil
```

Todo lo demás es verdadero, incluidos `0`, `0.0`, `""`, `[]` y `{}`.

```jolt
if (0) {
  print("0 es verdadero en Jolt");
}
```

### 3.2 Igualdad

- `int` y `float` se comparan numéricamente: `1 == 1.0` es `true`.
- Las cadenas se comparan por contenido.
- Los arrays y mapas se comparan **estructuralmente**, en profundidad.
- Los rangos comparan inicio, fin e inclusividad.
- Las funciones se comparan por identidad (la misma función).
- Valores de tipos distintos (salvo `int`/`float`) no son iguales: `0 == false` es `false`.

### 3.3 Mutabilidad y referencias

Asignar un array o un mapa a otra variable **no lo copia**: ambas variables apuntan al mismo valor.

```jolt
let a = [1];
let b = a;
b.push(2);
print(a); // [1, 2]
```

Para una copia superficial usa `.clone()`. Las cadenas son inmutables, por lo que se comportan como valores.

---

## 4. Variables y ámbito

### 4.1 Declaración

```jolt
let contador;          // se inicializa a nil
let nombre = "Jolt";
const LIMITE = 10;     // requiere un valor
```

`let` declara una variable reasignable. `const` declara una constante: asignarle otro valor es un error en tiempo de ejecución.

### 4.2 Ámbito de bloque

Cada bloque `{ ... }` crea un ámbito nuevo. Las variables se resuelven desde el ámbito actual hacia los padres.

```jolt
let x = 1;
{
  let x = 2;   // sombrea la x exterior
  print(x);    // 2
}
print(x);      // 1
```

La redeclaración de un nombre en el mismo bloque sustituye la definición anterior.

### 4.3 Asignación

La asignación busca la variable en el ámbito actual y, si no existe, en los ámbitos padres. No crea variables nuevas:

```jolt
let x = 1;
{
  x = 2; // modifica la x exterior
}
print(x); // 2

y = 3; // error: undefined variable 'y'
```

### 4.4 Declaraciones de función

Las declaraciones `fn nombre(...) { ... }` se elevan (*hoisting*) dentro de su bloque, de modo que se pueden llamar antes de la línea donde aparecen. Esto permite la recursión y la recursión mutua.

Las declaraciones `let` y `const` **no** se elevan: usar una variable antes de su declaración produce un error.

---

## 5. Operadores

### 5.1 Precedencia

De mayor a menor precedencia:

| Precedencia | Operadores | Asociatividad |
| --- | --- | --- |
| 1 | `()` `[]` `.` | izquierda |
| 2 | `**` | derecha |
| 3 | `!` `-` `+` `~` (unarios) | derecha |
| 4 | `*` `/` `%` | izquierda |
| 5 | `+` `-` | izquierda |
| 6 | `..` `..=` | izquierda |
| 7 | `<<` `>>` | izquierda |
| 8 | `<` `<=` `>` `>=` `in` | izquierda |
| 9 | `==` `!=` | izquierda |
| 10 | `&` | izquierda |
| 11 | `^` | izquierda |
| 12 | `\|` | izquierda |
| 13 | `&&` | izquierda |
| 14 | `\|\|` | izquierda |
| 15 | `?:` | derecha |
| 16 | `=` `+=` `-=` `*=` `/=` `%=` `**=` | derecha |

Observaciones:

- `**` se asocia a la derecha: `2 ** 3 ** 2` es `2 ** (3 ** 2) = 512`.
- El menos unario tiene menos precedencia que `**`: `-2 ** 2` es `-(2 ** 2) = -4`.
- Los rangos tienen más precedencia que los desplazamientos y las comparaciones: `2 in 1..5` equivale a `2 in (1..5)`.
- Las comparaciones no se encadenan: `1 < 2 < 3` intenta comparar un booleano con un entero y produce un error.

### 5.2 Aritméticos

| Operador | Descripción |
| --- | --- |
| `+` | Suma de números, concatenación de cadenas o de arrays. |
| `-` | Resta. |
| `*` | Multiplicación. |
| `/` | División. |
| `%` | Resto. |
| `**` | Potencia. |

Reglas:

- Si ambos operandos son `int`, `+ - * / %` producen `int`.
- La división entera trunca hacia cero: `7 / 2 == 3`, `-7 / 2 == -3`.
- El resto entero sigue el signo del dividendo: `-7 % 3 == -1`.
- Si al menos un operando es `float`, el resultado es `float`.
- Dividir o calcular el resto con divisor cero es un error en tiempo de ejecución, también para flotantes.
- `**` devuelve `int` si base y exponente son enteros y el exponente no es negativo; en caso contrario usa `std::pow` y devuelve `float`.
- El desbordamiento de `int` es envolvente (complemento a dos).
- `+` concatena dos cadenas y también dos arrays (creando un array nuevo).

```jolt
assert_eq(7 / 2, 3);
assert_eq(7 / 2.0, 3.5);
assert_eq(2 ** 10, 1024);
assert_eq("a" + "b", "ab");
assert_eq([1] + [2], [1, 2]);
```

### 5.3 Comparación

`<`, `<=`, `>`, `>=` funcionan con:

- dos números (`int` o `float` mezclados);
- dos cadenas (orden lexicográfico por bytes UTF-8);
- dos booleanos (`false < true`).

Comparar tipos incompatibles es un error.

### 5.4 Igualdad

`==` y `!=` usan la igualdad estructural descrita en la sección 3.2.

### 5.5 Lógicos

| Operador | Descripción |
| --- | --- |
| `!` | Negación lógica; siempre devuelve `bool`. |
| `&&` | Y lógico con cortocircuito. |
| `\|\|` | O lógico con cortocircuito. |

`&&` y `||` **devuelven uno de sus operandos**, no un booleano:

```jolt
assert_eq(nil || "por defecto", "por defecto");
assert_eq("valor" || "por defecto", "valor");
assert_eq(false && 123, false);
assert_eq(true && 123, 123);
assert_eq(nil && 123, nil);
```

El operando derecho solo se evalúa si es necesario.

### 5.6 Bit a bit

| Operador | Descripción |
| --- | --- |
| `&` | Y bit a bit. |
| `\|` | O bit a bit. |
| `^` | O exclusivo bit a bit. |
| `~` | Complemento a uno (unario). |
| `<<` | Desplazamiento a la izquierda. |
| `>>` | Desplazamiento a la derecha con signo. |

Solo aceptan `int`. El desplazamiento requiere un contador entre 0 y 63.

```jolt
assert_eq(6 & 3, 2);
assert_eq(6 | 3, 7);
assert_eq(1 << 4, 16);
assert_eq(256 >> 3, 32);
assert_eq(~0, -1);
```

### 5.7 Pertenencia

El operador `in` comprueba pertenencia:

| Derecha | Comportamiento |
| --- | --- |
| array | Igualdad estructural con algún elemento. |
| mapa | El izquierdo debe ser cadena; comprueba si existe la clave. |
| cadena | El izquierdo debe ser cadena; comprueba si es una subcadena. |
| rango | El izquierdo debe ser entero; comprueba si está dentro del rango. |

```jolt
assert_eq(2 in [1, 2, 3], true);
assert_eq("a" in {a: 1}, true);
assert_eq("ell" in "hello", true);
assert_eq(5 in 1..10, true);
```

### 5.8 Ternario

```jolt
let etiqueta = edad >= 18 ? "adulto" : "menor";
```

Solo se evalúa la rama elegida. Se puede anidar:

```jolt
let tamano = n > 100 ? "grande" : n > 10 ? "mediano" : "pequeño";
```

### 5.9 Asignación

La asignación es una expresión y devuelve el valor asignado:

```jolt
let x = 0;
let y = (x = 5);
assert_eq(x, 5);
assert_eq(y, 5);

let a = 0;
let b = 0;
let c = (a = b = 3); // asociativa a la derecha
```

Operadores compuestos:

```jolt
x += 1;
x -= 1;
x *= 2;
x /= 2;
x %= 3;
x **= 2;
```

También funcionan sobre índices y miembros:

```jolt
let datos = [1, 2];
datos[0] += 10;
datos[0] *= 2;

let persona = {edad: 36};
persona.edad += 1;
persona["edad"] *= 2;
```

No existen `++` ni `--`; usa `+= 1` y `-= 1`.

---

## 6. Sentencias

### 6.1 Sentencias de expresión

Una expresión seguida de `;` se evalúa y su resultado se descarta:

```jolt
1 + 2;
print("hola");
```

### 6.2 Bloques

```jolt
{
  let temporal = 1;
  print(temporal);
}
```

Los bloques crean un ámbito nuevo. Una sentencia `;` vacía también es válida.

### 6.3 `if` / `else`

La condición va entre paréntesis y el cuerpo **debe** ir entre llaves:

```jolt
if (edad >= 18) {
  print("adulto");
} else if (edad >= 13) {
  print("adolescente");
} else {
  print("menor");
}
```

No se admite la forma sin llaves (`if (x) print(x);`).

### 6.4 `while`

```jolt
let i = 0;
while (i < 5) {
  print(i);
  i += 1;
}
```

### 6.5 `for` clásico

```jolt
for (let i = 0; i < 5; i += 1) {
  print(i);
}
```

Cualquiera de las tres cláusulas puede omitirse:

```jolt
for (;;) {
  break;
}
```

La variable declarada en el inicializador vive en un ámbito propio del bucle. El cuerpo crea además un ámbito nuevo en cada iteración.

### 6.6 `for ... in`

Sintaxis, sin paréntesis:

```jolt
for valor in [10, 20, 30] {
  print(valor);
}
```

Se puede iterar sobre:

- **arrays**: en orden, por valor;
- **cadenas**: por puntos de código, cada uno como cadena de un carácter;
- **mapas**: por claves, en orden de inserción;
- **rangos**: ascendente o descendente según los límites.

```jolt
for c in "abc" {
  print(c.upper());
}

for clave in {a: 1, b: 2} {
  print(clave);
}

for n in 1..=3 {
  print(n);
}
```

El iterable se evalúa una sola vez. Para arrays y mapas se toma una instantánea de sus elementos o claves al comenzar el bucle. La variable de iteración tiene un enlace nuevo en cada vuelta, por lo que los closures creados dentro del cuerpo capturan valores distintos.

### 6.7 `break` y `continue`

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

Afectan solo al bucle más interno. No pueden cruzar el límite de una función: un `break` dentro de una función que no está en un bucle es un error, aunque la función se llame desde dentro de un bucle.

### 6.8 `return`

```jolt
fn maximo(a, b) {
  if (a > b) {
    return a;
  }
  return b;
}
```

`return;` devuelve `nil`. Usar `return` fuera de una función es un error.

### 6.9 Declaraciones de función

```jolt
fn nombre(parametro1, parametro2) {
  // cuerpo
  return valor;
}
```

Se pueden declarar en el ámbito global o dentro de un bloque. Se elevan dentro de su bloque. Consulta la sección 7.

---

## 7. Funciones y closures

### 7.1 Declaraciones y llamadas

```jolt
fn sumar(a, b) {
  return a + b;
}

print(sumar(2, 3)); // 5
```

El número de argumentos debe coincidir exactamente con el de parámetros. No hay parámetros por defecto ni argumentos variables en funciones de usuario (las funciones nativas como `print` sí son variádicas).

Los parámetros son locales. Los arrays y mapas se pasan por referencia; reasignar el parámetro dentro de la función no cambia la variable del llamador.

```jolt
fn agregar(arr) {
  arr.push(1); // visible fuera
}

fn reemplazar(arr) {
  arr = [9];   // solo cambia el parámetro local
}
```

### 7.2 Funciones anónimas y lambdas

Una función es un valor:

```jolt
let doble = fn(x) {
  return x * 2;
};

let triple = fn(x) => x * 3;

let sin_nombre = fn() {
  print("hola");
};
```

La forma `fn(params) => expresión` es azúcar para una función que devuelve esa expresión.

También se pueden pasar como argumentos:

```jolt
let cuadrados = [1, 2, 3].map(fn(x) => x * x);
```

### 7.3 Funciones con nombre propio

Una lambda puede tener nombre para llamarse recursivamente:

```jolt
let potencia = fn potencia_de(base, exp) {
  if (exp == 0) {
    return 1;
  }
  return base * potencia_de(base, exp - 1);
};
```

El nombre solo existe dentro del cuerpo de la función, no en el ámbito exterior.

### 7.4 Closures

Las funciones capturan el entorno donde se crean y lo mantienen vivo:

```jolt
fn crear_contador() {
  let n = 0;
  return fn() {
    n += 1;
    return n;
  };
}

let contador = crear_contador();
print(contador()); // 1
print(contador()); // 2
```

La captura es por referencia: si la variable capturada cambia después de crear la función, la función ve el valor nuevo.

```jolt
fn ejemplo() {
  let estado = 1;
  let leer = fn() => estado;
  estado = 2;
  return leer;
}

print(ejemplo()()); // 2
```

### 7.5 Recursión y profundidad

La recursión funciona tanto en funciones declaradas como en lambdas con nombre. El intérprete limita la profundidad de llamadas a **2000**; superarla produce un error de tiempo de ejecución.

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

### 8.1 Literales e índices

```jolt
let vacio = [];
let numeros = [1, 2, 3,];
```

Se permite una coma final. Los índices son enteros; los negativos cuentan desde el final:

```jolt
assert_eq(numeros[0], 1);
assert_eq(numeros[-1], 3);
```

Un índice fuera de rango es un error de tiempo de ejecución. Los arrays son mutables:

```jolt
numeros[0] = 10;
numeros.push(4);
```

### 8.2 Miembros

`array.length` devuelve el número de elementos. `len(array)` hace lo mismo.

### 8.3 Métodos de array

| Método | Descripción |
| --- | --- |
| `push(valor, ...)` | Añade uno o más valores al final y devuelve el array. |
| `pop()` | Quita y devuelve el último elemento; `nil` si está vacío. |
| `shift()` | Quita y devuelve el primer elemento; `nil` si está vacío. |
| `unshift(valor)` | Inserta un valor al principio y devuelve el array. |
| `insert(indice, valor)` | Inserta en `indice` (admite negativos) y devuelve el array. |
| `remove_at(indice)` | Quita y devuelve el elemento en `indice`. |
| `slice(inicio?, fin?)` | Devuelve un array nuevo con el rango `[inicio, fin)`. |
| `concat(otro)` | Devuelve un array nuevo con ambos arrays. |
| `join(separador?)` | Une los elementos en una cadena. |
| `sort(comparador?)` | Ordena el array en su lugar y lo devuelve. |
| `sorted(comparador?)` | Devuelve una copia ordenada. |
| `reversed()` | Devuelve una copia invertida. |
| `contains(valor)` | Comprueba igualdad estructural. |
| `index_of(valor)` | Devuelve el primer índice o `-1`. |
| `map(fn)` | Devuelve un array con los resultados de `fn(elemento)`. |
| `filter(fn)` | Devuelve los elementos para los que `fn` es verdadero. |
| `reduce(fn, inicial?)` | Acumula con `fn(acumulador, elemento)`. |
| `each(fn)` | Llama a `fn(elemento)` por cada elemento y devuelve el array. |
| `find(fn)` | Devuelve el primer elemento que cumple `fn`, o `nil`. |
| `any(fn)` | `true` si algún elemento cumple `fn`. |
| `all(fn)` | `true` si todos los elementos cumplen `fn`. |
| `sum()` | Suma de un array de números. |
| `clear()` | Vacía el array y lo devuelve. |
| `clone()` | Copia superficial. |

Detalles:

- `slice` ajusta los índices al tamaño del array y admite negativos. Si `fin < inicio`, devuelve un array vacío.
- `sort` y `sorted` usan orden ascendente por defecto. El comparador recibe `(a, b)` y debe devolver `true` si `a` debe ir antes que `b`; la ordenación es estable.
- `reduce` sin valor inicial usa el primer elemento como acumulador y falla si el array está vacío.
- `sum` exige que todos los elementos sean números; devuelve `int` si todos son enteros y `float` en caso contrario.
- `clone` es superficial: los arrays y mapas anidados se comparten.

```jolt
let nums = [3, 1, 2];
assert_eq(nums.sorted(), [1, 2, 3]);
assert_eq(nums.sorted(fn(a, b) => a > b), [3, 2, 1]);
assert_eq(nums.map(fn(n) => n * 2), [6, 2, 4]);
assert_eq(nums.reduce(fn(a, n) => a + n, 0), 6);
```

---

## 9. Cadenas

Las cadenas son inmutables y se indexan por puntos de código:

```jolt
let s = "áé";
assert_eq(s.length, 2);
assert_eq(s[0], "á");
assert_eq(s[-1], "é");
```

Un índice fuera de rango es un error. No se puede asignar a un índice de cadena.

No hay sintaxis de rebanado (`s[1:3]`); usa `substr`, `split`, etc.

### 9.1 Métodos de cadena

| Método | Descripción |
| --- | --- |
| `upper()` | Convierte letras ASCII a mayúsculas. |
| `lower()` | Convierte letras ASCII a minúsculas. |
| `trim()` | Quita espacios ASCII al inicio y al final. |
| `trim_start()` | Quita espacios ASCII al inicio. |
| `trim_end()` | Quita espacios ASCII al final. |
| `split(separador?)` | Sin argumento divide por espacios; con `""` divide en caracteres. |
| `lines()` | Divide por saltos de línea `\n`. |
| `replace(buscar, reemplazo)` | Reemplaza todas las apariciones no solapadas. |
| `replace_first(buscar, reemplazo)` | Reemplaza la primera aparición. |
| `starts_with(prefijo)` | Comprueba el prefijo. |
| `ends_with(sufijo)` | Comprueba el sufijo. |
| `contains(texto)` | Comprueba si contiene la subcadena. |
| `index_of(texto)` | Índice en puntos de código de la primera aparición, o `-1`. |
| `substr(inicio, longitud?)` | Subcadena desde `inicio` (admite negativos). |
| `chars()` | Array de cadenas de un carácter. |
| `repeat(n)` | Repite la cadena `n` veces. |
| `reversed()` | Invierte los puntos de código. |
| `pad_start(ancho, relleno?)` | Rellena por la izquierda hasta `ancho`. |
| `pad_end(ancho, relleno?)` | Rellena por la derecha hasta `ancho`. |

Detalles:

- `upper` y `lower` solo transforman `A-Z` y `a-z`; el resto de Unicode no cambia.
- `trim*` considera espacios ASCII (`std::isspace`), no todos los espacios Unicode.
- `lines` no añade una línea vacía al final si la cadena termina en `\n`.
- `index_of` busca bytes pero devuelve el índice en puntos de código.
- `substr` cuenta puntos de código; `longitud` negativa produce una cadena vacía.
- `pad_start` y `pad_end` rellenan por puntos de código y recortan el relleno al ancho exacto.
- `repeat` tiene un límite de 64 MB de resultado.

```jolt
assert_eq("  hola  ".trim(), "hola");
assert_eq("a,b,c".split(","), ["a", "b", "c"]);
assert_eq("abc".substr(1, 2), "bc");
assert_eq("5".pad_start(3, "0"), "005");
```

---

## 10. Mapas

### 10.1 Literales

Las claves de un literal deben ser cadenas simples o identificadores:

```jolt
let persona = {
  nombre: "Ada",
  edad: 36,
  "clave con espacios": true,
};
```

No se admiten claves calculadas ni interpoladas en el literal. Si una clave se repite, prevalece el último valor, pero se conserva la posición de la primera aparición.

### 10.2 Acceso y asignación

```jolt
assert_eq(persona.nombre, "Ada");
assert_eq(persona["edad"], 36);

persona.edad = 37;
persona["ciudad"] = "Londres";
```

Acceder a una clave inexistente devuelve `nil` (tanto con `.` como con `[]`). Las claves deben ser cadenas; usar otro tipo como índice es un error.

Los mapas conservan el orden de inserción. Volver a asignar una clave existente no la mueve.

### 10.3 Miembros y colisión con métodos

`mapa.length` devuelve el número de claves, salvo que exista una clave `"length"`, en cuyo caso se devuelve su valor. Del mismo modo, una clave con el nombre de un método (por ejemplo `"keys"`) tiene prioridad sobre el método.

### 10.4 Métodos de mapa

| Método | Descripción |
| --- | --- |
| `keys()` | Array de claves en orden de inserción. |
| `values()` | Array de valores en orden de inserción. |
| `entries()` | Array de pares `[clave, valor]`. |
| `has(clave)` | `true` si existe la clave. |
| `get(clave, defecto?)` | Valor de la clave o `defecto`/`nil`. |
| `set(clave, valor)` | Asigna y devuelve el mapa. |
| `delete(clave)` | Elimina la clave; devuelve `true` si existía. |
| `clear()` | Vacía el mapa y lo devuelve. |
| `merge(otro)` | Copia las claves de `otro` en este mapa y lo devuelve. |
| `clone()` | Copia superficial. |

```jolt
let m = {a: 1};
m.set("b", 2);
assert_eq(m.keys(), ["a", "b"]);
assert_eq(m.entries(), [["a", 1], ["b", 2]]);
assert_eq(m.get("z", 0), 0);
assert_eq(m.delete("a"), true);
assert_eq(m, {b: 2});
```

La igualdad de mapas no depende del orden: dos mapas con las mismas claves y valores son iguales.

Iterar un mapa produce sus claves:

```jolt
for clave in {a: 1, b: 2} {
  print(clave);
}
```

---

## 11. Rangos

### 11.1 Sintaxis

| Expresión | Significado |
| --- | --- |
| `a..b` | Desde `a` hasta `b`, sin incluir `b`. |
| `a..=b` | Desde `a` hasta `b`, incluyendo `b`. |

Los límites deben ser enteros. Un rango puede ser ascendente o descendente:

```jolt
assert_eq((1..4).to_array(), [1, 2, 3]);
assert_eq((1..=3).to_array(), [1, 2, 3]);
assert_eq((3..0).to_array(), [3, 2, 1]);
assert_eq((3..=0).to_array(), [3, 2, 1, 0]);
```

`5..5` está vacío; `5..=5` contiene un elemento.

### 11.2 Operaciones

- `.length` / `len(rango)`: número de elementos.
- `.to_array()`: convierte a array (límite de 10 000 000 de elementos).
- `.contains(valor)`: igual que `valor in rango`.
- La igualdad compara inicio, fin e inclusividad: `1..3 != 1..=3`.

La función `range(fin)` produce `0..fin`, y `range(inicio, fin)` produce `inicio..fin` (siempre exclusivo).

```jolt
assert_eq(2 in 1..5, true);
assert_eq(5 in 1..5, false);
assert_eq(5 in 1..=5, true);
assert_eq(len(5..0), 5);
```

---

## 12. Biblioteca estándar

### 12.1 Consola y entrada

| Función | Descripción |
| --- | --- |
| `print(valor, ...)` | Imprime los valores separados por espacio y añade `\n`. |
| `write(valor, ...)` | Como `print`, pero sin salto de línea final. |
| `eprint(valor, ...)` | Como `print`, pero en la salida de error. |
| `input(prompt?)` | Lee una línea; devuelve `nil` al final de la entrada. |

### 12.2 Tipos y conversión

| Función | Descripción |
| --- | --- |
| `type(valor)` | Nombre del tipo como cadena. |
| `str(valor)` | Representación textual. |
| `repr(valor)` | Representación con cadenas entre comillas. |
| `int(valor)` | Convierte a entero (trunca flotantes). |
| `float(valor)` | Convierte a flotante. |
| `bool(valor)` | Aplica la verdad de Jolt. |

`int(cadena)` acepta el formato de `strtoll` con base 0 (decimal, `0x...`, `0...` octal) y recorta espacios. `float(cadena)` usa `strtod`. Convertir una cadena vacía o con formato inválido es un error.

`str` de un array o mapa usa la representación de sus elementos, con las cadenas internas entre comillas:

```jolt
assert_eq(str(42), "42");
assert_eq(str([1, "a"]), "[1, \"a\"]");
assert_eq(repr("a"), "\"a\"");
```

### 12.3 Matemáticas

| Función | Descripción |
| --- | --- |
| `abs(n)` | Valor absoluto. |
| `floor(n)` | Redondeo hacia abajo. |
| `ceil(n)` | Redondeo hacia arriba. |
| `round(n)` | Redondeo al entero más cercano (mitades hacia fuera de cero). |
| `sqrt(n)` | Raíz cuadrada; error si `n < 0`. |
| `pow(base, exp)` | Potencia como `float`. |
| `min(a, ...)` | Mínimo de al menos un argumento. |
| `max(a, ...)` | Máximo de al menos un argumento. |
| `clamp(n, mínimo, máximo)` | Limita `n` al intervalo. |

`floor`, `ceil` y `round` devuelven el mismo `int` si reciben un `int`, y un `float` si reciben un `float`.

### 12.4 Aleatoriedad y tiempo

| Función | Descripción |
| --- | --- |
| `random()` | `float` en `[0, 1)`. |
| `random(alto)` | `int` en `[0, alto)`. |
| `random(bajo, alto)` | `int` en `[bajo, alto)`. |
| `seed(n)` | Reinicia el generador con una semilla. |
| `clock()` | Segundos transcurridos desde el inicio del proceso. |

### 12.5 Utilidades

| Función | Descripción |
| --- | --- |
| `len(valor)` | Longitud de cadena, array, mapa o rango. |
| `chr(código)` | Carácter Unicode a partir de un punto de código. |
| `ord(carácter)` | Punto de código de una cadena de un carácter. |
| `range(fin)` | Rango `0..fin`. |
| `range(inicio, fin)` | Rango `inicio..fin`. |
| `args()` | Argumentos del script como array de cadenas. |

### 12.6 Archivos

| Función | Descripción |
| --- | --- |
| `read_file(ruta)` | Devuelve el contenido de un archivo como cadena. |
| `write_file(ruta, contenido)` | Escribe el contenido (binario exacto). |
| `file_exists(ruta)` | Comprueba si el archivo se puede abrir. |

### 12.7 Aserciones y control

| Función | Descripción |
| --- | --- |
| `assert(condición, mensaje?)` | Falla si la condición es falsa. |
| `assert_eq(esperado, real, mensaje?)` | Falla si no son iguales. |
| `panic(mensaje?)` | Provoca un error de tiempo de ejecución. |
| `exit(código?)` | Termina el proceso con el código indicado (0 por defecto). |

`assert_eq` sigue el orden `esperado, real` y añade ambos valores al mensaje de error.

---

## 13. Errores y diagnóstico

Jolt distingue dos clases de errores:

- **Error de compilación**: lo detectan el lexer o el parser (por ejemplo, un `;` ausente o un literal mal formado).
- **Error de tiempo de ejecución**: lo detecta el intérprete (por ejemplo, una variable indefinida, una división por cero o un índice fuera de rango).

Ambos se muestran con la ubicación y un fragmento de código:

```text
script.jolt:3:9: runtime error: division by zero
    |
  3 | let x = 1 / 0;
    |         ^
```

Los errores de tiempo de ejecución incluyen además una traza de pila con las funciones activas.

Códigos de salida del proceso:

| Código | Significado |
| --- | --- |
| `0` | Correcto. |
| `64` | Uso incorrecto de la línea de comandos. |
| `65` | Error de compilación. |
| `66` | No se pudo leer el archivo. |
| `70` | Error de tiempo de ejecución. |
| otro | Código indicado por `exit(n)`. |

`jolt -c archivo.jolt` comprueba solo el lexer y el parser; no ejecuta el programa.

---

## 14. REPL

Ejecutar `jolt` sin argumentos inicia el REPL. Características:

- Las expresiones se evalúan y se muestran con `repr`; `nil` no se imprime.
- `_` guarda el último valor evaluado.
- El prompt `...>` indica continuación; una línea en blanco cancela la entrada incompleta.
- Se puede omitir el `;` final de cada entrada.
- Las definiciones persisten hasta `:clear`.

Comandos:

| Comando | Descripción |
| --- | --- |
| `:help`, `:?` | Ayuda. |
| `:quit`, `:q`, `:exit` | Salir. |
| `:clear` | Borrar las definiciones del usuario. |
| `:load <archivo>` | Ejecutar un script en la sesión actual. |
| `:vars` | Listar variables globales del usuario. |

Ejemplo:

```text
jolt> let x = 10
jolt> fn doble(n) => n * 2
jolt> doble(x)
20
jolt> _
20
jolt> :vars
  doble: function
  x: int
```

---

## 15. Línea de comandos

```text
jolt                      inicia el REPL
jolt <archivo.jolt>       ejecuta un script
jolt -e "<código>"        ejecuta código desde la línea de comandos
jolt -c <archivo.jolt>    comprueba la sintaxis sin ejecutar
```

| Opción | Descripción |
| --- | --- |
| `-h`, `--help` | Ayuda. |
| `-v`, `--version` | Versión. |
| `-e`, `--eval <código>` | Código a ejecutar. El resto de argumentos pasa al script. |
| `-c`, `--check` | Solo comprobación sintáctica. |
| `--` | Fin de opciones; el siguiente argumento es el archivo. |

Las opciones deben ir antes del archivo. Todo lo que sigue al archivo se considera argumento del script y se obtiene con `args()`.

```sh
jolt programa.jolt uno dos
```

```jolt
for argumento in args() {
  print(argumento);
}
```

---

## 16. Limitaciones conocidas

- Tipado dinámico y ejecución por recorrido del AST; no hay bytecode ni compilación JIT.
- Sin clases, structs, interfaces ni herencia.
- Sin `try`/`catch` ni excepciones capturables desde el lenguaje.
- Sin módulos ni `import`. En el REPL se puede usar `:load`.
- Sin `switch`, `do`/`while`, `++`, `--`, parámetros por defecto ni argumentos variables en funciones de usuario.
- Las claves de mapa son siempre cadenas y los literales solo admiten claves simples.
- Los cuerpos de `if`, `while` y `for` requieren llaves.
- El `for ... in` no admite paréntesis alrededor de la variable y el iterable.
- Sin sintaxis de rebanado para cadenas o arrays; se usan métodos como `substr` y `slice`.
- `upper` y `lower` solo funcionan con ASCII.
- Profundidad máxima de llamadas: 2000.
- Memoria gestionada con recuento de referencias; las referencias cíclicas no se liberan.
- La ordenación con comparadores personalizados confía en que el comparador defina un orden estricto.

---

## 17. Ejemplo completo

```jolt
// Cuenta palabras y muestra las más frecuentes.

fn contar_palabras(texto) {
  let conteo = {};
  for palabra in texto.lower().split() {
    conteo[palabra] = conteo.get(palabra, 0) + 1;
  }
  return conteo;
}

let texto = "uno dos dos tres tres tres cuatro cuatro cuatro cuatro";
let conteo = contar_palabras(texto);

let pares = conteo.entries();
pares.sort(fn(a, b) => a[1] > b[1]);

for par in pares {
  print("${par[0]}: ${par[1]}");
}
```

Salida:

```text
cuatro: 4
tres: 3
dos: 2
uno: 1
```
