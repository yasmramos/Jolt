#include "gc.h"

#include <algorithm>
#include <vector>

namespace {

// Every live tracked object, as an intrusive doubly linked list.
GcNode* registryHead = nullptr;
std::size_t trackedCount = 0;

// Tracked objects created since the last collection, and how many it takes to
// trigger the next one.
std::size_t createdSinceCollect = 0;
std::size_t threshold = 10000;
constexpr std::size_t kMinThreshold = 10000;

bool collecting = false;

void subtractReference(GcNode& child, void*) { --child.trial; }

void markReachable(GcNode& child, void* context) {
  if (child.reachable) return;
  child.reachable = true;
  static_cast<std::vector<GcNode*>*>(context)->push_back(&child);
}

}  // namespace

GcNode::GcNode(const Ops& opsTable, bool tracked) : ops(&opsTable) {
  if (tracked) link();
}

GcNode::GcNode(const GcNode& other) : ops(other.ops) {
  if (other.linked) link();
}

GcNode::~GcNode() {
  if (linked) unlink();
}

void GcNode::track() {
  if (!linked) link();
}

void GcNode::link() {
  next = registryHead;
  if (registryHead) registryHead->prev = this;
  registryHead = this;
  ++trackedCount;
  ++createdSinceCollect;
  linked = true;
}

void GcNode::unlink() {
  if (prev) {
    prev->next = next;
  } else {
    registryHead = next;
  }
  if (next) next->prev = prev;
  prev = nullptr;
  next = nullptr;
  --trackedCount;
  linked = false;
}

void gcCollect() {
  if (collecting) return;
  collecting = true;
  struct Done {
    ~Done() { collecting = false; }
  } done;

  // 1. Every object starts with all of its references counted as external.
  for (GcNode* node = registryHead; node; node = node->next) {
    node->trial = static_cast<long>(node->ops->refCount(*node));
    node->reachable = false;
  }

  // 2. Remove the references that come from other tracked objects.
  for (GcNode* node = registryHead; node; node = node->next) {
    node->ops->visit(*node, subtractReference, nullptr);
  }

  // 3. What still has references is held from outside: it and everything it
  //    can reach is alive.
  std::vector<GcNode*> pending;
  for (GcNode* node = registryHead; node; node = node->next) {
    if (node->trial > 0) {
      node->reachable = true;
      pending.push_back(node);
    }
  }
  while (!pending.empty()) {
    GcNode* node = pending.back();
    pending.pop_back();
    node->ops->visit(*node, markReachable, &pending);
  }

  // 4. The rest only references itself. Pin every garbage object first so that
  //    clearing one cannot free another while it is still being visited, then
  //    clear them all and let the pins release the memory.
  std::vector<GcNode*> garbage;
  for (GcNode* node = registryHead; node; node = node->next) {
    if (!node->reachable) garbage.push_back(node);
  }
  std::vector<std::shared_ptr<void>> pins;
  pins.reserve(garbage.size());
  for (GcNode* node : garbage) pins.push_back(node->ops->pin(*node));
  for (GcNode* node : garbage) node->ops->clear(*node);
  pins.clear();

  createdSinceCollect = 0;
  threshold = std::max(kMinThreshold, trackedCount);
}

void gcCollectIfNeeded() {
#ifdef JOLT_GC_STRESS
  // Test builds collect at every safe point to flush out objects the collector
  // would free while they are still in use.
  gcCollect();
#else
  if (createdSinceCollect >= threshold) gcCollect();
#endif
}

std::size_t gcTrackedCount() { return trackedCount; }
