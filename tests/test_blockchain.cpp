// Unit tests for LogosBlockchainModule.
// All logos_blockchain C functions are mocked at link time via mock_logos_blockchain.cpp.

#include <logos_test.h>
#include "logos_blockchain_module.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
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

void reset_node_changed_events();
std::vector<std::string> node_changed_events();
using NewBlockHook = void (*)();
void set_new_block_hook(NewBlockHook hook);
void trigger_mock_new_block(const char* block_json);
void reset_mock_start_control();
void set_mock_start_blocked(bool blocked);
bool mock_start_entered();

static json read_node_status(LogosBlockchainModule& module) {
    return json::parse(module.nodeStatus());
}

static json wait_for_lifecycle_terminal(LogosBlockchainModule& module) {
    json status;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        status = read_node_status(module);
        if (status.at("pending_operation").is_null())
            return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return status;
}

static json invoke_node_action(LogosBlockchainModule& module, const json& request) {
    const json acknowledgement = json::parse(module.nodeAction(request.dump()));
    if (acknowledgement.value("accepted", false) && !acknowledgement.value("duplicate", false))
        (void)wait_for_lifecycle_terminal(module);
    return acknowledgement;
}

static json lifecycle_command(
    const std::string& operation_id,
    const std::string& action,
    const json& parameters = json::object()
) {
    json command = {
        {"schema", "logos.managed_node_lifecycle.command"},
        {"version", 1},
        {"operation_id", operation_id},
        {"action", action},
    };
    if (!parameters.empty())
        command["parameters"] = parameters;
    return command;
}

static std::vector<json> lifecycle_events() {
    std::vector<json> result;
    for (const std::string& event : node_changed_events()) {
        result.push_back(json::parse(event));
    }
    return result;
}

static std::string test_hex_id(const size_t value) {
    char id[65] = {};
    std::snprintf(id, sizeof(id), "%064zx", value);
    return id;
}

static LogosBlockchainModule* g_callbackStopModule = nullptr;

static void stop_from_new_block_callback() {
    if (g_callbackStopModule) {
        (void)g_callbackStopModule->stop();
    }
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
    t.mockCFunction("cryptarchia_info_abi_version").returns(CRYPTARCHIA_INFO_ABI_VERSION);

    StdLogosResult rc = module->start(tmpDir.filePath("config.json"), "");
    if (!rc.success) {
        delete module;
        return nullptr;
    }
    return module;
}

static LogosBlockchainModule* createStartedModuleWithConfig(
    LogosTestContext& t,
    TempDir& tmpDir,
    const std::string& config_contents
) {
    std::ofstream config(tmpDir.filePath("config.json"));
    config << config_contents;
    config.close();
    return createStartedModule(t, tmpDir);
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

LOGOS_TEST(node_status_reports_a_versioned_snapshot_before_initialization) {
    LogosBlockchainModule module;

    const json status = read_node_status(module);
    LOGOS_ASSERT_EQ(status.at("schema").get<std::string>(), std::string("logos.managed_node_lifecycle.snapshot"));
    LOGOS_ASSERT_EQ(status.at("version").get<int>(), 1);
    LOGOS_ASSERT_FALSE(status.at("instance_id").get<std::string>().empty());
    LOGOS_ASSERT_EQ(status.at("epoch").get<std::uint64_t>(), 0U);
    LOGOS_ASSERT_EQ(status.at("sequence").get<std::uint64_t>(), 0U);
    LOGOS_ASSERT_EQ(status.at("scope").at("kind").get<std::string>(), std::string("bedrock"));
    LOGOS_ASSERT_EQ(status.at("state").get<std::string>(), std::string("uninitialized"));
    LOGOS_ASSERT_EQ(status.at("health").get<std::string>(), std::string("unknown"));
    LOGOS_ASSERT_EQ(status.at("supported_actions").size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(status.at("supported_actions").at(0).get<std::string>(), std::string("initialize"));
}

LOGOS_TEST(node_action_initializes_with_ordered_redacted_events) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmp_dir;
    LogosBlockchainModule module;
    t.mockCFunction("generate_user_config").returns(0);
    reset_node_changed_events();

    const std::string secret = "must-not-appear-in-lifecycle-output";
    const json request = lifecycle_command(
        "bedrock-initialize-v1",
        "initialize",
        {{"config",
          json({
                   {"output", tmp_dir.filePath("node.json")},
                   {"external_address", secret},
               })
              .dump()}}
    );
    const json acknowledgement = invoke_node_action(module, request);

    LOGOS_ASSERT_EQ(acknowledgement.at("schema").get<std::string>(), std::string("logos.managed_node_lifecycle.ack"));
    LOGOS_ASSERT_TRUE(acknowledgement.at("accepted").get<bool>());
    LOGOS_ASSERT_FALSE(acknowledgement.at("duplicate").get<bool>());
    LOGOS_ASSERT_TRUE(acknowledgement.at("error").is_null());
    LOGOS_ASSERT(t.cFunctionCalled("generate_user_config"));

    const std::vector<json> events = lifecycle_events();
    LOGOS_ASSERT_EQ(events.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(events.at(0).at("phase").get<std::string>(), std::string("accepted"));
    LOGOS_ASSERT_EQ(events.at(0).at("status").at("state").get<std::string>(), std::string("initializing"));
    LOGOS_ASSERT_EQ(events.at(1).at("phase").get<std::string>(), std::string("settled"));
    LOGOS_ASSERT_EQ(events.at(1).at("outcome").get<std::string>(), std::string("succeeded"));
    LOGOS_ASSERT_EQ(events.at(1).at("status").at("state").get<std::string>(), std::string("stopped"));
    LOGOS_ASSERT_TRUE(events.at(1).at("emitted_at_ms").get<std::int64_t>() > 0);
    LOGOS_ASSERT_TRUE(
        events.at(1).at("sequence").get<std::uint64_t>() > events.at(0).at("sequence").get<std::uint64_t>()
    );
    LOGOS_ASSERT_TRUE(events.at(1).dump().find(secret) == std::string::npos);

    const json status = read_node_status(module);
    LOGOS_ASSERT_EQ(status.at("state").get<std::string>(), std::string("stopped"));
    LOGOS_ASSERT_EQ(status.at("epoch").get<std::uint64_t>(), 1U);
    LOGOS_ASSERT_TRUE(status.dump().find(secret) == std::string::npos);
}

LOGOS_TEST(node_action_recovers_basecamp_config_after_host_recreation) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmp_dir;
    LOGOS_ASSERT_TRUE(tmp_dir.isValid());

    const fs::path persistence = tmp_dir.path / "persistence";
    const fs::path config_path = tmp_dir.path / "inspector-workspace" / "config" / "bedrock.yaml";
    fs::create_directories(config_path.parent_path());
    {
        std::ofstream config(config_path, std::ios::binary);
        config << "bedrock-sentinel";
    }

    {
        LogosBlockchainModule original;
        original._logosCoreSetContext_("/module", "bedrock", persistence.string());
        const json initialized = invoke_node_action(
            original,
            lifecycle_command(
                "bedrock-recover-existing-v1",
                "initialize",
                {{"config", json({{"output", config_path.string()}}).dump()}}
            )
        );
        LOGOS_ASSERT_TRUE(initialized.at("accepted").get<bool>());
        LOGOS_ASSERT_EQ(t.cFunctionCallCount("generate_user_config"), 0);
        LOGOS_ASSERT_EQ(read_node_status(original).at("state").get<std::string>(), std::string("stopped"));
    }

    const fs::path lifecycle_state = persistence / ".logos-blockchain-lifecycle-v1.json";
    LOGOS_ASSERT_TRUE(fs::is_regular_file(lifecycle_state));
    std::ifstream state_input(lifecycle_state, std::ios::binary);
    const std::string state((std::istreambuf_iterator<char>(state_input)), std::istreambuf_iterator<char>());
    LOGOS_ASSERT_TRUE(state.find(config_path.string()) != std::string::npos);
    std::ifstream config_input(config_path, std::ios::binary);
    const std::string config((std::istreambuf_iterator<char>(config_input)), std::istreambuf_iterator<char>());
    LOGOS_ASSERT_EQ(config, std::string("bedrock-sentinel"));

    LogosBlockchainModule recreated;
    recreated._logosCoreSetContext_("/module", "bedrock", persistence.string());
    const json recovered = read_node_status(recreated);
    LOGOS_ASSERT_EQ(recovered.at("state").get<std::string>(), std::string("stopped"));
    LOGOS_ASSERT_EQ(recovered.at("epoch").get<std::uint64_t>(), 1U);
    LOGOS_ASSERT_EQ(recovered.at("supported_actions").at(0).get<std::string>(), std::string("start"));

    t.mockCFunction("start_lb_node").returns(1);
    t.mockCFunction("subscribe_to_new_blocks").returns(0);
    reset_node_changed_events();
    const json started = invoke_node_action(recreated, lifecycle_command("bedrock-start-after-recovery-v1", "start"));
    LOGOS_ASSERT_TRUE(started.at("accepted").get<bool>());
    const std::vector<json> events = lifecycle_events();
    LOGOS_ASSERT_EQ(events.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(events.at(0).at("status").at("state").get<std::string>(), std::string("starting"));
    LOGOS_ASSERT_EQ(events.at(1).at("status").at("state").get<std::string>(), std::string("running"));
    LOGOS_ASSERT_EQ(events.at(1).at("epoch").get<std::uint64_t>(), recovered.at("epoch").get<std::uint64_t>());
}

LOGOS_TEST(node_status_ignores_missing_persisted_config) {
    TempDir tmp_dir;
    LOGOS_ASSERT_TRUE(tmp_dir.isValid());
    const fs::path persistence = tmp_dir.path / "persistence";
    fs::create_directories(persistence);
    {
        std::ofstream state(persistence / ".logos-blockchain-lifecycle-v1.json", std::ios::binary);
        state << json({
                          {"schema", "logos.blockchain.lifecycle_config"},
                          {"version", 1},
                          {"config_path", (tmp_dir.path / "missing.yaml").string()},
                      })
                     .dump();
    }

    LogosBlockchainModule module;
    module._logosCoreSetContext_("/module", "bedrock", persistence.string());
    const json status = read_node_status(module);
    LOGOS_ASSERT_EQ(status.at("state").get<std::string>(), std::string("uninitialized"));
    LOGOS_ASSERT_EQ(status.at("supported_actions").at(0).get<std::string>(), std::string("initialize"));
}

LOGOS_TEST(node_action_starts_and_stops_an_initialized_node) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmp_dir;
    LogosBlockchainModule module;
    t.mockCFunction("generate_user_config").returns(0);
    t.mockCFunction("start_lb_node").returns(1);
    t.mockCFunction("subscribe_to_new_blocks").returns(0);

    LOGOS_ASSERT_TRUE(invoke_node_action(
                          module,
                          lifecycle_command(
                              "bedrock-initialize-start-stop",
                              "initialize",
                              {{"config", json({{"output", tmp_dir.filePath("node.json")}}).dump()}}
                          )
    )
                          .at("accepted")
                          .get<bool>());

    reset_node_changed_events();
    const json started =
        invoke_node_action(module, lifecycle_command("bedrock-start-v1", "start", {{"deployment", ""}}));
    LOGOS_ASSERT_TRUE(started.at("accepted").get<bool>());
    LOGOS_ASSERT(t.cFunctionCalled("start_lb_node"));
    LOGOS_ASSERT(t.cFunctionCalled("subscribe_to_new_blocks"));
    std::vector<json> events = lifecycle_events();
    LOGOS_ASSERT_EQ(events.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(events.at(0).at("status").at("state").get<std::string>(), std::string("starting"));
    LOGOS_ASSERT_EQ(events.at(1).at("status").at("state").get<std::string>(), std::string("running"));
    LOGOS_ASSERT_EQ(read_node_status(module).at("state").get<std::string>(), std::string("running"));

    reset_node_changed_events();
    const json stopped = invoke_node_action(module, lifecycle_command("bedrock-stop-v1", "stop"));
    LOGOS_ASSERT_TRUE(stopped.at("accepted").get<bool>());
    LOGOS_ASSERT(t.cFunctionCalled("shutdown_node"));
    events = lifecycle_events();
    LOGOS_ASSERT_EQ(events.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(events.at(1).at("outcome").get<std::string>(), std::string("succeeded"));
    LOGOS_ASSERT_EQ(events.at(1).at("status").at("state").get<std::string>(), std::string("stopped"));
}

LOGOS_TEST(block_callback_stop_defers_shutdown_until_callback_returns) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmp_dir;
    LogosBlockchainModule module;
    t.mockCFunction("start_lb_node").returns(1);
    t.mockCFunction("subscribe_to_new_blocks").returns(0);
    t.mockCFunction("shutdown_node").returns(0);

    LOGOS_ASSERT_TRUE(module.start(tmp_dir.filePath("config.json"), "").success);
    g_callbackStopModule = &module;
    set_new_block_hook(stop_from_new_block_callback);
    trigger_mock_new_block(R"({"slot":1})");
    set_new_block_hook(nullptr);
    g_callbackStopModule = nullptr;

    for (int attempt = 0; attempt < 500 && !t.cFunctionCalled("shutdown_node"); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    LOGOS_ASSERT_TRUE(t.cFunctionCalled("shutdown_node"));
    LOGOS_ASSERT_EQ(read_node_status(module).at("state").get<std::string>(), std::string("stopped"));
}

LOGOS_TEST(node_action_start_acknowledges_before_blocking_node_start_finishes) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmp_dir;
    LogosBlockchainModule module;
    t.mockCFunction("generate_user_config").returns(0);
    t.mockCFunction("start_lb_node").returns(1);
    t.mockCFunction("subscribe_to_new_blocks").returns(0);

    LOGOS_ASSERT_TRUE(invoke_node_action(
                          module,
                          lifecycle_command(
                              "bedrock-async-start-initialize",
                              "initialize",
                              { {"config", json({{"output", tmp_dir.filePath("node.json")}}).dump()} }
                          )
    )
                          .at("accepted")
                          .get<bool>());

    reset_node_changed_events();
    reset_mock_start_control();
    set_mock_start_blocked(true);
    const auto started_at = std::chrono::steady_clock::now();
    const json acknowledgement = json::parse(
        module.nodeAction(lifecycle_command("bedrock-async-start", "start").dump())
    );
    const auto acknowledgement_time = std::chrono::steady_clock::now() - started_at;
    LOGOS_ASSERT_TRUE(acknowledgement.at("accepted").get<bool>());
    LOGOS_ASSERT_TRUE(acknowledgement_time < std::chrono::milliseconds(500));

    const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!mock_start_entered() && std::chrono::steady_clock::now() < start_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    LOGOS_ASSERT_TRUE(mock_start_entered());

    const json pending = read_node_status(module);
    LOGOS_ASSERT_EQ(pending.at("state").get<std::string>(), std::string("starting"));
    LOGOS_ASSERT_FALSE(pending.at("pending_operation").is_null());

    set_mock_start_blocked(false);
    const json settled = wait_for_lifecycle_terminal(module);
    LOGOS_ASSERT_EQ(settled.at("state").get<std::string>(), std::string("running"));
    LOGOS_ASSERT_TRUE(settled.at("pending_operation").is_null());
    const std::vector<json> events = lifecycle_events();
    LOGOS_ASSERT_EQ(events.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(events.at(0).at("phase").get<std::string>(), std::string("accepted"));
    LOGOS_ASSERT_EQ(events.at(1).at("phase").get<std::string>(), std::string("settled"));
    LOGOS_ASSERT_EQ(events.at(1).at("outcome").get<std::string>(), std::string("succeeded"));
    reset_mock_start_control();
}

LOGOS_TEST(node_action_restarts_a_legacy_started_node) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmp_dir;
    LogosBlockchainModule module;
    t.mockCFunction("start_lb_node").returns(1);
    t.mockCFunction("subscribe_to_new_blocks").returns(0);
    t.mockCFunction("shutdown_node").returns(0);

    LOGOS_ASSERT_TRUE(module.start(tmp_dir.filePath("node.json"), "").success);
    LOGOS_ASSERT_EQ(read_node_status(module).at("state").get<std::string>(), std::string("running"));

    const json stopped = invoke_node_action(module, lifecycle_command("bedrock-stop-legacy-v1", "stop"));
    LOGOS_ASSERT_TRUE(stopped.at("accepted").get<bool>());
    LOGOS_ASSERT_EQ(read_node_status(module).at("state").get<std::string>(), std::string("stopped"));

    const json restarted = invoke_node_action(module, lifecycle_command("bedrock-restart-legacy-v1", "start"));
    LOGOS_ASSERT_TRUE(restarted.at("accepted").get<bool>());
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("start_lb_node"), 2);
    LOGOS_ASSERT_EQ(read_node_status(module).at("state").get<std::string>(), std::string("running"));
}

LOGOS_TEST(node_action_rejects_stale_and_reused_operation_ids_without_dispatch) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmp_dir;
    LogosBlockchainModule module;
    t.mockCFunction("generate_user_config").returns(0);
    t.mockCFunction("start_lb_node").returns(1);
    t.mockCFunction("subscribe_to_new_blocks").returns(0);

    LOGOS_ASSERT_TRUE(invoke_node_action(
                          module,
                          lifecycle_command(
                              "bedrock-initialize-rejections",
                              "initialize",
                              {{"config", json({{"output", tmp_dir.filePath("node.json")}}).dump()}}
                          )
    )
                          .at("accepted")
                          .get<bool>());
    const json status = read_node_status(module);
    json stale = lifecycle_command("bedrock-stale-start-v1", "start");
    stale["expected"] = {
        {"instance_id", status.at("instance_id")},
        {"epoch", status.at("epoch")},
        {"sequence", status.at("sequence").get<std::uint64_t>() + 1},
    };
    reset_node_changed_events();
    const json stale_acknowledgement = invoke_node_action(module, stale);
    LOGOS_ASSERT_FALSE(stale_acknowledgement.at("accepted").get<bool>());
    LOGOS_ASSERT_EQ(stale_acknowledgement.at("error").at("code").get<std::string>(), std::string("state_mismatch"));
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("start_lb_node"));
    const std::vector<json> stale_events = lifecycle_events();
    LOGOS_ASSERT_EQ(stale_events.size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(stale_events.at(0).at("outcome").get<std::string>(), std::string("rejected"));

    reset_node_changed_events();
    const json start = lifecycle_command("bedrock-reused-operation-v1", "start");
    LOGOS_ASSERT_TRUE(invoke_node_action(module, start).at("accepted").get<bool>());
    const int starts_after_first_request = t.cFunctionCallCount("start_lb_node");
    const json duplicate = invoke_node_action(module, start);
    LOGOS_ASSERT_TRUE(duplicate.at("accepted").get<bool>());
    LOGOS_ASSERT_TRUE(duplicate.at("duplicate").get<bool>());
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("start_lb_node"), starts_after_first_request);
    const json conflict = invoke_node_action(module, lifecycle_command("bedrock-reused-operation-v1", "stop"));
    LOGOS_ASSERT_FALSE(conflict.at("accepted").get<bool>());
    LOGOS_ASSERT_EQ(conflict.at("error").at("code").get<std::string>(), std::string("operation_id_conflict"));

    LOGOS_ASSERT_TRUE(module.stop().success);
}

LOGOS_TEST(node_action_reports_safe_start_and_stop_failures) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmp_dir;
    LogosBlockchainModule module;
    t.mockCFunction("generate_user_config").returns(0);
    t.mockCFunction("start_lb_node").returns(0);

    LOGOS_ASSERT_TRUE(invoke_node_action(
                          module,
                          lifecycle_command(
                              "bedrock-initialize-failure",
                              "initialize",
                              {{"config", json({{"output", tmp_dir.filePath("node.json")}}).dump()}}
                          )
    )
                          .at("accepted")
                          .get<bool>());
    reset_node_changed_events();
    const json start = invoke_node_action(module, lifecycle_command("bedrock-failing-start-v1", "start"));
    LOGOS_ASSERT_TRUE(start.at("accepted").get<bool>());
    const std::vector<json> events = lifecycle_events();
    LOGOS_ASSERT_EQ(events.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(events.at(1).at("outcome").get<std::string>(), std::string("failed"));
    LOGOS_ASSERT_EQ(events.at(1).at("error").at("code").get<std::string>(), std::string("start_failed"));
    LOGOS_ASSERT_TRUE(events.at(1).dump().find("mock error") == std::string::npos);
    LOGOS_ASSERT_EQ(read_node_status(module).at("state").get<std::string>(), std::string("stopped"));

    t.mockCFunction("start_lb_node").returns(1);
    t.mockCFunction("subscribe_to_new_blocks").returns(0);
    LOGOS_ASSERT_TRUE(module.start(tmp_dir.filePath("node.json"), "").success);
    t.mockCFunction("shutdown_node").returns(1);
    LOGOS_ASSERT_FALSE(module.stop().success);
    LOGOS_ASSERT_EQ(read_node_status(module).at("state").get<std::string>(), std::string("stopped"));

    const auto second_stop = module.stop();
    LOGOS_ASSERT_FALSE(second_stop.success);
    LOGOS_ASSERT_TRUE(second_stop.error.find("not running") != std::string::npos);
}

LOGOS_TEST(start_subscription_failure_clears_consumed_node_handle) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmp_dir;
    LogosBlockchainModule module;
    t.mockCFunction("start_lb_node").returns(1);
    t.mockCFunction("subscribe_to_new_blocks").returns(1);
    t.mockCFunction("shutdown_node").returns(1);

    const StdLogosResult failed = module.start(tmp_dir.filePath("node.json"), "");
    LOGOS_ASSERT_FALSE(failed.success);
    LOGOS_ASSERT_TRUE(failed.error.find("subscribe") != std::string::npos);

    t.mockCFunction("subscribe_to_new_blocks").returns(0);
    const StdLogosResult retried = module.start(tmp_dir.filePath("node.json"), "");
    LOGOS_ASSERT_TRUE(retried.success);
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("start_lb_node"), 2);
    t.mockCFunction("shutdown_node").returns(0);
    LOGOS_ASSERT_TRUE(module.stop().success);
}

// The mock records the paths handed to the FFI (see mock_logos_blockchain.cpp).
extern std::string g_lastGeneratedOutput;
extern std::string g_lastGeneratedStatePath;
extern std::string g_lastGeneratedStoragePath;
extern std::string g_lastGeneratedLogsPath;
extern int g_lastGeneratedIbd;
extern uint64_t g_lastFinalizedBlocksRangeFromSlot;
extern uint64_t g_lastFinalizedBlocksRangeToSlot;
extern uint64_t g_lastFinalizedBlocksRangeLimit;
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
    g_lastGeneratedIbd = -1;
}

// Basecamp opts in with the flag: output and state/storage/logs are routed under
// the per-instance persistence dir. The output keeps its given path as the
// relative part below the base.
LOGOS_TEST(generate_user_config_routes_paths_under_persistence_when_flagged) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmp_dir;
    LOGOS_ASSERT_TRUE(tmp_dir.isValid());
    const fs::path persistence = tmp_dir.path / "persistence" / "data";

    LogosBlockchainModule module;
    module._logosCoreSetContext_("/mod", "id", persistence.string());

    t.mockCFunction("generate_user_config").returns(0);
    clearGeneratedPaths();

    StdLogosResult result =
        module.generate_user_config(R"({"output":"config/user_config.yaml","use_persistence_paths":true})");
    LOGOS_ASSERT_TRUE(result.success);
    // The resolved config path is returned for the caller to hand to start().
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), (persistence / "config/user_config.yaml").string());
    LOGOS_ASSERT_EQ(g_lastGeneratedOutput, (persistence / "config/user_config.yaml").string());
    LOGOS_ASSERT_EQ(g_lastGeneratedStatePath, (persistence / "state").string());
    LOGOS_ASSERT_EQ(g_lastGeneratedStoragePath, (persistence / "db").string());
    LOGOS_ASSERT_EQ(g_lastGeneratedLogsPath, (persistence / "logs").string());
}

// Persistence-routed nested config outputs must be usable by the underlying
// generator even when neither the instance directory nor the output parent
// exists yet.
LOGOS_TEST(generate_user_config_creates_nested_persistence_output_parent) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmp_dir;
    LOGOS_ASSERT_TRUE(tmp_dir.isValid());

    const fs::path persistence = tmp_dir.path / "persistence";
    const fs::path expected_output = persistence / "config" / "nested" / "user_config.yaml";
    LOGOS_ASSERT_FALSE(fs::exists(expected_output.parent_path()));

    LogosBlockchainModule module;
    module._logosCoreSetContext_("/mod", "id", persistence.string());
    t.mockCFunction("generate_user_config").returns(0);
    clearGeneratedPaths();

    const StdLogosResult result =
        module.generate_user_config(R"({"output":"config/nested/user_config.yaml","use_persistence_paths":true})");

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_TRUE(fs::is_directory(expected_output.parent_path()));
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), expected_output.string());
    LOGOS_ASSERT_EQ(g_lastGeneratedOutput, expected_output.string());
    LOGOS_ASSERT(t.cFunctionCalled("generate_user_config"));
}

// A root-anchored output (the UI's "//user_config.yaml" artifact) is treated as
// relative to the base, not written to the read-only filesystem root.
LOGOS_TEST(generate_user_config_routes_root_anchored_output_under_persistence) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmp_dir;
    LOGOS_ASSERT_TRUE(tmp_dir.isValid());
    const fs::path persistence = tmp_dir.path / "persistence" / "data";

    LogosBlockchainModule module;
    module._logosCoreSetContext_("/mod", "id", persistence.string());

    t.mockCFunction("generate_user_config").returns(0);
    clearGeneratedPaths();

    LOGOS_ASSERT_TRUE(
        module.generate_user_config(R"({"output":"//user_config.yaml","use_persistence_paths":true})").success
    );
    LOGOS_ASSERT_EQ(g_lastGeneratedOutput, (persistence / "user_config.yaml").string());
}

// With no output given, the config defaults to <base>/user_config.yaml.
LOGOS_TEST(generate_user_config_defaults_output_under_persistence) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmp_dir;
    LOGOS_ASSERT_TRUE(tmp_dir.isValid());
    const fs::path persistence = tmp_dir.path / "persistence" / "data";

    LogosBlockchainModule module;
    module._logosCoreSetContext_("/mod", "id", persistence.string());

    t.mockCFunction("generate_user_config").returns(0);
    clearGeneratedPaths();

    LOGOS_ASSERT_TRUE(module.generate_user_config(R"({"use_persistence_paths":true})").success);
    LOGOS_ASSERT_EQ(g_lastGeneratedOutput, (persistence / "user_config.yaml").string());
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
    TempDir tmp_dir;
    LOGOS_ASSERT_TRUE(tmp_dir.isValid());
    const fs::path persistence = tmp_dir.path / "persistence" / "data";

    LogosBlockchainModule module;
    module._logosCoreSetContext_("/mod", "id", persistence.string());

    t.mockCFunction("generate_user_config").returns(0);
    clearGeneratedPaths();

    LOGOS_ASSERT_TRUE(module
                          .generate_user_config(
                              R"({"output":"/tmp/out.json","state_path":"/tmp/state","use_persistence_paths":true})"
                          )
                          .success);
    LOGOS_ASSERT_EQ(g_lastGeneratedStatePath, std::string("/tmp/state"));
    LOGOS_ASSERT_EQ(g_lastGeneratedStoragePath, (persistence / "db").string());
    LOGOS_ASSERT_EQ(g_lastGeneratedLogsPath, (persistence / "logs").string());
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
    LOGOS_ASSERT_EQ(g_lastGeneratedIbd, 1);
}

LOGOS_TEST(generate_user_config_preserves_skip_ibd_and_legacy_ibd_inputs) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    t.mockCFunction("generate_user_config").returns(0);

    clearGeneratedPaths();
    LOGOS_ASSERT_TRUE(module.generate_user_config(R"({"skip_ibd":false})").success);
    LOGOS_ASSERT_EQ(g_lastGeneratedIbd, 1);

    clearGeneratedPaths();
    LOGOS_ASSERT_TRUE(module.generate_user_config(R"({"skip_ibd":true})").success);
    LOGOS_ASSERT_EQ(g_lastGeneratedIbd, 0);

    clearGeneratedPaths();
    LOGOS_ASSERT_TRUE(module.generate_user_config(R"({"ibd":false})").success);
    LOGOS_ASSERT_EQ(g_lastGeneratedIbd, 0);

    clearGeneratedPaths();
    LOGOS_ASSERT_TRUE(module.generate_user_config("{}").success);
    LOGOS_ASSERT_EQ(g_lastGeneratedIbd, 1);
}

// ============================================================================
// No-node error paths — all methods should fail gracefully
// ============================================================================

LOGOS_TEST(stop_without_node_returns_1) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_FALSE(module.stop().success);
}

LOGOS_TEST(stop_without_node_preserves_uninitialized_lifecycle_state) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;

    LOGOS_ASSERT_FALSE(module.stop().success);
    const auto status = json::parse(module.nodeStatus());
    LOGOS_ASSERT_EQ(status.at("state").get<std::string>(), "uninitialized");
    LOGOS_ASSERT_EQ(status.at("supported_actions").at(0).get<std::string>(), "initialize");
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
    StdLogosResult result = module.blend_join_as_core_node(VALID_HEX, VALID_HEX, VALID_HEX, {});
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

LOGOS_TEST(get_time_info_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_FALSE(module.get_time_info().success);
}

LOGOS_TEST(get_finalized_blocks_range_without_node_returns_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_FALSE(module.get_finalized_blocks_range(1, 10, 10).success);
}

LOGOS_TEST(diagnostic_reads_without_node_return_error) {
    auto t = LogosTestContext("blockchain_module");
    LogosBlockchainModule module;
    LOGOS_ASSERT_FALSE(module.get_cryptarchia_headers().success);
    LOGOS_ASSERT_FALSE(module.get_network_info().success);
    LOGOS_ASSERT_FALSE(module.get_mantle_metrics().success);
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

LOGOS_TEST(blend_join_rejects_invalid_provider_id) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->blend_join_as_core_node("short", VALID_HEX, VALID_HEX, {});
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "provider_id"));
    delete module;
}

LOGOS_TEST(blend_join_rejects_invalid_zk_id) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->blend_join_as_core_node(VALID_HEX, "short", VALID_HEX, {});
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "zk_id"));
    delete module;
}

LOGOS_TEST(blend_join_rejects_invalid_locked_note_id) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    StdLogosResult result = module->blend_join_as_core_node(VALID_HEX, VALID_HEX, "short", {});
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
    auto* module = createStartedModuleWithConfig(
        t,
        tmpDir,
        "public_keys:\n  BlendSigning: " + VALID_HEX + "\n  BlendZk: " + VALID_HEX + "\n"
    );
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("blend_join_as_core_node_error").returns(0);

    StdLogosResult result = module->blend_join_as_core_node(VALID_HEX, VALID_HEX, VALID_HEX, {"locator1"});
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
    auto* module = createStartedModuleWithConfig(
        t,
        tmpDir,
        "public_keys:\n  BlendSigning: " + VALID_HEX + "\n  BlendZk: " + VALID_HEX + "\n"
    );
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("blend_join_as_core_node_error").returns(1);

    StdLogosResult result = module->blend_join_as_core_node(VALID_HEX, VALID_HEX, VALID_HEX, {"locator1"});
    LOGOS_ASSERT_FALSE(result.success);
    delete module;
}

LOGOS_TEST(blend_join_rejects_multiple_locators_without_dropping_input) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModuleWithConfig(
        t,
        tmpDir,
        "public_keys:\n  BlendSigning: " + VALID_HEX + "\n  BlendZk: " + VALID_HEX + "\n"
    );
    LOGOS_ASSERT_TRUE(module != nullptr);

    const StdLogosResult result =
        module->blend_join_as_core_node(VALID_HEX, VALID_HEX, VALID_HEX, {"locator1", "locator2"});
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "exactly one locator"));
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("blend_join_as_core_node"));
    delete module;
}

LOGOS_TEST(blend_join_rejects_identities_that_do_not_match_configuration) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModuleWithConfig(
        t,
        tmpDir,
        "public_keys:\n  BlendSigning: " + VALID_HEX + "\n  BlendZk: " + VALID_HEX + "\n"
    );
    LOGOS_ASSERT_TRUE(module != nullptr);

    const StdLogosResult result = module->blend_join_as_core_node(
        std::string(64, 'b'), VALID_HEX, VALID_HEX, {"locator1"}
    );
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "BlendSigning identity"));
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("blend_join_as_core_node"));
    delete module;
}

LOGOS_TEST(blend_join_uses_identities_captured_at_startup) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModuleWithConfig(
        t,
        tmpDir,
        "public_keys:\n  BlendSigning: " + VALID_HEX + "\n  BlendZk: " + VALID_HEX + "\n"
    );
    LOGOS_ASSERT_TRUE(module != nullptr);

    // Editing the path after startup must not change the identities attached
    // to the live node.
    std::ofstream config(tmpDir.filePath("config.json"));
    config << "public_keys:\n  BlendSigning: " << std::string(64, 'b') << "\n  BlendZk: "
           << std::string(64, 'c') << "\n";
    config.close();

    t.mockCFunction("blend_join_as_core_node_error").returns(0);
    const StdLogosResult result = module->blend_join_as_core_node(
        VALID_HEX, VALID_HEX, VALID_HEX, {"locator1"}
    );
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_TRUE(t.cFunctionCalled("blend_join_as_core_node"));
    delete module;
}

LOGOS_TEST(blend_join_rejects_unverifiable_runtime_kms_identity_configuration) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModuleWithConfig(
        t,
        tmpDir,
        "blend: {non_ephemeral_signing_key_id: signing-kms-id, "
        "core: {zk: {secret_key_kms_id: zk-kms-id}}}\n"
    );
    LOGOS_ASSERT_TRUE(module != nullptr);

    // Production node configs expose KMS key IDs. The running C binding
    // resolves those IDs to the public identities used by the join request.
    const StdLogosResult result = module->blend_join_as_core_node(
        VALID_HEX, VALID_HEX, VALID_HEX, {"/ip4/127.0.0.1/udp/4040/quic-v1"}
    );
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "Unable to verify Blend identities"));
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("blend_join_as_core_node"));
    delete module;
}

LOGOS_TEST(blend_join_accepts_quoted_public_key_configuration) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModuleWithConfig(
        t,
        tmpDir,
        "public_keys: {\"BlendSigning\": \"" + VALID_HEX + "\", 'BlendZk': \"" + VALID_HEX + "\"}\n"
    );
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("blend_join_as_core_node_error").returns(0);

    const StdLogosResult result = module->blend_join_as_core_node(
        VALID_HEX, VALID_HEX, VALID_HEX, {"/ip4/127.0.0.1/udp/4040/quic-v1"}
    );
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_TRUE(t.cFunctionCalled("blend_join_as_core_node"));
    delete module;
}

LOGOS_TEST(blend_join_accepts_whitespace_before_mapping_colon) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModuleWithConfig(
        t,
        tmpDir,
        "public_keys:\n  BlendSigning : " + VALID_HEX + "\n  BlendZk : " + VALID_HEX + "\n"
    );
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("blend_join_as_core_node_error").returns(0);
    const StdLogosResult result = module->blend_join_as_core_node(
        VALID_HEX, VALID_HEX, VALID_HEX, {"locator1"}
    );
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_TRUE(t.cFunctionCalled("blend_join_as_core_node"));
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

LOGOS_TEST(get_block_normalizes_core_transaction_id_to_mantle_hash) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    const std::string transaction_id = "0x" + std::string(64, 'C');
    const std::string response = std::string(
        R"({"header":{"slot":42},"transactions":[{"id":")"
    ) + transaction_id + R"(","mantle_tx":{"ops":[]}}]})";
    t.mockCFunction("get_block").returns(response.c_str());
    t.mockCFunction("get_block_error").returns(0);

    const StdLogosResult result = module->get_block(VALID_HEX);
    LOGOS_ASSERT_TRUE(result.success);
    const json block = json::parse(result.value.get<std::string>());
    LOGOS_ASSERT_EQ(
        block["transactions"][0]["mantle_tx"]["hash"].get<std::string>(),
        std::string(64, 'c')
    );
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

LOGOS_TEST(get_time_info_projects_the_ffi_json) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    const std::string expected = R"({"slot":42,"unix_time":1234})";
    t.mockCFunction("get_time_info").returns(expected.c_str());
    t.mockCFunction("get_time_info_error").returns(0);

    const StdLogosResult result = module->get_time_info();
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), expected);
    LOGOS_ASSERT(t.cFunctionCalled("get_time_info"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
    delete module;
}

LOGOS_TEST(get_time_info_propagates_ffi_error_and_rejects_empty_success) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_time_info_error").returns(1);
    LOGOS_ASSERT_FALSE(module->get_time_info().success);

    t.mockCFunction("get_time_info_error").returns(0);
    t.mockCFunction("get_time_info").returns(static_cast<const char*>(nullptr));
    const StdLogosResult empty = module->get_time_info();
    LOGOS_ASSERT_FALSE(empty.success);
    LOGOS_ASSERT_EQ(empty.error, std::string("get_time_info returned an empty response."));
    delete module;
}

LOGOS_TEST(get_finalized_blocks_range_projects_the_snapshot_wire_contract) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    const std::string expected =
        R"([{"block":{"header":{"slot":40}},"tip":"tip","tip_slot":42,"lib":"lib","lib_slot":40}])";
    t.mockCFunction("get_finalized_blocks_range").returns(expected.c_str());
    t.mockCFunction("get_finalized_blocks_range_error").returns(0);

    const StdLogosResult result = module->get_finalized_blocks_range(40, 42, 3);
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), expected);
    LOGOS_ASSERT_EQ(g_lastFinalizedBlocksRangeFromSlot, static_cast<uint64_t>(40));
    LOGOS_ASSERT_EQ(g_lastFinalizedBlocksRangeToSlot, static_cast<uint64_t>(42));
    LOGOS_ASSERT_EQ(g_lastFinalizedBlocksRangeLimit, static_cast<uint64_t>(3));
    LOGOS_ASSERT(t.cFunctionCalled("get_finalized_blocks_range"));
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("get_blocks"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cstring"));
    delete module;
}

LOGOS_TEST(get_finalized_blocks_range_propagates_ffi_error_and_rejects_empty_success) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_finalized_blocks_range_error").returns(1);
    LOGOS_ASSERT_FALSE(module->get_finalized_blocks_range(40, 42, 3).success);

    t.mockCFunction("get_finalized_blocks_range_error").returns(0);
    t.mockCFunction("get_finalized_blocks_range").returns(static_cast<const char*>(nullptr));
    const StdLogosResult empty = module->get_finalized_blocks_range(40, 42, 3);
    LOGOS_ASSERT_FALSE(empty.success);
    LOGOS_ASSERT_EQ(empty.error, std::string("get_finalized_blocks_range returned an empty response."));
    delete module;
}

LOGOS_TEST(diagnostic_reads_project_the_ffi_json) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    const std::string headers = R"(["header-a","header-b"])";
    const std::string network = R"({"n_peers":3})";
    const std::string metrics = R"({"transactions":1})";
    t.mockCFunction("get_cryptarchia_headers").returns(headers.c_str());
    t.mockCFunction("get_cryptarchia_headers_error").returns(0);
    t.mockCFunction("get_network_info").returns(network.c_str());
    t.mockCFunction("get_network_info_error").returns(0);
    t.mockCFunction("get_mantle_metrics").returns(metrics.c_str());
    t.mockCFunction("get_mantle_metrics_error").returns(0);

    const StdLogosResult headers_result = module->get_cryptarchia_headers();
    const StdLogosResult network_result = module->get_network_info();
    const StdLogosResult metrics_result = module->get_mantle_metrics();
    LOGOS_ASSERT_TRUE(headers_result.success);
    LOGOS_ASSERT_TRUE(network_result.success);
    LOGOS_ASSERT_TRUE(metrics_result.success);
    LOGOS_ASSERT_EQ(headers_result.value.get<std::string>(), headers);
    LOGOS_ASSERT_EQ(network_result.value.get<std::string>(), network);
    LOGOS_ASSERT_EQ(metrics_result.value.get<std::string>(), metrics);
    LOGOS_ASSERT(t.cFunctionCalled("get_cryptarchia_headers"));
    LOGOS_ASSERT(t.cFunctionCalled("get_network_info"));
    LOGOS_ASSERT(t.cFunctionCalled("get_mantle_metrics"));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("free_cstring"), 3);
    delete module;
}

LOGOS_TEST(diagnostic_reads_propagate_ffi_errors_and_reject_empty_success) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_cryptarchia_headers_error").returns(1);
    LOGOS_ASSERT_FALSE(module->get_cryptarchia_headers().success);
    t.mockCFunction("get_network_info_error").returns(1);
    LOGOS_ASSERT_FALSE(module->get_network_info().success);
    t.mockCFunction("get_mantle_metrics_error").returns(1);
    LOGOS_ASSERT_FALSE(module->get_mantle_metrics().success);

    t.mockCFunction("get_cryptarchia_headers_error").returns(0);
    t.mockCFunction("get_cryptarchia_headers").returns(static_cast<const char*>(nullptr));
    const StdLogosResult empty = module->get_cryptarchia_headers();
    LOGOS_ASSERT_FALSE(empty.success);
    LOGOS_ASSERT_EQ(empty.error, std::string("get_cryptarchia_headers returned an empty response."));
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
    t.mockCFunction("cryptarchia_lib_slot").returns(99);

    StdLogosResult result = module->get_cryptarchia_info();
    LOGOS_ASSERT_TRUE(result.success);
    const json info = json::parse(result.value.get<std::string>());
    LOGOS_ASSERT_EQ(info["slot"].get<uint64_t>(), static_cast<uint64_t>(100));
    LOGOS_ASSERT_EQ(info["height"].get<uint64_t>(), static_cast<uint64_t>(50));
    LOGOS_ASSERT_EQ(info["mode"].get<std::string>(), std::string("Online"));
    LOGOS_ASSERT_TRUE(info.contains("lib"));
    LOGOS_ASSERT_TRUE(info.contains("tip"));
    LOGOS_ASSERT_EQ(info["genesis_id"].get<std::string>(), std::string(64, 'd'));
    LOGOS_ASSERT_EQ(info["lib_slot"].get<uint64_t>(), static_cast<uint64_t>(99));
    LOGOS_ASSERT(t.cFunctionCalled("get_cryptarchia_info"));
    LOGOS_ASSERT(t.cFunctionCalled("cryptarchia_info_abi_version"));
    LOGOS_ASSERT(t.cFunctionCalled("free_cryptarchia_info"));
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("get_block"));
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("free_cstring"));
    delete module;
}

LOGOS_TEST(get_cryptarchia_info_bootstrapping_mode) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("get_cryptarchia_info_error").returns(0);
    t.mockCFunction("cryptarchia_mode").returns(0); // Bootstrapping

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

LOGOS_TEST(get_cryptarchia_info_rejects_an_incompatible_abi_version) {
    auto t = LogosTestContext("blockchain_module");
    TempDir tmpDir;
    auto* module = createStartedModule(t, tmpDir);
    LOGOS_ASSERT_TRUE(module != nullptr);

    t.mockCFunction("cryptarchia_info_abi_version").returns(CRYPTARCHIA_INFO_ABI_VERSION + 1);

    StdLogosResult result = module->get_cryptarchia_info();
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_TRUE(contains(result.error, "Incompatible CryptarchiaInfo C ABI"));
    LOGOS_ASSERT(t.cFunctionCalled("cryptarchia_info_abi_version"));
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("get_cryptarchia_info"));
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
