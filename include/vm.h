#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "bytecode.h"
#include "value.h"

// Maquina virtual register-based (FASE 3-5).
//
// Ejecuta el bytecode generado por el compilador. Todos los registros y
// variables globales son `Value`; no hay arrays separados por tipo. Las
// operaciones aritmeticas tienen rutas rapidas para enteros y flotantes.
//
// Los globales se conservan entre llamadas a `run()` (util para un REPL);
// los registros y locales se reinician al principio de cada ejecucion.
class VM {
 public:
  VM() = default;

  // Ejecuta el bytecode desde la primera instruccion hasta HALT o el final.
  // Lanza `RuntimeError` si el bytecode intenta una operacion invalida.
  void run(const Bytecode& bytecode);

  // Devuelve el valor de un global, o nullptr si no existe.
  const Value* findGlobal(const std::string& name) const;

  // Numero de registros reservados en la ultima ejecucion.
  int registerCount() const { return static_cast<int>(registers_.size()); }

  // Numero de celdas locales reservadas en la ultima ejecucion.
  int localCount() const { return static_cast<int>(locals_.size()); }

 private:
  std::vector<Value> registers_;
  std::vector<CellPtr> locals_;
  std::unordered_map<std::string, Value> globals_;

  Value& getReg(int index);
  void setReg(int index, Value value);
  CellPtr& localAt(int index);
};
