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

void LogosBlockchainModule::newBlock(const std::string& blockJson) {
    emitEventImpl_("newBlock", nullptr);
}
