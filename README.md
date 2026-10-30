# Jolt

**Jolt** es un lenguaje de programación dinámico y de propósito general, implementado desde cero en **C++17**. El proyecto incluye el lexer, el parser, el AST, el compilador a bytecode, la máquina virtual register-based y la biblioteca estándar, sin dependencias externas más allá de la biblioteca estándar de C++.

Su objetivo es ser pequeño, legible y fácil de estudiar: un único ejecutable, una arquitectura de bytecode estilo Lua y una sintaxis familiar con llaves y punto y coma.

```jolt
fn saludar(nombre) {
  return "Hola, ${nombre}!";
}

let lenguajes = ["C++", "Jolt"];
for lenguaje in lenguajes {
  print(saludar(lenguaje));
}
```

## Características

- Tipos dinámicos: `nil`, `bool`, `int` (64 bits), `float`, `string`, `array`, `map`, `function` y `range`.
- Variables con `let` y `const`, ámbito de bloque y closures reales.
- Funciones de primera clase, recursión, funciones anónimas y lambdas con `=>`.
- Interpolación de cadenas con `"${expresión}"` y soporte UTF-8.
- Arrays ordenados y mapas que conservan el orden de inserción.
- Rangos ascendentes y descendentes: `1..5`, `0..=10`, `5..0`.
- Operadores aritméticos, lógicos, bit a bit, de comparación, ternario y de asignación compuesta.
- Control de flujo: `if`/`else`, `while`, `for` clásico y `for ... in`.
- Biblioteca estándar integrada: cadenas, arrays, mapas, matemáticas, archivos, consola y utilidades.
- REPL interactivo con estado de sesión, comandos y evaluación de expresiones.
- Diagnósticos con archivo, línea, columna, fragmento de código y traza de pila.

## Estado

Versión **0.1.0**. El lenguaje es funcional y está cubierto por una suite de pruebas, pero todavía no se considera estable ni completo. Consulta la sección de [limitaciones](#limitaciones-y-hoja-de-ruta).

## Requisitos

- Un compilador de C++17: `g++`, `clang++` o MSVC.
- `cmake` 3.15 o superior (opcional, solo si prefieres compilar con CMake).
- En Windows se ha probado con **llvm-mingw** (clang 21 con la interfaz de `g++`).

## Compilación

### Windows (script)

```bat
build.bat
build\jolt.exe examples\hello.jolt
```

### Linux, macOS o MSYS2 (script)

```sh
chmod +x build.sh tests/run.sh
./build.sh
./build/jolt examples/hello.jolt
```

Puedes elegir el compilador y las opciones:

```sh
CXX=clang++ CXXFLAGS="-O0 -g" ./build.sh
```

### CMake

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
./build-cmake/jolt examples/hello.jolt
```

### Compilación manual

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build/jolt src/*.cpp
```

En Windows, con `cmd.exe`:

```bat
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build\jolt.exe src\*.cpp
```

## Uso

```text
jolt                      inicia el REPL
jolt <archivo.jolt>       ejecuta un script
jolt -e "<código>"        ejecuta código desde la línea de comandos
jolt -c <archivo.jolt>    analiza el script sin ejecutarlo
```

Opciones:

| Opción | Descripción |
| --- | --- |
| `-h`, `--help` | Muestra la ayuda. |
| `-v`, `--version` | Muestra la versión. |
| `-e`, `--eval <código>` | Ejecuta el código indicado. |
| `-c`, `--check` | Solo comprueba la sintaxis, sin ejecutar. |
| `--` | Marca el final de las opciones; el siguiente argumento es el archivo. |

Los argumentos que siguen al archivo (o al código de `-e`) se pasan al script y se obtienen con `args()`:

```sh
jolt saludo.jolt Ada Grace
```

```jolt
for nombre in args() {
  print("Hola, ${nombre}");
}
```

### Códigos de salida

| Código | Significado |
| --- | --- |
| `0` | Ejecución correcta. |
| `64` | Error de uso de la línea de comandos. |
| `65` | Error de compilación (lexer o parser). |
| `66` | No se pudo leer el archivo. |
| `70` | Error en tiempo de ejecución. |
| otro | Valor indicado por `exit(código)`. |

### REPL

Al ejecutar `jolt` sin argumentos se abre el REPL:

```text
Jolt 0.1.0 - a small scripting language written in C++
Type :help for help, :quit to exit.
jolt> let x = 21
jolt> x * 2
42
jolt> fn doble(n) => n * 2
jolt> doble(x)
42
```

- Las expresiones muestran su resultado en formato `repr`.
- `_` contiene el último valor evaluado.
- El prompt `...>` indica que la entrada continúa en la línea siguiente.
- Una línea en blanco cancela una entrada incompleta.
- Se acepta omitir el `;` final de cada entrada del REPL.
- Las definiciones permanecen en la sesión hasta usar `:clear`.

Comandos disponibles:

| Comando | Descripción |
| --- | --- |
| `:help`, `:?` | Muestra la ayuda del REPL. |
| `:quit`, `:q`, `:exit` | Sale del REPL. |
| `:clear` | Olvida todas las definiciones del usuario. |
| `:load <archivo>` | Ejecuta un script dentro de la sesión actual. |
| `:vars` | Lista las variables globales definidas por el usuario. |

## Ejemplos

En el directorio `examples/`:

| Archivo | Contenido |
| --- | --- |
| `hello.jolt` | Saludo e interpolación de cadenas. |
| `fizzbuzz.jolt` | Bucles, condicionales y módulo. |
| `fibonacci.jolt` | Recursión y versión iterativa. |
| `closures.jolt` | Closures y funciones que recuerdan estado. |
| `collections.jolt` | Arrays, mapas, filtros, reducción y ordenación. |

```sh
jolt examples/fizzbuzz.jolt
```

## El lenguaje en un minuto

```jolt
// Variables y constantes
let nombre = "mundo";
const VERSION = "0.1.0";

// Funciones y closures
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

// Condicionales y bucles
if (contador() > 2) {
  print("va rápido");
} else {
  print("va lento");
}

for i in 1..=3 {
  print("i = ${i}");
}

// Colecciones
let numeros = [1, 2, 3, 4, 5];
let cuadrados_pares = numeros
  .filter(fn(n) => n % 2 == 0)
  .map(fn(n) => n * n);
print(cuadrados_pares); // [4, 16]

let persona = {nombre: "Ada", edad: 36};
persona.edad += 1;
print("${persona.nombre} tiene ${persona.edad} años");
```

La referencia completa está en **[docs/language.md](docs/language.md)**.

## Pruebas

Windows:

```bat
powershell -ExecutionPolicy Bypass -File tests\run.ps1
powershell -ExecutionPolicy Bypass -File tests\run_compiler.ps1
powershell -ExecutionPolicy Bypass -File tests\run_vm.ps1
```

Linux, macOS o MSYS2:

```sh
./tests/run.sh
./tests/run_compiler.sh
./tests/run_vm.sh
```

Las pruebas positivas usan `assert` y `assert_eq`; un script termina con código distinto de cero si alguna aserción falla. Las pruebas de `tests/errors/` comprueban errores de compilación y de ejecución: declaran el código de salida esperado en su primera línea con `// expect-exit: N`. Las pruebas del compilador de bytecode (`compiler_test.cpp`) verifican la secuencia de instrucciones, los pools de constantes, los literales `nil`/booleanos, los operadores unarios y compuestos, las comparaciones, los saltos de `if`/`while`/`for` con backpatching, los slots locales y el rechazo de constantes reasignadas. Las pruebas de la VM (`vm_test.cpp`) compilan fragmentos a bytecode y comprueban la ejecución de aritmética entera y flotante, `%`, `**`, comparaciones, unarios, globales, cadenas, asignación compuesta, `if`/`else`, bucles, `break`/`continue`, shadowing de bloques, operadores de bits, cortocircuito de `&&`/`||`, ternario y errores de ejecución.

## Limitaciones y hoja de ruta

Limitaciones actuales:

- Tipado dinámico. El modelo del producto es el de Lua: el código fuente siempre se compila a bytecode y se ejecuta en la VM. Mientras se completa la FASE 6 (funciones, closures, colecciones), el ejecutable `jolt` todavía usa el intérprete de AST como motor temporal; el compilador y la VM ya cubren control de flujo y locales. No hay JIT.
- No hay clases, structs, interfaces ni herencia.
- No hay `try`/`catch` ni excepciones capturables desde el lenguaje.
- No hay `import`/módulos; en el REPL se puede usar `:load`.
- No hay `switch`, `do`/`while`, `++`/`--` ni parámetros por defecto.
- Las claves de los mapas son siempre cadenas y los literales de mapa solo admiten claves simples.
- Las llaves son obligatorias en los cuerpos de `if`, `while` y `for`.
- No existe sintaxis de rebanado (`s[1:3]`); se usan `substr`, `slice`, etc.
- `upper` y `lower` solo transforman letras ASCII.
- La profundidad máxima de llamadas es 2000.
- No hay recolección de basura explícita: se usa recuento de referencias, por lo que las referencias cíclicas pueden retener memoria.

Ideas para futuras versiones:

- Conectar el compilador a bytecode y la VM al CLI y al REPL.
- `try`/`catch` y errores como valores.
- Structs y métodos definidos por el usuario.
- Módulos e `import`.
- Rebanado de cadenas y arrays.
- Mejor soporte Unicode (mayúsculas/minúsculas y grafemas).
- Más herramientas: formateador, resaltado de sintaxis y depurador.

## Licencia

Distribuido bajo la licencia **MIT**. Consulta [LICENSE](LICENSE).

---

Hecho con C++17, sin dependencias externas.
