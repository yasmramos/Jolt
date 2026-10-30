# AGENT.md — Proyecto Jolt

Instrucciones para agentes de IA que asisten en el desarrollo de **Jolt**,
un lenguaje de programación compilado con arquitectura interna estilo Lua.

---

## 1. Identidad del proyecto

| Campo | Valor |
|-------|-------|
| **Nombre** | Jolt |
| **Extensión** | `.jolt` |
| **Filosofía** | Simple como Python, estructurado como Java, rápido como Lua |
| **Implementación** | C++17 (sin dependencias externas) |
| **Plataforma inicial** | Windows (g++/MinGW), luego multiplataforma |
| **Arquitectura** | Bytecode register-based + VM (estilo Lua 5.4) |

---

## 2. Arquitectura obligatoria
.jolt → LEXER → PARSER → AST → COMPILADOR → BYTECODE → VM
↓
(futuro) COMPILADOR NATIVO


### Módulos y responsabilidades

| Módulo | Entrada | Salida | Estado |
|--------|---------|--------|--------|
| `lexer.cpp` | texto fuente | `Token[]` | ✅ existente |
| `parser.cpp` | `Token[]` | `Node*` (AST) | ✅ existente |
| `bytecode.cpp` | `Bytecode` | operaciones básicas | ✅ FASE 1 |
| `compiler.cpp` | `Node*` | `Bytecode*` | ✅ FASE 5 |
| `vm.cpp` | `Bytecode*` | ejecución | ✅ FASE 5 |
| `main.cpp` | CLI | pipeline completo | ⬜ FASE 4 (VM como motor) |
| `interpreter.cpp` | `Node*` | ejecución de referencia | ⚠️ solo oráculo de pruebas |

---

## 3. Reglas del bytecode (CRÍTICAS)

Estas reglas **no son negociables**. Si una propuesta las viola, rechazarla
y proponer una alternativa.

1. **Register-based**, NO stack-based. Los registros son un array de valores
   dinámicos en la VM (`Value regs[256]`). No usar solo `double`: Jolt
   necesita representar también cadenas, booleanos, `nil`, arrays y mapas.
   Todos los registros, variables globales, frames y valores temporales usan
   el mismo tipo `Value`. `Bytecode::numbers` y `Bytecode::strings` son solo
   pools de constantes para el compilador; la VM no mantiene registros
   separados por tipo. Las operaciones aritméticas deben incluir rutas rápidas
   para `int` y `float`.
   En C++17, `Value` es un tagged union de 16 bytes con recuento de
   referencias no atómico (el intérprete/VM es single-threaded). Las cadenas
   son inmutables y compartidas, de modo que `MOVE` no duplica texto.
2. **Cada opcode mapea a 1 instrucción ASM x86-64** (o casi). Si no mapea,
   está mal diseñado.
3. **Máximo ~40 opcodes** en total. Si se necesitan más, rediseñar.
4. **Instrucciones prohibidas** (demasiado alto nivel):
   - `OP_EVAL_EXPR`, `OP_EXECUTE_STATEMENT`, `OP_CALL_METHOD_DYNAMIC`
5. **Instrucciones permitidas** (bajo nivel):
   - Carga: `LOADK` (número), `LOADKS` (cadena), `LOADNIL`, `LOADBOOL`, `MOVE`
   - Aritmética: `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `POW`, `NEG`, `NOT`
   - Bits: `BAND`, `BOR`, `BXOR`, `SHL`, `SHR`, `BNOT`
   - Comparación: `EQ`, `NEQ`, `LT`, `GT`, `LTE`, `GTE`
   - Variables: `GETGLOBAL`, `SETGLOBAL`, `DECLLOCAL`, `GETLOCAL`, `SETLOCAL`
   - Control: `JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE`
   - Funciones: `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`, `SETUPVAL`
   - I/O: `PRINT`
   - Fin: `HALT`

### Anti-objetivo explícito

> **NO repetir el error de Java**: bytecode de alto nivel que luego es
> imposible compilar a nativo de forma eficiente. El bytecode de Jolt
> debe nacer pensando en la compilación nativa futura.

### Mapeo esperado bytecode → ASM

| Bytecode | ASM x86-64 |
|----------|-----------|
| `LOADK R0, 10` | `movq $10, %rax` |
| `ADD R2, R0, R1` | `addq %rbx, %rax` |
| `JMP_IF_FALSE R2, +2` | `testq %rax, %rax; jz .label` |
| `SETGLOBAL R2, "x"` | `movq %rax, x(%rip)` |
| `PRINT R3` | `call printf` |

---

## 4. Fases de desarrollo (en orden estricto)

No se puede avanzar a la siguiente fase sin completar y probar la anterior.

### FASE 1 — `include/bytecode.h` ✅
- Definir `enum class OpCode` con: `LOADK`, `LOADKS`, `LOADNIL`, `LOADBOOL`,
  `MOVE`, `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `POW`, `NEG`, `NOT`, `EQ`, `NEQ`,
  `LT`, `LTE`, `GT`, `GTE`, `SETGLOBAL`, `GETGLOBAL`, `PRINT`, `HALT`
- Definir `struct Instruction { OpCode op; int a, b, c; int jump; }`
- Definir `struct Bytecode` con `code`, `numbers`, `strings` y `num_registers`
- `numbers` es `std::vector<Value>` para conservar la diferencia entre `int` y `float`
- `LOADK` carga constantes numéricas y `LOADKS` constantes de cadena (opcodes
  separados, sin usar `c` como discriminador)
- Funciones: `bytecode_init`, `bytecode_emit`, `bytecode_free` (declaradas en
  `include/bytecode.h`, implementadas en `src/bytecode.cpp`)

### FASE 2 — `src/compiler.cpp` (AST → bytecode) ✅
- Literales: `nil` → `LOADNIL`, booleanos → `LOADBOOL`, números → `LOADK`,
  cadenas → `LOADKS`
- `VariableExpr` → `GETGLOBAL`
- `BinaryExpr` con `+`, `-`, `*`, `/`, `%`, `**` y comparaciones (`==`, `!=`,
  `<`, `<=`, `>`, `>=`) → compilar izq, compilar der, emitir el opcode
- `UnaryExpr`: `-` → `NEG`, `!` → `NOT`, `+` no emite instrucción, `~` sigue
  reservado para FASE 5
- `&&`, `||`, `in` y operadores de bits → FASE 5
- `AssignExpr` sobre variable global → compilar expr, emitir `SETGLOBAL`
- Asignación compuesta (`+=`, `-=`, `*=`, `/=`, `%=`, `**=`) → `GETGLOBAL`,
  operación, `SETGLOBAL`
- `LetStmt` → compilar inicializador; si no hay, emitir `LOADNIL` explícito;
  después `SETGLOBAL`
- Seguimiento de `const` en tiempo de compilación (`consts_`): reasignar una
  constante lanza `CompileError` con el mismo mensaje que el intérprete
- `print(expr)` → compilar expr, emitir `PRINT`; en el AST actual `print` es
  una llamada, así que se detecta como `CallExpr`
- Asignación de registros con contador que sube/baja
- Actualizar `num_registers` al reservar registros
- Reutilización de constantes en los pools de números y cadenas; `int` y
  `float` son constantes distintas
- Nodos aún no soportados lanzan `CompileError` con línea/columna

### FASE 3 — `src/vm.cpp` (ejecutar bytecode) ✅
- Bucle `while (pc < count)` + `switch` sobre opcode
- `std::vector<Value> registers_` dimensionado por `num_registers`
- Tabla de variables globales `std::unordered_map<std::string, Value>`
- Rutas rápidas numéricas en `ADD`, `SUB`, `MUL`, `DIV` sin recorrer variantes
- `ADD` también concatena cadenas; `DIV` y `MOD` detectan división por cero
- `POW` replica `intPow` para `int ** int` con exponente no negativo y usa
  `std::pow` en el resto de los casos
- `NEG` y `NOT` respetan la semántica del intérprete (`0` es truthy)
- Comparaciones con `EQ`/`NEQ`/`LT`/`LTE`/`GT`/`GTE` y la misma relación de
  orden que `Interpreter::lessThan`
- `PRINT` usa `Value::toString()`
- Errores de ejecución mediante `RuntimeError`
- `run()` reinicia registros y conserva globales entre ejecuciones
- Cada opcode: 2-3 líneas de C++

### FASE 4 — `src/main.cpp` (VM como único motor) ⬜ (se ejecuta tras FASE 6)
- Modelo Lua/LuaJIT: `jolt`, `jolt -e` y el REPL **siempre** compilan a bytecode
  y ejecutan en la VM. El AST nunca se ejecuta en producción.
- REPL con **una sola VM persistente**: cada entrada se compila a un chunk y se
  ejecuta sobre los mismos globales.
- `--ast` temporal solo para pruebas diferenciales (comparar AST vs VM); se
  retira del binario cuando haya paridad.
- `jolt -c archivo.jolt` compila sin ejecutar.
- `jolt build archivo.jolt` (futuro) → generar C++ + g++

### FASE 5 — Control de flujo y locales ✅
- Opcodes: `JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE`
- Saltos con backpatching (destino absoluto dentro de `code`)
- `if`/`else` (incluido `else if`), `while`, `for` clásico, `break` y `continue`
  (compile-time error fuera de un bucle)
- Scopes de bloque y variables locales con slots (`DECLLOCAL`, `GETLOCAL`,
  `SETLOCAL`); `Bytecode::num_locals`
- Las locales viven en `Cell` con recuento de referencias para que las closures
  de FASE 6 capturen la variable, no solo su valor
- Los `let`/`const` de nivel superior siguen siendo globales (persisten en el REPL)
- `&&`, `||` con cortocircuito real (saltos), ternario con saltos
- Operadores de bits: `BAND`, `BOR`, `BXOR`, `SHL`, `SHR`, `BNOT`
- `+x` no emite instrucción; `~x` exige entero
- **NO** se añaden `OP_IF` ni `OP_WHILE` (alto nivel)
- `in` y `for-in` quedan para FASE 6 (dependen de colecciones)

### FASE 6 — Funciones, colecciones y biblioteca ⬜ ← SIGUIENTE
- `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`, `SETUPVAL`
- Call stack en la VM y registros por frame
- `Bytecode` evoluciona a **protos por función** con protos hijos (estilo Lua)
- Closures con upvalues sobre `Cell` (las locales ya son celdas)
- Interfaz `Runtime` compartida por VM e intérprete AST para que los builtins
  (`map`, `filter`, `sort`) invoquen funciones de la VM sin reentrar en el AST
- Arrays, mapas, índice, miembro, rangos, interpolación y `for-in`
- `in` para arrays, mapas, cadenas y rangos

### FASE 7 — Compilador bytecode → C++ (futuro) ⬜
- Traducir cada instrucción a una línea de C++
- `g++ -O2` produce el binario nativo
- Evaluar `union`/`variant` para `Instruction` y eliminar `jump` cuando no se use
- Después, el JIT de LuaJIT como referencia

### Estado actual
Fase 1: ✅ completada y revisada
Fase 2: ✅ completada
Fase 3: ✅ completada
Fase 4: ⬜ pendiente (VM como único motor; requiere FASE 6)
Fase 5: ✅ completada y probada
Fase 6: ⬜ pendiente ← SIGUIENTE
Fase 7: ⬜ pendiente


---

## 5. Reglas de código

1. **C++17 puro**, sin dependencias externas (solo biblioteca estándar).
2. **Nombres**: `snake_case` para funciones y variables.
3. **Estructuras**: `struct` para datos y `enum class` para opcodes.
4. **Memoria**: RAII y contenedores estándar (`std::vector`, `std::string`); sin `malloc`/`free`.
5. **Errores**: structs de diagnóstico con línea/columna; la CLI los imprime en `stderr`.
6. **Comentarios en español**, concisos, solo donde aporten.
7. **Cada `.cpp` nuevo** tiene su `.h` en `include/`.
8. **Sin variables globales mutables** excepto la tabla de globals de la VM.
9. **Compilar con**: `g++ -std=c++17 -Wall -Wextra -O2`

---

## 6. Checklist obligatorio por cada feature nueva

Antes de escribir **una sola línea de código**, el agente debe responder
y mostrar al usuario:
¿Necesito un token nuevo en el lexer? → cuál

¿Necesito un nodo nuevo en el AST? → cuál

¿Necesito una regla nueva en el parser? → cuál

¿Necesito un opcode nuevo en el bytecode? → cuál

¿El opcode mapea a 1 instrucción ASM? → sí/no (si no, rediseñar)

¿Necesito lógica nueva en la VM? → cuál

¿Necesito lógica nueva en el compilador? → cuál


Sin este checklist completo y aprobado, **no se procede a codificar**.

---

## 7. Estilo de respuesta del agente

- ✅ Explicar el **concepto** antes del código.
- ✅ Mostrar archivos **completos** modificados (no fragmentos).
- ✅ Incluir un programa `.jolt` de prueba con cada feature nueva.
- ✅ Señalar qué **NO** hacer y por qué (evitar el error de Java).
- ✅ Advertir si una decisión afecta al diseño futuro (compilación nativa).
- ❌ No añadir features no pedidas (evitar scope creep).
- ❌ No proponer soluciones fuera de las fases actuales.

---

## 8. Prohibiciones explícitas

El agente **NO debe** proponer ni implementar:

| Prohibido | Razón |
|-----------|-------|
| VM stack-based | Jolt es register-based (estilo Lua) |
| Bytecode de alto nivel (`OP_EVAL_EXPR`) | Rompe la compilación nativa futura |
| JIT (compilador en caliente) | Nivel 6, años de trabajo, no es objetivo actual |
| Migrar el núcleo a C puro | El núcleo se mantiene en C++17 |
| Añadir features sin probar la anterior | Rompe el desarrollo incremental |
| Saltar el checklist de 7 puntos | Genera deuda arquitectónica |
| Bibliotecas externas | Solo biblioteca estándar de C++ |

---

## 9. Comandos que el agente debe reconocer

| Comando del usuario | Acción del agente |
|---------------------|-------------------|
| `empezar` | Iniciar FASE 1: mostrar `bytecode.h` completo y esperar aprobación |
| `fase N completada` | Verificar, actualizar estado, proponer FASE N+1 |
| `añadir <feature>` | Aplicar checklist de 7 puntos, luego codificar |
| `revisar <archivo>` | Verificar contra reglas del bytecode, señalar violaciones |
| `estado` | Mostrar resumen de fases y checklist |
| `resumen` | Resumir lo hecho en la sesión y actualizar estado |

---

## 10. Contexto histórico

- Existe un **intérprete de AST previo** (MiniLang, renombrado a Jolt) que se
  conserva únicamente como **oráculo de pruebas diferenciales** mientras se
  alcanza la paridad con la VM.
- El modelo del producto es el de **Lua/LuaJIT**: fuente → lexer → parser → AST
  → compilador → bytecode → VM. El AST **no se ejecuta** en producción.
- El enfoque "dual" (intérprete AST como motor alternativo) **NO** se preserva.
  Cuando la VM cubra el lenguaje (FASE 4), el intérprete AST sale del binario y
  queda solo en tests, y después se retira.
- Las fases se ejecutan en orden: 5 ✅, 6, luego 4 (VM como único motor) y 7.

---

## 11. Recursos de referencia (para el agente)

- *Crafting Interpreters* (Robert Nystrom) — fundamentos
- *The Implementation of Lua 5.0* — diseño de la VM de registros
- LuaJIT source code (Mike Pall) — referencia de rendimiento
- System V ABI AMD64 — convención de llamadas (FASE 7)

---

## 12. Primera tarea

Cuando el usuario escriba **`empezar`**, el agente debe:

1. Confirmar el diseño de `include/bytecode.h` con los opcodes mínimos.
2. Mostrar el **archivo completo**.
3. Explicar por qué cada opcode mapea a ASM.
4. Esperar **aprobación explícita** antes de pasar a la siguiente fase.
