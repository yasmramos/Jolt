#include "env.h"

Binding* Env::find(const std::string& name) {
  for (Env* scope = this; scope != nullptr; scope = scope->parent.get()) {
    auto it = scope->vars.find(name);
    if (it != scope->vars.end()) return &it->second;
  }
  return nullptr;
}

void Env::define(const std::string& name, Value value, bool isImmutable) {
  Binding binding;
  binding.value = std::move(value);
  binding.isImmutable = isImmutable;
  vars[name] = std::move(binding);
}

Env::Assign Env::assign(const std::string& name, Value value) {
  Binding* binding = find(name);
  if (binding == nullptr) return Env::Assign::NotFound;
  if (binding->isImmutable) return Env::Assign::Immutable;
  binding->value = std::move(value);
  return Env::Assign::Ok;
}
