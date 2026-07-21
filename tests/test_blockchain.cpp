// Unit tests for LogosBlockchainModule.
// All logos_blockchain C functions are mocked at link time via mock_logos_blockchain.cpp.

#include <logos_test.h>
#include "logos_blockchain_module.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// 64-char hex string = 32 bytes (valid address/hash)
static const std::string VALID_HEX(64, 'a');
static const std::string VALID_HEX_WITH_PREFIX = "0x" + std::string(64, 'b');

static bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

static std::string test_hex_id(const size_t value) {
    char id[65] = {};
    std::snprintf(id, sizeof(id), "%064zx", value);
    return id;
}

// RAII wrapper for a temporary directory (removed on destruction).
struct TempDir {
    fs::path path;
    TempDir() {
        char tmpl[] = "/tmp/logos-blockchain-test-XXXXXX";
        char* dir = mkdtemp(tmpl);
        if (dir) path = dir;
    }
    ~TempDir() {
        if (!path.empty()) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
    bool isValid() const { return !path.empty(); }
    std::string filePath(const std::string& name) const { return (path / name).string(); }
};

// Helper: create a module with a running (mocked) node.
static LogosBlockchainModule* createStartedModule(LogosTestContext& t, TempDir& tmpDir) {
    auto* module = new LogosBlockchainModule();

    t.mockCFunction("start_lb_node").returns(1);
    t.mockCFunction("subscribe_to_new_blocks").returns(0);

    StdLogosResult rc = module->start(tmpDir.filePath("config.json"), "");
    if (!rc.success) {
        delete module;
        return nullptr;
    }
    return module;
}

// ============================================================================
// generate_user_config
// ============================================================================

LOGOS_TEST(generate_user_config_returns_0_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_user_config").returns(0);

    LOGOS_ASSERT_TRUE(module.generate_user_config(R"({"output":"/tmp/test-config.json"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("generate_user_config"));
}

LOGOS_TEST(generate_user_config_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_user_config").returns(1);

    LOGOS_ASSERT_FALSE(module.generate_user_config("{}").success);
}

LOGOS_TEST(generate_user_config_from_json_string) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_user_config").returns(0);

    LOGOS_ASSERT_TRUE(module.generate_user_config(R"({"output":"/tmp/out.json"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("generate_user_config"));
}

// The mock records the paths handed to the FFI (see mock_logos_blockchain.cpp).
extern std::string g_lastGeneratedOutput;
extern std::string g_lastGeneratedStatePath;
extern std::string g_lastGeneratedStoragePath;
extern std::string g_lastGeneratedLogsPath;
extern std::string g_lastNewBlockJson;
extern void set_mock_get_block_responses(std::vector<std::string> responses);
extern void clear_mock_get_block_responses();
extern void trigger_mock_new_block(const char* block_json);

struct ScopedMockGetBlockResponses {
    explicit ScopedMockGetBlockResponses(std::vector<std::string> responses) {
        set_mock_get_block_responses(std::move(responses));
    }

    ~ScopedMockGetBlockResponses() {
        clear_mock_get_block_responses();
    }
};

static void clearGeneratedPaths() {
    g_lastGeneratedOutput.clear();
    g_lastGeneratedStatePath.clear();
    g_lastGeneratedStoragePath.clear();
    g_lastGeneratedLogsPath.clear();
}

// Basecamp opts in with the flag: output and state/storage/logs are routed under
// the per-instance persistence dir. The output keeps its given path as the
// relative part below the base.
LOGOS_TEST(generate_user_config_routes_paths_under_persistence_when_flagged) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    module._logosCoreSetContext_("/mod", "id", "/persist/data");

    t.mockCFunction("generate_user_config").returns(0);
    clearGeneratedPaths();

    StdLogosResult result =
        module.generate_user_config(R"({"output":"config/user_config.yaml","use_persistence_paths":true})");
    LOGOS_ASSERT_TRUE(result.success);
    // The resolved config path is returned for the caller to hand to start().
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("/persist/data/config/user_config.yaml"));
    LOGOS_ASSERT_EQ(g_lastGeneratedOutput, std::string("/persist/data/config/user_config.yaml"));
    LOGOS_ASSERT_EQ(g_lastGeneratedStatePath, std::string("/persist/data/state"));
    LOGOS_ASSERT_EQ(g_lastGeneratedStoragePath, std::string("/persist/data/db"));
    LOGOS_ASSERT_EQ(g_lastGeneratedLogsPath, std::string("/persist/data/logs"));
}

// A root-anchored output (the UI's "//user_config.yaml" artifact) is treated as
// relative to the base, not written to the read-only filesystem root.
LOGOS_TEST(generate_user_config_routes_root_anchored_output_under_persistence) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    module._logosCoreSetContext_("/mod", "id", "/persist/data");

    t.mockCFunction("generate_user_config").returns(0);
    clearGeneratedPaths();

    LOGOS_ASSERT_TRUE(
        module.generate_user_config(R"({"output":"//user_config.yaml","use_persistence_paths":true})").success);
    LOGOS_ASSERT_EQ(g_lastGeneratedOutput, std::string("/persist/data/user_config.yaml"));
}

// With no output given, the config defaults to <base>/user_config.yaml.
LOGOS_TEST(generate_user_config_defaults_output_under_persistence) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    module._logosCoreSetContext_("/mod", "id", "/persist/data");

    t.mockCFunction("generate_user_config").returns(0);
    clearGeneratedPaths();

    LOGOS_ASSERT_TRUE(module.generate_user_config(R"({"use_persistence_paths":true})").success);
    LOGOS_ASSERT_EQ(g_lastGeneratedOutput, std::string("/persist/data/user_config.yaml"));
}

// A host load WITHOUT the flag (e.g. logoscore-cli) does NOT redirect any path,
// even though the persistence path is populated.
LOGOS_TEST(generate_user_config_host_without_flag_leaves_paths_unset) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    module._logosCoreSetContext_("/mod", "id", "/persist/data");

    t.mockCFunction("generate_user_config").returns(0);
    clearGeneratedPaths();

    StdLogosResult result = module.generate_user_config(R"({"output":"/tmp/out.json"})");
    LOGOS_ASSERT_TRUE(result.success);
    // Without routing, the caller's output is returned unchanged.
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("/tmp/out.json"));
    LOGOS_ASSERT_EQ(g_lastGeneratedOutput, std::string("/tmp/out.json"));
    LOGOS_ASSERT_EQ(g_lastGeneratedStatePath, std::string("<null>"));
    LOGOS_ASSERT_EQ(g_lastGeneratedStoragePath, std::string("<null>"));
    LOGOS_ASSERT_EQ(g_lastGeneratedLogsPath, std::string("<null>"));
}

// An explicitly provided path wins over the flag; the others are still routed.
LOGOS_TEST(generate_user_config_explicit_path_wins_over_flag) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    module._logosCoreSetContext_("/mod", "id", "/persist/data");

    t.mockCFunction("generate_user_config").returns(0);
    clearGeneratedPaths();

    LOGOS_ASSERT_TRUE(
        module.generate_user_config(
            R"({"output":"/tmp/out.json","state_path":"/tmp/state","use_persistence_paths":true})").success);
    LOGOS_ASSERT_EQ(g_lastGeneratedStatePath, std::string("/tmp/state"));
    LOGOS_ASSERT_EQ(g_lastGeneratedStoragePath, std::string("/persist/data/db"));
    LOGOS_ASSERT_EQ(g_lastGeneratedLogsPath, std::string("/persist/data/logs"));
}

// The flag with no host context (standalone) leaves paths unset.
LOGOS_TEST(generate_user_config_flag_without_context_leaves_paths_unset) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_user_config").returns(0);
    clearGeneratedPaths();

    LOGOS_ASSERT_TRUE(
        module.generate_user_config(R"({"output":"/tmp/out.json","use_persistence_paths":true})").success);
    LOGOS_ASSERT_EQ(g_lastGeneratedOutput, std::string("/tmp/out.json"));
    LOGOS_ASSERT_EQ(g_lastGeneratedStatePath, std::string("<null>"));
    LOGOS_ASSERT_EQ(g_lastGeneratedStoragePath, std::string("<null>"));
    LOGOS_ASSERT_EQ(g_lastGeneratedLogsPath, std::string("<null>"));
}

LOGOS_TEST(generate_user_config_with_all_fields) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_user_config").returns(0);

    std::string args = R"({
        "initial_peers": ["peer1", "peer2"],
        "output": "/tmp/out.json",
        "net_port": 9000,
        "blend_port": 9001,
        "http_addr": "0.0.0.0:8080",
        "external_address": "1.2.3.4",
        "state_path": "/tmp/state",
        "ibd": true,
        "log_filter": "warn,logos_blockchain=debug",
        "kms_file": "/tmp/kms.yaml"
    })";

    LOGOS_ASSERT_TRUE(module.generate_user_config(args).success);
}

// ============================================================================
// No-node error paths — all methods should fail gracefully
// ============================================================================

LOGOS_TEST(stop_without_node_returns_1) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_FALSE(module.stop().success);
}

LOGOS_TEST(wallet_get_balance_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    StdLogosResult result = module.wallet_get_balance(VALID_HEX);
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "not running"));
}

LOGOS_TEST(wallet_transfer_funds_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    StdLogosResult result = module.wallet_transfer_funds(VALID_HEX, {VALID_HEX}, VALID_HEX, "100", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "not running"));
}

LOGOS_TEST(leader_claim_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    StdLogosResult result = module.leader_claim();
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "not running"));
}

LOGOS_TEST(channel_deposit_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    StdLogosResult result = module.channel_deposit(VALID_HEX, VALID_HEX, "100", "", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "not running"));
}

LOGOS_TEST(channel_deposit_with_notes_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    StdLogosResult result = module.channel_deposit_with_notes(
        VALID_HEX, {VALID_HEX}, "", VALID_HEX, {VALID_HEX}, "0", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "not running"));
}

LOGOS_TEST(wallet_get_notes_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    StdLogosResult result = module.wallet_get_notes(VALID_HEX, "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "not running"));
}

LOGOS_TEST(wallet_get_known_addresses_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_FALSE(module.wallet_get_known_addresses().success);
}

LOGOS_TEST(blend_join_as_core_node_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    StdLogosResult result = module.blend_join_as_core_node("locator1", VALID_HEX);
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "not running"));
}

LOGOS_TEST(get_block_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_FALSE(module.get_block(VALID_HEX).success);
}

LOGOS_TEST(get_blocks_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_FALSE(module.get_blocks(0, 10).success);
}

LOGOS_TEST(get_transaction_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_FALSE(module.get_transaction(VALID_HEX).success);
}

LOGOS_TEST(get_cryptarchia_info_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_FALSE(module.get_cryptarchia_info().success);
}

// ============================================================================
// Node lifecycle (start / stop)
// ============================================================================

LOGOS_TEST(start_succeeds_with_mocked_dependencies) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    LOGOS_ASSERT_TRUE(tmpDir.isValid());

    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);
    LOGOS_ASSERT(t.cFunctionCalled("start_lb_node"));
    LOGOS_ASSERT(t.cFunctionCalled("subscribe_to_new_blocks"));
    delete module;
}

LOGOS_TEST(start_returns_1_when_already_running) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    LOGOS_ASSERT_FALSE(module->start("/tmp/config.json", "").success);
    delete module;
}

LOGOS_TEST(stop_succeeds_with_running_node) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    LOGOS_ASSERT_TRUE(module->stop().success);
    LOGOS_ASSERT(t.cFunctionCalled("shutdown_node"));
    delete module;
}

// ============================================================================
// Input validation (requires running node)
// ============================================================================

// wallet_get_balance validation

LOGOS_TEST(wallet_get_balance_rejects_short_hex) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->wallet_get_balance("abcd");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "64 hex"));
    delete module;
}

LOGOS_TEST(wallet_get_balance_rejects_long_hex) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->wallet_get_balance(std::string(66, 'a'));
    LOGOS_ASSERT_FALSE(result.success);
    delete module;
}

LOGOS_TEST(wallet_get_balance_rejects_invalid_chars) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    std::string hex = std::string(62, 'a') + "zz";
    StdLogosResult result = module->wallet_get_balance(hex);
    LOGOS_ASSERT_FALSE(result.success);
    delete module;
}

// wallet_transfer_funds validation

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_amount) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->wallet_transfer_funds(VALID_HEX, {VALID_HEX}, VALID_HEX, "not_a_number", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "Invalid amount"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_change_key) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->wallet_transfer_funds("bad", {VALID_HEX}, VALID_HEX, "100", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "change_public_key"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_recipient) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->wallet_transfer_funds(VALID_HEX, {VALID_HEX}, "short", "100", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "recipient_address"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_empty_senders) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->wallet_transfer_funds(VALID_HEX, {}, VALID_HEX, "100", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "sender"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_sender_address) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->wallet_transfer_funds(VALID_HEX, {"bad_addr"}, VALID_HEX, "100", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "sender"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_optional_tip) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->wallet_transfer_funds(VALID_HEX, {VALID_HEX}, VALID_HEX, "100", "bad_tip");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "tip"));
    delete module;
}

// blend_join_as_core_node validation

LOGOS_TEST(blend_join_rejects_empty_locator) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->blend_join_as_core_node("", VALID_HEX);
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "locator"));
    delete module;
}

LOGOS_TEST(blend_join_rejects_invalid_locked_note_id) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->blend_join_as_core_node("locator1", "short");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "locked_note_id"));
    delete module;
}

// get_block / get_transaction validation

LOGOS_TEST(get_block_rejects_invalid_hex) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->get_block("tooshort");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "64 hex"));
    delete module;
}

LOGOS_TEST(get_transaction_rejects_invalid_hex) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->get_transaction("bad");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "64 hex"));
    delete module;
}

// ============================================================================
// 0x prefix handling
// ============================================================================

LOGOS_TEST(wallet_get_balance_accepts_0x_prefix) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_balance_value").returns(42);
    t.mockCFunction("get_balance_error").returns(0);

    StdLogosResult result = module->wallet_get_balance(VALID_HEX_WITH_PREFIX);
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("42"));
    delete module;
}

// ============================================================================
// Success paths (requires running node + mocked C functions)
// ============================================================================

// Wallet

LOGOS_TEST(wallet_get_balance_returns_balance_string) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_balance_value").returns(1000);
    t.mockCFunction("get_balance_error").returns(0);

    StdLogosResult result = module->wallet_get_balance(VALID_HEX);
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("1000"));
    LOGOS_ASSERT(t.cFunctionCalled("get_balance"));
    delete module;
}

LOGOS_TEST(wallet_get_balance_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_balance_error").returns(1);

    StdLogosResult result = module->wallet_get_balance(VALID_HEX);
    LOGOS_ASSERT_FALSE(result.success);
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_returns_tx_hash) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("transfer_funds_error").returns(0);

    StdLogosResult result = module->wallet_transfer_funds(VALID_HEX, {VALID_HEX}, VALID_HEX, "500", "");
    LOGOS_ASSERT_TRUE(result.success);
    std::string hash = result.value.get<std::string>();
    LOGOS_ASSERT_EQ(static_cast<int>(hash.length()), 64);
    LOGOS_ASSERT_TRUE(contains(hash.substr(0, 2), "ab"));
    LOGOS_ASSERT(t.cFunctionCalled("transfer_funds"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_with_optional_tip) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("transfer_funds_error").returns(0);

    StdLogosResult result = module->wallet_transfer_funds(VALID_HEX, {VALID_HEX}, VALID_HEX, "100", VALID_HEX);
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(static_cast<int>(result.value.get<std::string>().length()), 64);
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("transfer_funds_error").returns(1);

    StdLogosResult result = module->wallet_transfer_funds(VALID_HEX, {VALID_HEX}, VALID_HEX, "100", "");
    LOGOS_ASSERT_FALSE(result.success);
    delete module;
}

LOGOS_TEST(leader_claim_returns_tx_hash) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("leader_claim_error").returns(0);

    StdLogosResult result = module->leader_claim();
    LOGOS_ASSERT_TRUE(result.success);
    std::string hash = result.value.get<std::string>();
    LOGOS_ASSERT_EQ(static_cast<int>(hash.length()), 64);
    LOGOS_ASSERT_TRUE(hash.substr(0, 2) == "ef");
    LOGOS_ASSERT(t.cFunctionCalled("leader_claim"));
    delete module;
}

LOGOS_TEST(leader_claim_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("leader_claim_error").returns(1);

    StdLogosResult result = module->leader_claim();
    LOGOS_ASSERT_FALSE(result.success);
    delete module;
}

LOGOS_TEST(channel_deposit_returns_tx_hash) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("channel_deposit_error").returns(0);

    StdLogosResult result = module->channel_deposit(VALID_HEX, VALID_HEX, "500", "", "");
    LOGOS_ASSERT_TRUE(result.success);
    std::string hash = result.value.get<std::string>();
    LOGOS_ASSERT_EQ(static_cast<int>(hash.length()), 64);
    LOGOS_ASSERT_TRUE(hash.substr(0, 2) == "bc");
    LOGOS_ASSERT(t.cFunctionCalled("channel_deposit"));
    delete module;
}

LOGOS_TEST(channel_deposit_with_metadata_and_tip) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("channel_deposit_error").returns(0);

    StdLogosResult result = module->channel_deposit(VALID_HEX, VALID_HEX, "100", "deadbeef", VALID_HEX);
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(static_cast<int>(result.value.get<std::string>().length()), 64);
    delete module;
}

LOGOS_TEST(channel_deposit_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("channel_deposit_error").returns(1);

    StdLogosResult result = module->channel_deposit(VALID_HEX, VALID_HEX, "100", "", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "mock error"));
    delete module;
}

LOGOS_TEST(channel_deposit_rejects_invalid_amount) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->channel_deposit(VALID_HEX, VALID_HEX, "not_a_number", "", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "amount"));
    delete module;
}

LOGOS_TEST(channel_deposit_rejects_zero_amount) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->channel_deposit(VALID_HEX, VALID_HEX, "0", "", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "amount"));
    delete module;
}

LOGOS_TEST(channel_deposit_rejects_invalid_channel_id) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->channel_deposit("bad", VALID_HEX, "100", "", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "channel_id"));
    delete module;
}

LOGOS_TEST(channel_deposit_rejects_invalid_funding_key) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->channel_deposit(VALID_HEX, "short", "100", "", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "funding_public_key"));
    delete module;
}

LOGOS_TEST(channel_deposit_rejects_invalid_metadata) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->channel_deposit(VALID_HEX, VALID_HEX, "100", "xyz", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "metadata"));
    delete module;
}

LOGOS_TEST(channel_deposit_rejects_invalid_optional_tip) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->channel_deposit(VALID_HEX, VALID_HEX, "100", "", "bad_tip");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "tip"));
    delete module;
}

LOGOS_TEST(wallet_get_notes_returns_json_on_success) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_wallet_notes_error").returns(0);
    t.mockCFunction("get_wallet_notes_count").returns(2);

    StdLogosResult result = module->wallet_get_notes(VALID_HEX, "");
    LOGOS_ASSERT_TRUE(result.success);
    std::string json = result.value.get<std::string>();
    LOGOS_ASSERT_TRUE(contains(json, "\"tip\""));
    LOGOS_ASSERT_TRUE(contains(json, "\"notes\""));
    LOGOS_ASSERT_TRUE(contains(json, "\"value\":\"100\""));
    LOGOS_ASSERT_TRUE(contains(json, "\"value\":\"200\""));
    LOGOS_ASSERT(t.cFunctionCalled("get_wallet_notes"));
    LOGOS_ASSERT(t.cFunctionCalled("free_wallet_notes"));
    delete module;
}

LOGOS_TEST(wallet_get_notes_returns_empty_notes_array) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_wallet_notes_error").returns(0);
    t.mockCFunction("get_wallet_notes_count").returns(0);

    StdLogosResult result = module->wallet_get_notes(VALID_HEX, "");
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.value.get<std::string>(), "\"notes\":[]"));
    delete module;
}

LOGOS_TEST(wallet_get_notes_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_wallet_notes_error").returns(1);

    StdLogosResult result = module->wallet_get_notes(VALID_HEX, "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "mock error"));
    delete module;
}

LOGOS_TEST(wallet_get_notes_rejects_invalid_address) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->wallet_get_notes("bad", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "wallet address"));
    delete module;
}

LOGOS_TEST(channel_deposit_with_notes_returns_tx_hash) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("channel_deposit_with_notes_error").returns(0);

    StdLogosResult result = module->channel_deposit_with_notes(
        VALID_HEX, {VALID_HEX}, "deadbeef", VALID_HEX, {VALID_HEX}, "1000", "");
    LOGOS_ASSERT_TRUE(result.success);
    std::string hash = result.value.get<std::string>();
    LOGOS_ASSERT_EQ(static_cast<int>(hash.length()), 64);
    LOGOS_ASSERT_TRUE(hash.substr(0, 2) == "de");
    LOGOS_ASSERT(t.cFunctionCalled("channel_deposit_with_notes"));
    delete module;
}

LOGOS_TEST(channel_deposit_with_notes_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("channel_deposit_with_notes_error").returns(1);

    StdLogosResult result = module->channel_deposit_with_notes(
        VALID_HEX, {VALID_HEX}, "", VALID_HEX, {VALID_HEX}, "0", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "mock error"));
    delete module;
}

LOGOS_TEST(channel_deposit_with_notes_rejects_empty_notes) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->channel_deposit_with_notes(
        VALID_HEX, {}, "", VALID_HEX, {VALID_HEX}, "0", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "input note"));
    delete module;
}

LOGOS_TEST(channel_deposit_with_notes_rejects_invalid_note_id) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->channel_deposit_with_notes(
        VALID_HEX, {"bad"}, "", VALID_HEX, {VALID_HEX}, "0", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "input note id"));
    delete module;
}

LOGOS_TEST(channel_deposit_with_notes_rejects_empty_funding_keys) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->channel_deposit_with_notes(
        VALID_HEX, {VALID_HEX}, "", VALID_HEX, {}, "0", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "funding public key"));
    delete module;
}

LOGOS_TEST(channel_deposit_with_notes_rejects_invalid_max_tx_fee) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->channel_deposit_with_notes(
        VALID_HEX, {VALID_HEX}, "", VALID_HEX, {VALID_HEX}, "not_a_number", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "max_tx_fee"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_single_sender_via_vector) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("transfer_funds_error").returns(0);

    StdLogosResult result = module->wallet_transfer_funds(VALID_HEX, {VALID_HEX}, VALID_HEX, "100", "");
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT(t.cFunctionCalled("transfer_funds"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_multiple_senders) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("transfer_funds_error").returns(0);

    std::vector<std::string> senders = {VALID_HEX, std::string(64, 'b')};
    StdLogosResult result = module->wallet_transfer_funds(VALID_HEX, senders, VALID_HEX, "200", "");
    LOGOS_ASSERT_TRUE(result.success);
    delete module;
}

LOGOS_TEST(wallet_get_known_addresses_returns_addresses) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_known_addresses_error").returns(0);
    t.mockCFunction("get_known_addresses_count").returns(2);

    StdLogosResult result = module->wallet_get_known_addresses();
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(static_cast<int>(result.value.size()), 2);
    // Mock fills addr0 with 0x11 -> hex "1111...11", addr1 with 0x22 -> "2222...22"
    LOGOS_ASSERT_EQ(result.value[0].get<std::string>(), std::string(64, '1'));
    LOGOS_ASSERT_EQ(result.value[1].get<std::string>(), std::string(64, '2'));
    LOGOS_ASSERT(t.cFunctionCalled("get_known_addresses"));
    LOGOS_ASSERT(t.cFunctionCalled("free_known_addresses"));
    delete module;
}

LOGOS_TEST(wallet_get_known_addresses_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_known_addresses_error").returns(1);

    StdLogosResult result = module->wallet_get_known_addresses();
    LOGOS_ASSERT_FALSE(result.success);
    delete module;
}

LOGOS_TEST(wallet_get_claimable_vouchers_returns_json) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_claimable_vouchers_error").returns(0);
    t.mockCFunction("get_claimable_vouchers_count").returns(2);

    StdLogosResult result = module->wallet_get_claimable_vouchers();
    LOGOS_ASSERT_TRUE(result.success);
    std::string json = result.value.get<std::string>();
    LOGOS_ASSERT_TRUE(contains(json, "\"vouchers\""));
    LOGOS_ASSERT_TRUE(contains(json, std::string(64, 'a')));
    LOGOS_ASSERT_TRUE(contains(json, std::string(64, '1')));
    LOGOS_ASSERT_TRUE(contains(json, "20202020"));
    LOGOS_ASSERT(t.cFunctionCalled("get_claimable_vouchers"));
    LOGOS_ASSERT(t.cFunctionCalled("free_claimable_vouchers"));
    delete module;
}

LOGOS_TEST(wallet_get_claimable_vouchers_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_claimable_vouchers_error").returns(1);

    StdLogosResult result = module->wallet_get_claimable_vouchers();
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "mock error"));
    delete module;
}

// Blend

LOGOS_TEST(blend_join_as_core_node_returns_declaration_id) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("blend_join_as_core_node_error").returns(0);

    StdLogosResult result = module->blend_join_as_core_node("locator1", VALID_HEX);
    LOGOS_ASSERT_TRUE(result.success);
    // Mock fills hash with 0xCD -> hex "cdcd...cd" (64 chars)
    std::string declarationId = result.value.get<std::string>();
    LOGOS_ASSERT_EQ(static_cast<int>(declarationId.length()), 64);
    LOGOS_ASSERT_TRUE(declarationId.substr(0, 2) == "cd");
    LOGOS_ASSERT(t.cFunctionCalled("blend_join_as_core_node"));
    delete module;
}

LOGOS_TEST(blend_join_as_core_node_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("blend_join_as_core_node_error").returns(1);

    StdLogosResult result = module->blend_join_as_core_node("locator1", VALID_HEX);
    LOGOS_ASSERT_FALSE(result.success);
    delete module;
}

// Explorer

LOGOS_TEST(get_block_returns_json_on_success) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_block").returns(R"({"slot":42,"data":"test"})");
    t.mockCFunction("get_block_error").returns(0);

    StdLogosResult result = module->get_block(VALID_HEX);
    LOGOS_ASSERT_TRUE(result.success);
    std::string json = result.value.get<std::string>();
    LOGOS_ASSERT_TRUE(contains(json, "slot"));
    LOGOS_ASSERT_TRUE(contains(json, "42"));
    LOGOS_ASSERT(t.cFunctionCalled("get_block"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
    delete module;
}

LOGOS_TEST(get_block_adds_requested_canonical_header_id) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_block").returns(
        R"({"header":{"slot":42,"parent_block":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"transactions":[]})"
    );
    t.mockCFunction("get_block_error").returns(0);

    const std::string requested = "0x" + std::string(64, 'B');
    StdLogosResult result = module->get_block(requested);
    LOGOS_ASSERT_TRUE(result.success);

    const json block = json::parse(result.value.get<std::string>());
    LOGOS_ASSERT_EQ(block["header"]["id"].get<std::string>(), std::string(64, 'b'));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
    delete module;
}

LOGOS_TEST(get_block_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_block_error").returns(1);

    StdLogosResult result = module->get_block(VALID_HEX);
    LOGOS_ASSERT_FALSE(result.success);
    delete module;
}

LOGOS_TEST(explorer_empty_responses_identify_the_called_method) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_block").returns(static_cast<const char*>(nullptr));
    t.mockCFunction("get_block_error").returns(0);
    const StdLogosResult block = module->get_block(VALID_HEX);
    LOGOS_ASSERT_FALSE(block.success);
    LOGOS_ASSERT_EQ(block.error, std::string("get_block returned an empty response."));

    t.mockCFunction("get_blocks").returns(static_cast<const char*>(nullptr));
    t.mockCFunction("get_blocks_error").returns(0);
    const StdLogosResult blocks = module->get_blocks(1, 10);
    LOGOS_ASSERT_FALSE(blocks.success);
    LOGOS_ASSERT_EQ(blocks.error, std::string("get_blocks returned an empty response."));

    t.mockCFunction("get_transaction").returns(static_cast<const char*>(nullptr));
    t.mockCFunction("get_transaction_error").returns(0);
    const StdLogosResult transaction = module->get_transaction(VALID_HEX);
    LOGOS_ASSERT_FALSE(transaction.success);
    LOGOS_ASSERT_EQ(
        transaction.error,
        std::string("get_transaction returned an empty response.")
    );
    delete module;
}

LOGOS_TEST(get_blocks_returns_json_on_success) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_blocks").returns(R"([{"slot":1},{"slot":2}])");
    t.mockCFunction("get_blocks_error").returns(0);

    StdLogosResult result = module->get_blocks(1, 10);
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.value.get<std::string>(), "slot"));
    LOGOS_ASSERT(t.cFunctionCalled("get_blocks"));
    delete module;
}

LOGOS_TEST(get_blocks_normalizes_core_transaction_id_to_mantle_hash) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    const std::string transaction_id = "0x" + std::string(64, 'C');
    const std::string response = std::string(R"([{"header":{"slot":1},"transactions":[{"id":")") + transaction_id +
                                 R"(","mantle_tx":{"ops":[]}}]}])";
    t.mockCFunction("get_blocks").returns(response.c_str());
    t.mockCFunction("get_blocks_error").returns(0);

    const StdLogosResult result = module->get_blocks(1, 10);
    LOGOS_ASSERT_TRUE(result.success);
    const json blocks = json::parse(result.value.get<std::string>());
    LOGOS_ASSERT_EQ(blocks[0]["transactions"][0]["mantle_tx"]["hash"].get<std::string>(), std::string(64, 'c'));
    delete module;
}

LOGOS_TEST(get_blocks_preserves_raw_json_when_no_transaction_hash_is_added) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    const std::string transaction_id(64, 'a');
    const std::string response = std::string(
        R"([ { "header" : { "slot" : 1 }, "transactions" : [ { "id" : ")")
        + transaction_id + R"(", "mantle_tx" : { "hash" : ")" + transaction_id
        + R"(" } } ] } ])";
    t.mockCFunction("get_blocks").returns(response.c_str());
    t.mockCFunction("get_blocks_error").returns(0);

    const StdLogosResult result = module->get_blocks(1, 10);
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), response);
    delete module;
}

LOGOS_TEST(get_blocks_omits_mantle_hash_for_malformed_core_transaction_id) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    const std::string malformed_id = "0x" + std::string(63, 'c') + "g";
    const std::string response = std::string(R"([{"header":{"slot":1},"transactions":[{"id":")")
        + malformed_id + R"(","mantle_tx":{"ops":[]}}]}])";
    t.mockCFunction("get_blocks").returns(response.c_str());
    t.mockCFunction("get_blocks_error").returns(0);

    const StdLogosResult result = module->get_blocks(1, 10);
    LOGOS_ASSERT_TRUE(result.success);
    const json blocks = json::parse(result.value.get<std::string>());
    LOGOS_ASSERT_FALSE(blocks[0]["transactions"][0]["mantle_tx"].contains("hash"));
    delete module;
}

LOGOS_TEST(get_blocks_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_blocks_error").returns(1);

    LOGOS_ASSERT_FALSE(module->get_blocks(0, 10).success);
    delete module;
}

LOGOS_TEST(get_blocks_rejects_reversed_range_before_ffi) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->get_blocks(130, 100);
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "from_slot"));
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("get_blocks"));
    delete module;
}

LOGOS_TEST(get_blocks_walks_live_tip_when_immutable_range_is_empty) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_blocks").returns("[]");
    t.mockCFunction("get_blocks_error").returns(0);
    t.mockCFunction("get_cryptarchia_info_error").returns(0);
    t.mockCFunction("cryptarchia_slot").returns(130);
    t.mockCFunction("get_block_error").returns(0);
    ScopedMockGetBlockResponses responses({
        R"({"header":{"slot":130,"parent_block":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},"transactions":[]})",
        R"({"header":{"slot":115,"parent_block":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"},"transactions":[]})",
        R"({"header":{"slot":90,"parent_block":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"transactions":[]})",
    });

    StdLogosResult result = module->get_blocks(100, 130);
    LOGOS_ASSERT_TRUE(result.success);
    const json blocks = json::parse(result.value.get<std::string>());
    LOGOS_ASSERT_EQ(blocks.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(blocks[0]["header"]["slot"].get<uint64_t>(), static_cast<uint64_t>(115));
    LOGOS_ASSERT_EQ(blocks[0]["header"]["id"].get<std::string>(), std::string(64, 'b'));
    LOGOS_ASSERT_EQ(blocks[1]["header"]["slot"].get<uint64_t>(), static_cast<uint64_t>(130));
    LOGOS_ASSERT_EQ(blocks[1]["header"]["id"].get<std::string>(), std::string(64, 'f'));
    LOGOS_ASSERT(t.cFunctionCalled("get_cryptarchia_info"));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("get_block"), 3);
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("free_cstring"), 4);
    LOGOS_ASSERT(t.cFunctionCalled("free_cryptarchia_info"));
    delete module;
}

LOGOS_TEST(get_blocks_rejects_a_repeated_live_parent) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_blocks").returns("[]");
    t.mockCFunction("get_blocks_error").returns(0);
    t.mockCFunction("get_cryptarchia_info_error").returns(0);
    t.mockCFunction("cryptarchia_slot").returns(130);
    t.mockCFunction("get_block_error").returns(0);
    ScopedMockGetBlockResponses responses({
        R"({"header":{"slot":130,"parent_block":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},"transactions":[]})",
    });

    StdLogosResult result = module->get_blocks(100, 130);
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "repeated parent"));
    delete module;
}

LOGOS_TEST(get_blocks_rejects_a_truncated_live_parent_walk) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_blocks").returns("[]");
    t.mockCFunction("get_blocks_error").returns(0);
    t.mockCFunction("get_cryptarchia_info_error").returns(0);
    t.mockCFunction("cryptarchia_slot").returns(500);
    t.mockCFunction("get_block_error").returns(0);

    std::vector<std::string> responses;
    responses.reserve(500);
    for (size_t index = 0; index < 500; ++index) {
        responses.push_back(
            R"({"header":{"slot":500,"parent_block":")" + test_hex_id(index + 1) +
            R"("},"transactions":[]})"
        );
    }
    ScopedMockGetBlockResponses scoped_responses(std::move(responses));

    StdLogosResult result = module->get_blocks(1, 500);
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "traversal limit"));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("get_block"), 500);
    delete module;
}

LOGOS_TEST(get_transaction_returns_json_on_success) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_transaction").returns(R"({"status":"confirmed"})");
    t.mockCFunction("get_transaction_error").returns(0);

    StdLogosResult result = module->get_transaction(VALID_HEX);
    LOGOS_ASSERT_TRUE(result.success);
    const json transaction = json::parse(result.value.get<std::string>());
    LOGOS_ASSERT_EQ(transaction["status"].get<std::string>(), std::string("confirmed"));
    LOGOS_ASSERT_EQ(transaction["hash"].get<std::string>(), VALID_HEX);
    LOGOS_ASSERT(t.cFunctionCalled("get_transaction"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
    delete module;
}

LOGOS_TEST(get_transaction_adds_requested_canonical_hash) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_transaction").returns(R"({"mantle_tx":{"ops":[]},"ops_proofs":[]})");
    t.mockCFunction("get_transaction_error").returns(0);

    const std::string requested = "0x" + std::string(64, 'C');
    StdLogosResult result = module->get_transaction(requested);
    LOGOS_ASSERT_TRUE(result.success);
    const json transaction = json::parse(result.value.get<std::string>());
    LOGOS_ASSERT_EQ(transaction["mantle_tx"]["hash"].get<std::string>(), std::string(64, 'c'));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
    delete module;
}

LOGOS_TEST(get_transaction_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_transaction_error").returns(1);

    LOGOS_ASSERT_FALSE(module->get_transaction(VALID_HEX).success);
    delete module;
}

// Cryptarchia

LOGOS_TEST(get_cryptarchia_info_returns_json_on_success) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_cryptarchia_info_error").returns(0);
    t.mockCFunction("cryptarchia_slot").returns(100);
    t.mockCFunction("cryptarchia_height").returns(50);
    t.mockCFunction("cryptarchia_mode").returns(1); // Online
    t.mockCFunction("get_block_error").returns(0);
    ScopedMockGetBlockResponses responses({R"({"header":{"slot":99},"transactions":[]})"});

    StdLogosResult result = module->get_cryptarchia_info();
    LOGOS_ASSERT_TRUE(result.success);
    const json info = json::parse(result.value.get<std::string>());
    LOGOS_ASSERT_EQ(info["slot"].get<uint64_t>(), static_cast<uint64_t>(100));
    LOGOS_ASSERT_EQ(info["height"].get<uint64_t>(), static_cast<uint64_t>(50));
    LOGOS_ASSERT_EQ(info["mode"].get<std::string>(), std::string("Online"));
    LOGOS_ASSERT_TRUE(info.contains("lib"));
    LOGOS_ASSERT_TRUE(info.contains("tip"));
    LOGOS_ASSERT_EQ(info["lib_slot"].get<uint64_t>(), static_cast<uint64_t>(99));
    LOGOS_ASSERT(t.cFunctionCalled("get_cryptarchia_info"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cryptarchia_info"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
    delete module;
}

LOGOS_TEST(get_cryptarchia_info_bootstrapping_mode) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_cryptarchia_info_error").returns(0);
    t.mockCFunction("cryptarchia_mode").returns(0); // Bootstrapping
    t.mockCFunction("get_block_error").returns(0);
    ScopedMockGetBlockResponses responses({R"({"header":{"slot":0},"transactions":[]})"});

    StdLogosResult result = module->get_cryptarchia_info();
    LOGOS_ASSERT_TRUE(contains(result.value.get<std::string>(), "Bootstrapping"));
    delete module;
}

LOGOS_TEST(get_cryptarchia_info_preserves_not_started_mode) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_cryptarchia_info_error").returns(0);
    t.mockCFunction("cryptarchia_mode").returns(2); // NotStarted
    t.mockCFunction("get_block_error").returns(0);
    ScopedMockGetBlockResponses responses({R"({"header":{"slot":0},"transactions":[]})"});

    StdLogosResult result = module->get_cryptarchia_info();
    LOGOS_ASSERT_TRUE(result.success);
    const json info = json::parse(result.value.get<std::string>());
    LOGOS_ASSERT_EQ(info["mode"].get<std::string>(), std::string("NotStarted"));
    delete module;
}

LOGOS_TEST(get_cryptarchia_info_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_cryptarchia_info_error").returns(1);

    LOGOS_ASSERT_FALSE(module->get_cryptarchia_info().success);
    delete module;
}

LOGOS_TEST(new_block_event_emits_an_object_envelope_with_transaction_hashes) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    g_lastNewBlockJson.clear();
    const std::string transaction_id(64, 'd');
    const std::string event =
        R"({"header":{"slot":130},"transactions":[{"id":")" + transaction_id +
        R"(","mantle_tx":{"ops":[]}}]})";
    trigger_mock_new_block(event.c_str());

    const json emitted = json::parse(g_lastNewBlockJson);
    LOGOS_ASSERT_TRUE(emitted["block"].is_object());
    LOGOS_ASSERT_EQ(
        emitted["block"]["transactions"][0]["mantle_tx"]["hash"].get<std::string>(),
        transaction_id
    );
    delete module;
}

LOGOS_TEST(new_block_event_preserves_a_malformed_payload_as_a_string) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    g_lastNewBlockJson.clear();
    const std::string malformed_payload = "{not-json";
    trigger_mock_new_block(malformed_payload.c_str());

    const json emitted = json::parse(g_lastNewBlockJson);
    LOGOS_ASSERT_TRUE(emitted["block"].is_string());
    LOGOS_ASSERT_EQ(emitted["block"].get<std::string>(), malformed_payload);
    delete module;
}

// ============================================================================
// Config management (operate on file paths, no running node required)
// ============================================================================

LOGOS_TEST(update_user_config_returns_0_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("update_user_config").returns(0);

    LOGOS_ASSERT_TRUE(module.update_user_config("/tmp/config.yaml", "/tmp/keystore.yaml").success);
    LOGOS_ASSERT(t.cFunctionCalled("update_user_config"));
}

LOGOS_TEST(update_user_config_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("update_user_config").returns(1);

    LOGOS_ASSERT_FALSE(module.update_user_config("/tmp/config.yaml", "/tmp/keystore.yaml").success);
}

LOGOS_TEST(migrate_user_config_returns_0_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("migrate_user_config").returns(0);

    LOGOS_ASSERT_TRUE(module.migrate_user_config("/tmp/out.yaml", "/tmp/keystore.yaml").success);
    LOGOS_ASSERT(t.cFunctionCalled("migrate_user_config"));
}

LOGOS_TEST(migrate_user_config_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("migrate_user_config").returns(1);

    LOGOS_ASSERT_FALSE(module.migrate_user_config("/tmp/out.yaml", "/tmp/keystore.yaml").success);
}

LOGOS_TEST(migrate_user_config_0_1_2_returns_0_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("migrate_user_config_0_1_2").returns(0);

    LOGOS_ASSERT_TRUE(module.migrate_user_config_0_1_2("/tmp/new.yaml", "/tmp/old.yaml", "/tmp/keystore.yaml").success);
    LOGOS_ASSERT(t.cFunctionCalled("migrate_user_config_0_1_2"));
}

LOGOS_TEST(migrate_user_config_0_1_2_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("migrate_user_config_0_1_2").returns(1);

    LOGOS_ASSERT_FALSE(module.migrate_user_config_0_1_2("/tmp/new.yaml", "/tmp/old.yaml", "/tmp/keystore.yaml").success);
}

LOGOS_TEST(participate_returns_0_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("participate").returns(0);

    LOGOS_ASSERT_TRUE(module.participate("/tmp/config.yaml", "/tmp/keystore.yaml", "/tmp/out", "").success);
    LOGOS_ASSERT(t.cFunctionCalled("participate"));
}

LOGOS_TEST(participate_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("participate").returns(1);

    LOGOS_ASSERT_FALSE(module.participate("/tmp/config.yaml", "/tmp/keystore.yaml", "/tmp/out", "1.2.3.4").success);
}

// ============================================================================
// Keystore (generate_key / add_key / remove_key)
// ============================================================================

LOGOS_TEST(generate_key_returns_id_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_key").returns("key-abc123");
    t.mockCFunction("generate_key_error").returns(0);

    StdLogosResult result = module.generate_key("/tmp/config.yaml", "/tmp/keystore.yaml", "ed25519", "");
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("key-abc123"));
    LOGOS_ASSERT(t.cFunctionCalled("generate_key"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
}

LOGOS_TEST(generate_key_accepts_zk_type) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_key").returns("zk-key");
    t.mockCFunction("generate_key_error").returns(0);

    StdLogosResult result = module.generate_key("/tmp/config.yaml", "/tmp/keystore.yaml", "ZK", "my-title");
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("zk-key"));
}

LOGOS_TEST(generate_key_rejects_invalid_key_type) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    StdLogosResult result = module.generate_key("/tmp/config.yaml", "/tmp/keystore.yaml", "rsa", "");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "key_type"));
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("generate_key"));
}

LOGOS_TEST(generate_key_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_key_error").returns(1);

    StdLogosResult result = module.generate_key("/tmp/config.yaml", "/tmp/keystore.yaml", "ed25519", "");
    LOGOS_ASSERT_FALSE(result.success);
}

LOGOS_TEST(add_key_returns_0_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("add_key").returns(0);

    LOGOS_ASSERT_TRUE(module.add_key("/tmp/config.yaml", "/tmp/keystore.yaml", "ed25519", VALID_HEX, "").success);
    LOGOS_ASSERT(t.cFunctionCalled("add_key"));
}

LOGOS_TEST(add_key_rejects_invalid_key_type) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    LOGOS_ASSERT_FALSE(module.add_key("/tmp/config.yaml", "/tmp/keystore.yaml", "bogus", VALID_HEX, "").success);
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("add_key"));
}

LOGOS_TEST(add_key_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("add_key").returns(1);

    LOGOS_ASSERT_FALSE(module.add_key("/tmp/config.yaml", "/tmp/keystore.yaml", "zk", VALID_HEX, "title").success);
}

LOGOS_TEST(remove_key_returns_0_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("remove_key").returns(0);

    LOGOS_ASSERT_TRUE(module.remove_key("/tmp/config.yaml", "/tmp/keystore.yaml", "my-key").success);
    LOGOS_ASSERT(t.cFunctionCalled("remove_key"));
}

LOGOS_TEST(remove_key_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("remove_key").returns(1);

    LOGOS_ASSERT_FALSE(module.remove_key("/tmp/config.yaml", "/tmp/keystore.yaml", "my-key").success);
}

// ============================================================================
// Identity (get_peer_id)
// ============================================================================

LOGOS_TEST(get_peer_id_returns_id_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("get_peer_id").returns("12D3KooWPeerId");
    t.mockCFunction("get_peer_id_error").returns(0);

    StdLogosResult result = module.get_peer_id("/tmp/config.yaml");
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("12D3KooWPeerId"));
    LOGOS_ASSERT(t.cFunctionCalled("get_peer_id"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
}

LOGOS_TEST(get_peer_id_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("get_peer_id_error").returns(1);

    StdLogosResult result = module.get_peer_id("/tmp/config.yaml");
    LOGOS_ASSERT_FALSE(result.success);
}
