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

// Recorded payloads of the most recent stream events, so tests can assert what
// the trampolines forwarded (including the `null` end-of-stream sentinel).
std::string g_lastNewBlockEventJson;
std::string g_lastProcessedBlockEventJson;
std::string g_lastLibBlockEventJson;

void LogosBlockchainModule::newBlock(const std::string& blockJson) {
    g_lastNewBlockEventJson = blockJson;
    emitEventImpl_("newBlock", nullptr);
}

void LogosBlockchainModule::processedBlock(const std::string& eventJson) {
    g_lastProcessedBlockEventJson = eventJson;
    emitEventImpl_("processedBlock", nullptr);
}

void LogosBlockchainModule::libBlock(const std::string& blockInfoJson) {
    g_lastLibBlockEventJson = blockInfoJson;
    emitEventImpl_("libBlock", nullptr);
}
