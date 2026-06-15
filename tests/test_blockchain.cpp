// Unit tests for LogosBlockchainModule.
// All logos_blockchain C functions are mocked at link time via mock_logos_blockchain.cpp.

#include <logos_test.h>
#include "logos_blockchain_module.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// 64-char hex string = 32 bytes (valid address/hash)
static const std::string VALID_HEX(64, 'a');
static const std::string VALID_HEX_WITH_PREFIX = "0x" + std::string(64, 'b');

static bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

static bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
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
// Sets up circuits directory, LOGOS_MODULE_PATH env, and calls start().
static LogosBlockchainModule* createStartedModule(LogosTestContext& t, TempDir& tmpDir) {
    fs::create_directories(tmpDir.path / "circuits");
    {
        std::ofstream f((tmpDir.path / "circuits" / "dummy.bin").string());
        f << "x";
    }

    setenv("LOGOS_MODULE_PATH", tmpDir.path.string().c_str(), 1);

    auto* module = new LogosBlockchainModule();

    t.mockCFunction("start_lb_node").returns(1);
    t.mockCFunction("subscribe_to_new_blocks").returns(0);

    int rc = module->start(tmpDir.filePath("config.json"), "");
    if (rc != 0) {
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

    LOGOS_ASSERT_EQ(module.generate_user_config(R"({"output":"/tmp/test-config.json"})"), 0);
    LOGOS_ASSERT(t.cFunctionCalled("generate_user_config"));
}

LOGOS_TEST(generate_user_config_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_user_config").returns(1);

    LOGOS_ASSERT_EQ(module.generate_user_config("{}"), 1);
}

LOGOS_TEST(generate_user_config_from_json_string) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_user_config").returns(0);

    LOGOS_ASSERT_EQ(module.generate_user_config(R"({"output":"/tmp/out.json"})"), 0);
    LOGOS_ASSERT(t.cFunctionCalled("generate_user_config"));
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
        "no_public_ip_check": true,
        "deployment": { "well_known_deployment": "devnet" },
        "state_path": "/tmp/state"
    })";

    LOGOS_ASSERT_EQ(module.generate_user_config(args), 0);
}

// ============================================================================
// No-node error paths — all methods should fail gracefully
// ============================================================================

LOGOS_TEST(stop_without_node_returns_1) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_EQ(module.stop(), 1);
}

LOGOS_TEST(wallet_get_balance_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    std::string result = module.wallet_get_balance(VALID_HEX);
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "not running"));
}

LOGOS_TEST(wallet_transfer_funds_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    std::string result = module.wallet_transfer_funds(VALID_HEX, {VALID_HEX}, VALID_HEX, "100", "");
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "not running"));
}

LOGOS_TEST(wallet_get_known_addresses_without_node_returns_empty) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_TRUE(module.wallet_get_known_addresses().empty());
}

LOGOS_TEST(blend_join_as_core_node_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    std::string result = module.blend_join_as_core_node(VALID_HEX, VALID_HEX, VALID_HEX, {"locator1"});
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "not running"));
}

LOGOS_TEST(get_block_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_TRUE(starts_with(module.get_block(VALID_HEX), "Error:"));
}

LOGOS_TEST(get_blocks_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_TRUE(starts_with(module.get_blocks(0, 10), "Error:"));
}

LOGOS_TEST(get_transaction_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_TRUE(starts_with(module.get_transaction(VALID_HEX), "Error:"));
}

LOGOS_TEST(get_cryptarchia_info_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_TRUE(starts_with(module.get_cryptarchia_info(), "Error:"));
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

    LOGOS_ASSERT_EQ(module->start("/tmp/config.json", ""), 1);
    delete module;
}

LOGOS_TEST(stop_succeeds_with_running_node) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    LOGOS_ASSERT_EQ(module->stop(), 0);
    LOGOS_ASSERT(t.cFunctionCalled("stop_node"));
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

    std::string result = module->wallet_get_balance("abcd");
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "64 hex"));
    delete module;
}

LOGOS_TEST(wallet_get_balance_rejects_long_hex) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    std::string result = module->wallet_get_balance(std::string(66, 'a'));
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    delete module;
}

LOGOS_TEST(wallet_get_balance_rejects_invalid_chars) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    std::string hex = std::string(62, 'a') + "zz";
    std::string result = module->wallet_get_balance(hex);
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    delete module;
}

// wallet_transfer_funds validation

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_amount) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    std::string result = module->wallet_transfer_funds(VALID_HEX, {VALID_HEX}, VALID_HEX, "not_a_number", "");
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "Invalid amount"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_change_key) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    std::string result = module->wallet_transfer_funds("bad", {VALID_HEX}, VALID_HEX, "100", "");
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "change_public_key"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_recipient) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    std::string result = module->wallet_transfer_funds(VALID_HEX, {VALID_HEX}, "short", "100", "");
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "recipient_address"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_empty_senders) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    std::string result = module->wallet_transfer_funds(VALID_HEX, {}, VALID_HEX, "100", "");
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "sender"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_sender_address) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    std::string result = module->wallet_transfer_funds(VALID_HEX, {"bad_addr"}, VALID_HEX, "100", "");
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "sender"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_optional_tip) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    std::string result = module->wallet_transfer_funds(VALID_HEX, {VALID_HEX}, VALID_HEX, "100", "bad_tip");
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "tip"));
    delete module;
}

// blend_join_as_core_node validation

LOGOS_TEST(blend_join_rejects_invalid_provider_id) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    std::string result = module->blend_join_as_core_node("short", VALID_HEX, VALID_HEX, {});
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "provider_id"));
    delete module;
}

LOGOS_TEST(blend_join_rejects_invalid_zk_id) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    std::string result = module->blend_join_as_core_node(VALID_HEX, "short", VALID_HEX, {});
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "zk_id"));
    delete module;
}

LOGOS_TEST(blend_join_rejects_invalid_locked_note_id) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    std::string result = module->blend_join_as_core_node(VALID_HEX, VALID_HEX, "short", {});
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "locked_note_id"));
    delete module;
}

// get_block / get_transaction validation

LOGOS_TEST(get_block_rejects_invalid_hex) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    std::string result = module->get_block("tooshort");
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "64 hex"));
    delete module;
}

LOGOS_TEST(get_transaction_rejects_invalid_hex) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    std::string result = module->get_transaction("bad");
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "64 hex"));
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

    std::string result = module->wallet_get_balance(VALID_HEX_WITH_PREFIX);
    LOGOS_ASSERT_EQ(result, std::string("42"));
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

    std::string result = module->wallet_get_balance(VALID_HEX);
    LOGOS_ASSERT_EQ(result, std::string("1000"));
    LOGOS_ASSERT(t.cFunctionCalled("get_balance"));
    delete module;
}

LOGOS_TEST(wallet_get_balance_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_balance_error").returns(1);

    std::string result = module->wallet_get_balance(VALID_HEX);
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_returns_tx_hash) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("transfer_funds_error").returns(0);

    std::string result = module->wallet_transfer_funds(VALID_HEX, {VALID_HEX}, VALID_HEX, "500", "");
    LOGOS_ASSERT_FALSE(starts_with(result, "Error:"));
    LOGOS_ASSERT_EQ(static_cast<int>(result.length()), 64);
    LOGOS_ASSERT_TRUE(starts_with(result, "ab"));
    LOGOS_ASSERT(t.cFunctionCalled("transfer_funds"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_with_optional_tip) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("transfer_funds_error").returns(0);

    std::string result = module->wallet_transfer_funds(VALID_HEX, {VALID_HEX}, VALID_HEX, "100", VALID_HEX);
    LOGOS_ASSERT_FALSE(starts_with(result, "Error:"));
    LOGOS_ASSERT_EQ(static_cast<int>(result.length()), 64);
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("transfer_funds_error").returns(1);

    std::string result = module->wallet_transfer_funds(VALID_HEX, {VALID_HEX}, VALID_HEX, "100", "");
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_single_sender_via_vector) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("transfer_funds_error").returns(0);

    std::string result = module->wallet_transfer_funds(VALID_HEX, {VALID_HEX}, VALID_HEX, "100", "");
    LOGOS_ASSERT_FALSE(starts_with(result, "Error:"));
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
    std::string result = module->wallet_transfer_funds(VALID_HEX, senders, VALID_HEX, "200", "");
    LOGOS_ASSERT_FALSE(starts_with(result, "Error:"));
    delete module;
}

LOGOS_TEST(wallet_get_known_addresses_returns_addresses) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_known_addresses_error").returns(0);
    t.mockCFunction("get_known_addresses_count").returns(2);

    std::vector<std::string> addrs = module->wallet_get_known_addresses();
    LOGOS_ASSERT_EQ(static_cast<int>(addrs.size()), 2);
    // Mock fills addr0 with 0x11 -> hex "1111...11", addr1 with 0x22 -> "2222...22"
    LOGOS_ASSERT_EQ(addrs[0], std::string(64, '1'));
    LOGOS_ASSERT_EQ(addrs[1], std::string(64, '2'));
    LOGOS_ASSERT(t.cFunctionCalled("get_known_addresses"));
    LOGOS_ASSERT(t.cFunctionCalled("free_known_addresses"));
    delete module;
}

LOGOS_TEST(wallet_get_known_addresses_returns_empty_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_known_addresses_error").returns(1);

    std::vector<std::string> addrs = module->wallet_get_known_addresses();
    LOGOS_ASSERT_TRUE(addrs.empty());
    delete module;
}

// Blend

LOGOS_TEST(blend_join_as_core_node_returns_declaration_id) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("blend_join_as_core_node_error").returns(0);

    std::vector<std::string> locators = {"locator1", "locator2"};
    std::string result = module->blend_join_as_core_node(VALID_HEX, VALID_HEX, VALID_HEX, locators);
    // Mock fills hash with 0xCD -> hex "cdcd...cd" (64 chars)
    LOGOS_ASSERT_EQ(static_cast<int>(result.length()), 64);
    LOGOS_ASSERT_TRUE(starts_with(result, "cd"));
    LOGOS_ASSERT(t.cFunctionCalled("blend_join_as_core_node"));
    delete module;
}

LOGOS_TEST(blend_join_as_core_node_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("blend_join_as_core_node_error").returns(1);

    std::string result = module->blend_join_as_core_node(VALID_HEX, VALID_HEX, VALID_HEX, {});
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
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

    std::string result = module->get_block(VALID_HEX);
    LOGOS_ASSERT_TRUE(contains(result, "slot"));
    LOGOS_ASSERT_TRUE(contains(result, "42"));
    LOGOS_ASSERT(t.cFunctionCalled("get_block"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
    delete module;
}

LOGOS_TEST(get_block_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_block_error").returns(1);

    std::string result = module->get_block(VALID_HEX);
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    delete module;
}

LOGOS_TEST(get_blocks_returns_json_on_success) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_blocks").returns(R"([{"slot":1},{"slot":2}])");
    t.mockCFunction("get_blocks_error").returns(0);

    std::string result = module->get_blocks(1, 10);
    LOGOS_ASSERT_TRUE(contains(result, "slot"));
    LOGOS_ASSERT(t.cFunctionCalled("get_blocks"));
    delete module;
}

LOGOS_TEST(get_blocks_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_blocks_error").returns(1);

    LOGOS_ASSERT_TRUE(starts_with(module->get_blocks(0, 10), "Error:"));
    delete module;
}

LOGOS_TEST(get_transaction_returns_json_on_success) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_transaction").returns(R"({"hash":"abc","status":"confirmed"})");
    t.mockCFunction("get_transaction_error").returns(0);

    std::string result = module->get_transaction(VALID_HEX);
    LOGOS_ASSERT_TRUE(contains(result, "confirmed"));
    LOGOS_ASSERT(t.cFunctionCalled("get_transaction"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
    delete module;
}

LOGOS_TEST(get_transaction_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_transaction_error").returns(1);

    LOGOS_ASSERT_TRUE(starts_with(module->get_transaction(VALID_HEX), "Error:"));
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

    std::string result = module->get_cryptarchia_info();
    LOGOS_ASSERT_FALSE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "slot"));
    LOGOS_ASSERT_TRUE(contains(result, "100"));
    LOGOS_ASSERT_TRUE(contains(result, "height"));
    LOGOS_ASSERT_TRUE(contains(result, "50"));
    LOGOS_ASSERT_TRUE(contains(result, "Online"));
    LOGOS_ASSERT_TRUE(contains(result, "lib"));
    LOGOS_ASSERT_TRUE(contains(result, "tip"));
    LOGOS_ASSERT(t.cFunctionCalled("get_cryptarchia_info"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cryptarchia_info"));
    delete module;
}

LOGOS_TEST(get_cryptarchia_info_bootstrapping_mode) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_cryptarchia_info_error").returns(0);
    t.mockCFunction("cryptarchia_mode").returns(0); // Bootstrapping

    std::string result = module->get_cryptarchia_info();
    LOGOS_ASSERT_TRUE(contains(result, "Bootstrapping"));
    delete module;
}

LOGOS_TEST(get_cryptarchia_info_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_cryptarchia_info_error").returns(1);

    LOGOS_ASSERT_TRUE(starts_with(module->get_cryptarchia_info(), "Error:"));
    delete module;
}

// ============================================================================
// Config management (operate on file paths, no running node required)
// ============================================================================

LOGOS_TEST(update_user_config_returns_0_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("update_user_config").returns(0);

    LOGOS_ASSERT_EQ(module.update_user_config("/tmp/config.yaml", "/tmp/keystore.yaml"), 0);
    LOGOS_ASSERT(t.cFunctionCalled("update_user_config"));
}

LOGOS_TEST(update_user_config_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("update_user_config").returns(1);

    LOGOS_ASSERT_EQ(module.update_user_config("/tmp/config.yaml", "/tmp/keystore.yaml"), 1);
}

LOGOS_TEST(migrate_user_config_returns_0_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("migrate_user_config").returns(0);

    LOGOS_ASSERT_EQ(module.migrate_user_config("/tmp/out.yaml", "/tmp/keystore.yaml"), 0);
    LOGOS_ASSERT(t.cFunctionCalled("migrate_user_config"));
}

LOGOS_TEST(migrate_user_config_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("migrate_user_config").returns(1);

    LOGOS_ASSERT_EQ(module.migrate_user_config("/tmp/out.yaml", "/tmp/keystore.yaml"), 1);
}

LOGOS_TEST(migrate_user_config_0_1_2_returns_0_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("migrate_user_config_0_1_2").returns(0);

    LOGOS_ASSERT_EQ(module.migrate_user_config_0_1_2("/tmp/new.yaml", "/tmp/old.yaml", "/tmp/keystore.yaml"), 0);
    LOGOS_ASSERT(t.cFunctionCalled("migrate_user_config_0_1_2"));
}

LOGOS_TEST(migrate_user_config_0_1_2_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("migrate_user_config_0_1_2").returns(1);

    LOGOS_ASSERT_EQ(module.migrate_user_config_0_1_2("/tmp/new.yaml", "/tmp/old.yaml", "/tmp/keystore.yaml"), 1);
}

LOGOS_TEST(participate_returns_0_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("participate").returns(0);

    LOGOS_ASSERT_EQ(module.participate("/tmp/config.yaml", "/tmp/keystore.yaml", "/tmp/out", ""), 0);
    LOGOS_ASSERT(t.cFunctionCalled("participate"));
}

LOGOS_TEST(participate_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("participate").returns(1);

    LOGOS_ASSERT_EQ(module.participate("/tmp/config.yaml", "/tmp/keystore.yaml", "/tmp/out", "1.2.3.4"), 1);
}

// ============================================================================
// Keystore (generate_key / add_key / remove_key)
// ============================================================================

LOGOS_TEST(generate_key_returns_id_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_key").returns("key-abc123");
    t.mockCFunction("generate_key_error").returns(0);

    std::string result = module.generate_key("/tmp/config.yaml", "/tmp/keystore.yaml", "ed25519", "");
    LOGOS_ASSERT_EQ(result, std::string("key-abc123"));
    LOGOS_ASSERT(t.cFunctionCalled("generate_key"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
}

LOGOS_TEST(generate_key_accepts_zk_type) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_key").returns("zk-key");
    t.mockCFunction("generate_key_error").returns(0);

    std::string result = module.generate_key("/tmp/config.yaml", "/tmp/keystore.yaml", "ZK", "my-title");
    LOGOS_ASSERT_EQ(result, std::string("zk-key"));
}

LOGOS_TEST(generate_key_rejects_invalid_key_type) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    std::string result = module.generate_key("/tmp/config.yaml", "/tmp/keystore.yaml", "rsa", "");
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
    LOGOS_ASSERT_TRUE(contains(result, "key_type"));
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("generate_key"));
}

LOGOS_TEST(generate_key_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_key_error").returns(1);

    std::string result = module.generate_key("/tmp/config.yaml", "/tmp/keystore.yaml", "ed25519", "");
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
}

LOGOS_TEST(add_key_returns_0_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("add_key").returns(0);

    LOGOS_ASSERT_EQ(module.add_key("/tmp/config.yaml", "/tmp/keystore.yaml", "ed25519", VALID_HEX, ""), 0);
    LOGOS_ASSERT(t.cFunctionCalled("add_key"));
}

LOGOS_TEST(add_key_rejects_invalid_key_type) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    LOGOS_ASSERT_EQ(module.add_key("/tmp/config.yaml", "/tmp/keystore.yaml", "bogus", VALID_HEX, ""), 1);
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("add_key"));
}

LOGOS_TEST(add_key_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("add_key").returns(1);

    LOGOS_ASSERT_EQ(module.add_key("/tmp/config.yaml", "/tmp/keystore.yaml", "zk", VALID_HEX, "title"), 1);
}

LOGOS_TEST(remove_key_returns_0_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("remove_key").returns(0);

    LOGOS_ASSERT_EQ(module.remove_key("/tmp/config.yaml", "/tmp/keystore.yaml", "my-key"), 0);
    LOGOS_ASSERT(t.cFunctionCalled("remove_key"));
}

LOGOS_TEST(remove_key_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("remove_key").returns(1);

    LOGOS_ASSERT_EQ(module.remove_key("/tmp/config.yaml", "/tmp/keystore.yaml", "my-key"), 1);
}

// ============================================================================
// Identity (get_peer_id)
// ============================================================================

LOGOS_TEST(get_peer_id_returns_id_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("get_peer_id").returns("12D3KooWPeerId");
    t.mockCFunction("get_peer_id_error").returns(0);

    std::string result = module.get_peer_id("/tmp/config.yaml");
    LOGOS_ASSERT_EQ(result, std::string("12D3KooWPeerId"));
    LOGOS_ASSERT(t.cFunctionCalled("get_peer_id"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
}

LOGOS_TEST(get_peer_id_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("get_peer_id_error").returns(1);

    std::string result = module.get_peer_id("/tmp/config.yaml");
    LOGOS_ASSERT_TRUE(starts_with(result, "Error:"));
}
