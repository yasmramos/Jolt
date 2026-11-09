// Tests for the `ptr`/`cstring` value representation (PHASE 6.6, slice B1).
// A pointer is an opaque handle: a reference-counted `PtrObj` that owns the
// C-side bytes but is *not* a collector node, because it can never take part in
// a reference cycle.

// Release builds define NDEBUG, which would compile every assert() out and
// leave the suite green without testing anything.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "gc.h"
#include "value.h"

namespace {

void testCStringKindAndBytes() {
  Value value = makePtr(PtrKind::CString, std::vector<char>{'h', 'i', '\0'});
  assert(value.isPtr());
  assert(value.type() == VType::Ptr);
  // `typeName` reports the FFI spelling the handle was built as.
  assert(std::string(value.typeName()) == "cstring");
  assert(value.ptr().kind == PtrKind::CString);
  assert(value.ptr().bytes.size() == 3);
}

void testVoidPointerReportsPtr() {
  Value value = makePtr(PtrKind::Void, {}, 4096);
  assert(value.isPtr());
  assert(std::string(value.typeName()) == "ptr");
  // A `void *` carries only its opaque token, never a dereferenceable address.
  assert(value.ptr().address == 4096);
  assert(value.ptr().bytes.empty());
}

void testPtrsCompareByIdentity() {
  Value a = makePtr(PtrKind::Void, {}, 7);
  Value b = a;  // a copy shares the same handle
  Value c = makePtr(PtrKind::Void, {}, 7);  // same token, different handle
  assert(a.equals(b));
  assert(!a.equals(c));
}

void testStringRendering() {
  Value cs = makePtr(PtrKind::CString, std::vector<char>{'o', 'k', '\0'});
  assert(cs.toString() == "ok");   // display form is the text
  assert(cs.repr() == "\"ok\"");   // debug form is quoted

  Value buffer = makePtr(PtrKind::Buffer, std::vector<char>{'a', 'b', 'c'});
  assert(buffer.toString() == "<ptr>");  // opaque, never an address
  assert(buffer.repr() == "<ptr>");
}

// A pointer owns only raw bytes, so the collector must not track it: a value
// that cannot form a cycle adds nothing to the tracked set.
void testPointersAreNotTracked() {
  gcCollect();
  const std::size_t base = gcTrackedCount();
  {
    Value value = makePtr(PtrKind::Buffer, std::vector<char>(64, 'x'));
    (void)value;
    assert(gcTrackedCount() == base);
  }
  gcCollect();
  assert(gcTrackedCount() == base);
}

}  // namespace

// `str(double)` must print enough digits to round-trip through binary64. With a
// fixed 15-digit format `1.0 / 3.0` printed "0.333333333333333", which parses
// back to a different double.
void testDoubleFormattingRoundTrips() {
  const double thirds = 1.0 / 3.0;
  Value v(thirds);
  assert(v.isDouble());
  const std::string text = v.toString();
  assert(text == "0.3333333333333333");
  assert(std::strtod(text.c_str(), nullptr) == thirds);

  const double sum = 0.1 + 0.2;
  Value s(sum);
  assert(s.toString() == "0.30000000000000004");
  assert(std::strtod(s.toString().c_str(), nullptr) == sum);

  // Values that fit in fewer digits keep the shortest form.
  assert(Value(1.5).toString() == "1.5");
  assert(Value(0.1).toString() == "0.1");
}

// A `float` prints with the binary32 shortest round-trip text.
void testFloatFormattingRoundTrips() {
  Value v = Value::fromFloat(0.1f);
  assert(v.isFloat());
  assert(v.toString() == "0.1");
}

// Structural equality must terminate on cyclic arrays and maps. Before the fix
// `Value::equals` recursed forever on a self-referential container and the
// process died with a stack overflow.
void testCyclicEqualityTerminates() {
  // Two self-referential arrays with the same contents.
  Value a = makeArray({Value(int64_t(1))});
  a.arr().items.push_back(a);
  Value b = makeArray({Value(int64_t(1))});
  b.arr().items.push_back(b);
  assert(a.equals(b));
  assert(b.equals(a));
  assert(a.equals(a));

  // Same shape, different content.
  Value c = makeArray({Value(int64_t(2))});
  c.arr().items.push_back(c);
  assert(!a.equals(c));

  // Self-referential maps.
  Value m1 = makeMap();
  m1.map().set("v", Value(int64_t(1)));
  m1.map().set("self", m1);
  Value m2 = makeMap();
  m2.map().set("v", Value(int64_t(1)));
  m2.map().set("self", m2);
  assert(m1.equals(m2));

  Value m3 = makeMap();
  m3.map().set("v", Value(int64_t(2)));
  m3.map().set("self", m3);
  assert(!m1.equals(m3));

  // Mutual cycles.
  Value p = makeMap();
  Value q = makeMap();
  p.map().set("next", q);
  q.map().set("next", p);
  Value r = makeMap();
  Value s = makeMap();
  r.map().set("next", s);
  s.map().set("next", r);
  assert(p.equals(r));

  Value t = makeMap();
  Value u = makeMap();
  t.map().set("next", u);
  u.map().set("next", Value(int64_t(5)));
  assert(!p.equals(t));
}

int main() {
  testCStringKindAndBytes();
  testVoidPointerReportsPtr();
  testPtrsCompareByIdentity();
  testStringRendering();
  testPointersAreNotTracked();
  testDoubleFormattingRoundTrips();
  testFloatFormattingRoundTrips();
  testCyclicEqualityTerminates();
  std::puts("value tests passed");
  return 0;
}
