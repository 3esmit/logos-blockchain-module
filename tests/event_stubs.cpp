// Test-only definitions for `logos_events:` methods.
//
// In a real build the cpp-generator emits the bodies of every `logos_events:`
// method into a sidecar `<name>_events.cpp` (each forwards through
// `emitEventImpl_`). The unit/integration test targets compile the module
// source directly without running codegen, so those bodies are absent and the
// link fails on `on_new_block_callback`'s reference to `newBlock`.
//
// `emitEventImpl_` is a no-op outside a framework-provisioned context (no emit
// callback is installed in tests), so forwarding here mirrors production
// behaviour while keeping the test build link-complete.

#include "logos_blockchain_module.h"

#include <mutex>
#include <string>
#include <vector>

std::string g_lastNewBlockJson;

namespace {
    std::mutex nodeChangedEventsMutex;
    std::vector<std::string> nodeChangedEvents;
} // namespace

void reset_node_changed_events() {
    std::lock_guard<std::mutex> lock(nodeChangedEventsMutex);
    nodeChangedEvents.clear();
}

std::vector<std::string> node_changed_events() {
    std::lock_guard<std::mutex> lock(nodeChangedEventsMutex);
    return nodeChangedEvents;
}

void LogosBlockchainModule::nodeChanged(const std::string& event) {
    std::lock_guard<std::mutex> lock(nodeChangedEventsMutex);
    nodeChangedEvents.push_back(event);
}

void LogosBlockchainModule::newBlock(const std::string& blockJson) {
    g_lastNewBlockJson = blockJson;
    emitEventImpl_("newBlock", nullptr);
}
