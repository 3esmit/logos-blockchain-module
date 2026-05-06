// Unit tests for LogosBlockchainModule.
// All logos_blockchain C functions are mocked at link time via mock_logos_blockchain.cpp.

#include <logos_test.h>
#include "logos_blockchain_module.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

// 64-char hex string = 32 bytes (valid address/hash)
static const QString VALID_HEX = QString(64, 'a');
static const QString VALID_HEX_WITH_PREFIX = "0x" + QString(64, 'b');

// Helper: create a module with a running (mocked) node.
// Sets up circuits directory, mock LogosAPI, and calls start().
static LogosBlockchainModule* createStartedModule(LogosTestContext& t, QTemporaryDir& tmpDir) {
    QDir dir(tmpDir.path());
    dir.mkpath("circuits");
    QFile f(dir.filePath("circuits/dummy.bin"));
    f.open(QIODevice::WriteOnly);
    f.write("x");
    f.close();

    t.api()->setProperty("modulePath", tmpDir.path());

    auto* module = new LogosBlockchainModule();
    t.initLegacy(module);

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
// Core metadata
// ============================================================================

LOGOS_TEST(name_returns_module_name) {
    LogosBlockchainModule module;
    LOGOS_ASSERT_EQ(module.name(), QString("liblogos_blockchain_module"));
}

LOGOS_TEST(version_returns_module_version) {
    LogosBlockchainModule module;
    LOGOS_ASSERT_EQ(module.version(), QString("1.0.0"));
}

// ============================================================================
// generate_user_config
// ============================================================================

LOGOS_TEST(generate_user_config_returns_0_on_success) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_user_config").returns(0);

    QVariantMap args;
    args["output"] = "/tmp/test-config.json";
    LOGOS_ASSERT_EQ(module.generate_user_config(args), 0);
    LOGOS_ASSERT(t.cFunctionCalled("generate_user_config"));
}

LOGOS_TEST(generate_user_config_returns_1_on_failure) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_user_config").returns(1);

    QVariantMap args;
    LOGOS_ASSERT_EQ(module.generate_user_config(args), 1);
}

LOGOS_TEST(generate_user_config_from_str_delegates_to_generate_user_config) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_user_config").returns(0);

    LOGOS_ASSERT_EQ(module.generate_user_config_from_str(R"({"output":"/tmp/out.json"})"), 0);
    LOGOS_ASSERT(t.cFunctionCalled("generate_user_config"));
}

LOGOS_TEST(generate_user_config_with_all_fields) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    t.mockCFunction("generate_user_config").returns(0);

    QVariantMap deployment;
    deployment["well_known_deployment"] = "devnet";

    QVariantMap args;
    args["initial_peers"] = QStringList{"peer1", "peer2"};
    args["output"] = "/tmp/out.json";
    args["net_port"] = 9000;
    args["blend_port"] = 9001;
    args["http_addr"] = "0.0.0.0:8080";
    args["external_address"] = "1.2.3.4";
    args["no_public_ip_check"] = true;
    args["deployment"] = deployment;
    args["state_path"] = "/tmp/state";

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
    QString result = module.wallet_get_balance(VALID_HEX);
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("not running"));
}

LOGOS_TEST(wallet_transfer_funds_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    QString result = module.wallet_transfer_funds(VALID_HEX, QStringList{VALID_HEX}, VALID_HEX, "100", "");
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("not running"));
}

LOGOS_TEST(wallet_transfer_funds_single_sender_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    QString result = module.wallet_transfer_funds(VALID_HEX, VALID_HEX, VALID_HEX, "100", "");
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
}

LOGOS_TEST(wallet_get_known_addresses_without_node_returns_empty) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_TRUE(module.wallet_get_known_addresses().isEmpty());
}

LOGOS_TEST(blend_join_as_core_node_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    QString result = module.blend_join_as_core_node(VALID_HEX, VALID_HEX, VALID_HEX, {"locator1"});
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("not running"));
}

LOGOS_TEST(get_block_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_TRUE(module.get_block(VALID_HEX).startsWith("Error:"));
}

LOGOS_TEST(get_blocks_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_TRUE(module.get_blocks(0, 10).startsWith("Error:"));
}

LOGOS_TEST(get_transaction_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_TRUE(module.get_transaction(VALID_HEX).startsWith("Error:"));
}

LOGOS_TEST(get_cryptarchia_info_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_TRUE(module.get_cryptarchia_info().startsWith("Error:"));
}

// ============================================================================
// Node lifecycle (start / stop)
// ============================================================================

LOGOS_TEST(start_succeeds_with_mocked_dependencies) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    LOGOS_ASSERT_TRUE(tmpDir.isValid());

    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);
    LOGOS_ASSERT(t.cFunctionCalled("start_lb_node"));
    LOGOS_ASSERT(t.cFunctionCalled("subscribe_to_new_blocks"));
    delete module;
}

LOGOS_TEST(start_returns_1_when_already_running) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    LOGOS_ASSERT_EQ(module->start("/tmp/config.json", ""), 1);
    delete module;
}

LOGOS_TEST(start_returns_2_without_logos_api) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_EQ(module.start("/tmp/config.json", ""), 2);
}

LOGOS_TEST(stop_succeeds_with_running_node) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
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
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    QString result = module->wallet_get_balance("abcd");
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("64 hex"));
    delete module;
}

LOGOS_TEST(wallet_get_balance_rejects_long_hex) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    QString result = module->wallet_get_balance(QString(66, 'a'));
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    delete module;
}

LOGOS_TEST(wallet_get_balance_rejects_invalid_chars) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    QString hex = QString(62, 'a') + "zz";
    QString result = module->wallet_get_balance(hex);
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    delete module;
}

// wallet_transfer_funds validation

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_amount) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    QString result = module->wallet_transfer_funds(VALID_HEX, QStringList{VALID_HEX}, VALID_HEX, "not_a_number", "");
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("Invalid amount"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_change_key) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    QString result = module->wallet_transfer_funds("bad", QStringList{VALID_HEX}, VALID_HEX, "100", "");
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("change_public_key"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_recipient) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    QString result = module->wallet_transfer_funds(VALID_HEX, QStringList{VALID_HEX}, "short", "100", "");
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("recipient_address"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_empty_senders) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    QString result = module->wallet_transfer_funds(VALID_HEX, QStringList{}, VALID_HEX, "100", "");
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("sender"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_sender_address) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    QString result = module->wallet_transfer_funds(VALID_HEX, QStringList{"bad_addr"}, VALID_HEX, "100", "");
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("sender"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_rejects_invalid_optional_tip) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    QString result = module->wallet_transfer_funds(VALID_HEX, QStringList{VALID_HEX}, VALID_HEX, "100", "bad_tip");
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("tip"));
    delete module;
}

// blend_join_as_core_node validation

LOGOS_TEST(blend_join_rejects_invalid_provider_id) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    QString result = module->blend_join_as_core_node("short", VALID_HEX, VALID_HEX, {});
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("provider_id"));
    delete module;
}

LOGOS_TEST(blend_join_rejects_invalid_zk_id) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    QString result = module->blend_join_as_core_node(VALID_HEX, "short", VALID_HEX, {});
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("zk_id"));
    delete module;
}

LOGOS_TEST(blend_join_rejects_invalid_locked_note_id) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    QString result = module->blend_join_as_core_node(VALID_HEX, VALID_HEX, "short", {});
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("locked_note_id"));
    delete module;
}

// get_block / get_transaction validation

LOGOS_TEST(get_block_rejects_invalid_hex) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    QString result = module->get_block("tooshort");
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("64 hex"));
    delete module;
}

LOGOS_TEST(get_transaction_rejects_invalid_hex) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    QString result = module->get_transaction("bad");
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("64 hex"));
    delete module;
}

// ============================================================================
// 0x prefix handling
// ============================================================================

LOGOS_TEST(wallet_get_balance_accepts_0x_prefix) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_balance_value").returns(42);
    t.mockCFunction("get_balance_error").returns(0);

    QString result = module->wallet_get_balance(VALID_HEX_WITH_PREFIX);
    LOGOS_ASSERT_EQ(result, QString("42"));
    delete module;
}

// ============================================================================
// Success paths (requires running node + mocked C functions)
// ============================================================================

// Wallet

LOGOS_TEST(wallet_get_balance_returns_balance_string) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_balance_value").returns(1000);
    t.mockCFunction("get_balance_error").returns(0);

    QString result = module->wallet_get_balance(VALID_HEX);
    LOGOS_ASSERT_EQ(result, QString("1000"));
    LOGOS_ASSERT(t.cFunctionCalled("get_balance"));
    delete module;
}

LOGOS_TEST(wallet_get_balance_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_balance_error").returns(1);

    QString result = module->wallet_get_balance(VALID_HEX);
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_returns_tx_hash) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("transfer_funds_error").returns(0);

    QString result = module->wallet_transfer_funds(VALID_HEX, QStringList{VALID_HEX}, VALID_HEX, "500", "");
    LOGOS_ASSERT_FALSE(result.startsWith("Error:"));
    LOGOS_ASSERT_EQ(result.length(), 64);
    LOGOS_ASSERT_TRUE(result.startsWith("ab"));
    LOGOS_ASSERT(t.cFunctionCalled("transfer_funds"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_with_optional_tip) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("transfer_funds_error").returns(0);

    QString result = module->wallet_transfer_funds(VALID_HEX, QStringList{VALID_HEX}, VALID_HEX, "100", VALID_HEX);
    LOGOS_ASSERT_FALSE(result.startsWith("Error:"));
    LOGOS_ASSERT_EQ(result.length(), 64);
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("transfer_funds_error").returns(1);

    QString result = module->wallet_transfer_funds(VALID_HEX, QStringList{VALID_HEX}, VALID_HEX, "100", "");
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_single_sender_overload) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("transfer_funds_error").returns(0);

    QString result = module->wallet_transfer_funds(VALID_HEX, VALID_HEX, VALID_HEX, "100", "");
    LOGOS_ASSERT_FALSE(result.startsWith("Error:"));
    LOGOS_ASSERT(t.cFunctionCalled("transfer_funds"));
    delete module;
}

LOGOS_TEST(wallet_transfer_funds_multiple_senders) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("transfer_funds_error").returns(0);

    QStringList senders;
    senders << VALID_HEX << VALID_HEX_WITH_PREFIX.mid(2); // two different addresses
    QString result = module->wallet_transfer_funds(VALID_HEX, senders, VALID_HEX, "200", "");
    LOGOS_ASSERT_FALSE(result.startsWith("Error:"));
    delete module;
}

LOGOS_TEST(wallet_get_known_addresses_returns_addresses) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_known_addresses_error").returns(0);
    t.mockCFunction("get_known_addresses_count").returns(2);

    QStringList addrs = module->wallet_get_known_addresses();
    LOGOS_ASSERT_EQ(addrs.size(), 2);
    // Mock fills addr0 with 0x11 → hex "1111...11", addr1 with 0x22 → "2222...22"
    LOGOS_ASSERT_EQ(addrs[0], QString(64, '1'));
    LOGOS_ASSERT_EQ(addrs[1], QString(64, '2'));
    LOGOS_ASSERT(t.cFunctionCalled("get_known_addresses"));
    LOGOS_ASSERT(t.cFunctionCalled("free_known_addresses"));
    delete module;
}

LOGOS_TEST(wallet_get_known_addresses_returns_empty_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_known_addresses_error").returns(1);

    QStringList addrs = module->wallet_get_known_addresses();
    LOGOS_ASSERT_TRUE(addrs.isEmpty());
    delete module;
}

// Blend

LOGOS_TEST(blend_join_as_core_node_returns_declaration_id) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("blend_join_as_core_node_error").returns(0);

    QStringList locators = {"locator1", "locator2"};
    QString result = module->blend_join_as_core_node(VALID_HEX, VALID_HEX, VALID_HEX, locators);
    // Mock fills hash with 0xCD → hex "cdcd...cd" (64 chars)
    LOGOS_ASSERT_EQ(result.length(), 64);
    LOGOS_ASSERT_TRUE(result.startsWith("cd"));
    LOGOS_ASSERT(t.cFunctionCalled("blend_join_as_core_node"));
    delete module;
}

LOGOS_TEST(blend_join_as_core_node_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("blend_join_as_core_node_error").returns(1);

    QString result = module->blend_join_as_core_node(VALID_HEX, VALID_HEX, VALID_HEX, {});
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    delete module;
}

// Explorer

LOGOS_TEST(get_block_returns_json_on_success) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_block").returns(R"({"slot":42,"data":"test"})");
    t.mockCFunction("get_block_error").returns(0);

    QString result = module->get_block(VALID_HEX);
    LOGOS_ASSERT_TRUE(result.contains("slot"));
    LOGOS_ASSERT_TRUE(result.contains("42"));
    LOGOS_ASSERT(t.cFunctionCalled("get_block"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
    delete module;
}

LOGOS_TEST(get_block_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_block_error").returns(1);

    QString result = module->get_block(VALID_HEX);
    LOGOS_ASSERT_TRUE(result.startsWith("Error:"));
    delete module;
}

LOGOS_TEST(get_blocks_returns_json_on_success) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_blocks").returns(R"([{"slot":1},{"slot":2}])");
    t.mockCFunction("get_blocks_error").returns(0);

    QString result = module->get_blocks(1, 10);
    LOGOS_ASSERT_TRUE(result.contains("slot"));
    LOGOS_ASSERT(t.cFunctionCalled("get_blocks"));
    delete module;
}

LOGOS_TEST(get_blocks_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_blocks_error").returns(1);

    LOGOS_ASSERT_TRUE(module->get_blocks(0, 10).startsWith("Error:"));
    delete module;
}

LOGOS_TEST(get_transaction_returns_json_on_success) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_transaction").returns(R"({"hash":"abc","status":"confirmed"})");
    t.mockCFunction("get_transaction_error").returns(0);

    QString result = module->get_transaction(VALID_HEX);
    LOGOS_ASSERT_TRUE(result.contains("confirmed"));
    LOGOS_ASSERT(t.cFunctionCalled("get_transaction"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
    delete module;
}

LOGOS_TEST(get_transaction_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_transaction_error").returns(1);

    LOGOS_ASSERT_TRUE(module->get_transaction(VALID_HEX).startsWith("Error:"));
    delete module;
}

// Cryptarchia

LOGOS_TEST(get_cryptarchia_info_returns_json_on_success) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_cryptarchia_info_error").returns(0);
    t.mockCFunction("cryptarchia_slot").returns(100);
    t.mockCFunction("cryptarchia_height").returns(50);
    t.mockCFunction("cryptarchia_mode").returns(1); // Online

    QString result = module->get_cryptarchia_info();
    LOGOS_ASSERT_FALSE(result.startsWith("Error:"));
    LOGOS_ASSERT_TRUE(result.contains("slot"));
    LOGOS_ASSERT_TRUE(result.contains("100"));
    LOGOS_ASSERT_TRUE(result.contains("height"));
    LOGOS_ASSERT_TRUE(result.contains("50"));
    LOGOS_ASSERT_TRUE(result.contains("Online"));
    LOGOS_ASSERT_TRUE(result.contains("lib"));
    LOGOS_ASSERT_TRUE(result.contains("tip"));
    LOGOS_ASSERT(t.cFunctionCalled("get_cryptarchia_info"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cryptarchia_info"));
    delete module;
}

LOGOS_TEST(get_cryptarchia_info_bootstrapping_mode) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_cryptarchia_info_error").returns(0);
    t.mockCFunction("cryptarchia_mode").returns(0); // Bootstrapping

    QString result = module->get_cryptarchia_info();
    LOGOS_ASSERT_TRUE(result.contains("Bootstrapping"));
    delete module;
}

LOGOS_TEST(get_cryptarchia_info_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("blockchain_module");
    QTemporaryDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_cryptarchia_info_error").returns(1);

    LOGOS_ASSERT_TRUE(module->get_cryptarchia_info().startsWith("Error:"));
    delete module;
}
