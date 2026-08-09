#include "logos_blockchain_module.h"

#include <algorithm>
#include <atomic>
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Define static member
LogosBlockchainModule* LogosBlockchainModule::s_instance = nullptr;
std::mutex LogosBlockchainModule::s_instanceMutex;

namespace operation_status {
    // Takes the Rust-allocated message out of an OperationStatus and frees it.
    std::string take_message(OperationStatus& status) {
        std::string message;
        if (status.message) {
            message = status.message;
            (void)free_cstring(status.message);
            status.message = nullptr;
        }
        return message;
    }
} // namespace operation_status

// Shorthands for building StdLogosResult values.
namespace result {
    StdLogosResult ok() {
        return {true};
    }

    template <typename T>
    StdLogosResult ok(T value) { // NOLINT(performance-unnecessary-value-param)
        return {true, std::move(value)};
    }

    StdLogosResult err(std::string message) {
        return {false, {}, std::move(message)};
    }

    StdLogosResult from_operation_status(OperationStatus& status) {
        if (is_ok(&status)) {
            return ok();
        }
        return err(operation_status::take_message(status));
    }
} // namespace result

namespace {
    constexpr size_t MAX_TIP_PARENT_WALK_BLOCKS = 500;
    constexpr size_t MAX_NODE_LIFECYCLE_REQUEST_BYTES = 64 * 1024;
    constexpr size_t MAX_NODE_LIFECYCLE_CONFIG_BYTES = 48 * 1024;
    constexpr size_t MAX_NODE_LIFECYCLE_DEPLOYMENT_BYTES = 4 * 1024;
    constexpr size_t MAX_NODE_LIFECYCLE_OPERATION_ID_BYTES = 128;
    constexpr size_t MAX_COMPLETED_NODE_LIFECYCLE_OPERATIONS = 64;
    constexpr const char* NODE_LIFECYCLE_SNAPSHOT_SCHEMA = "logos.managed_node_lifecycle.snapshot";
    constexpr const char* NODE_LIFECYCLE_COMMAND_SCHEMA = "logos.managed_node_lifecycle.command";
    constexpr const char* NODE_LIFECYCLE_ACK_SCHEMA = "logos.managed_node_lifecycle.ack";
    constexpr const char* NODE_LIFECYCLE_EVENT_SCHEMA = "logos.managed_node_lifecycle.event";
    constexpr const char* PERSISTED_LIFECYCLE_STATE_FILE = ".logos-blockchain-lifecycle-v1.json";
    constexpr const char* PERSISTED_LIFECYCLE_STATE_SCHEMA = "logos.blockchain.lifecycle_config";
    constexpr size_t MAX_PERSISTED_LIFECYCLE_STATE_BYTES = 4 * 1024;
    std::atomic<std::uint64_t> node_lifecycle_instance_counter{0};
    thread_local const void* active_callback_lifetime = nullptr;

    std::int64_t nodeLifecycleTimestampMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()
        )
            .count();
    }

    std::string makeNodeLifecycleInstanceId() {
        return "blockchain-" + std::to_string(nodeLifecycleTimestampMs()) + "-" +
               std::to_string(node_lifecycle_instance_counter.fetch_add(1) + 1);
    }

    bool containsEmbeddedNul(const std::string& value) {
        return value.find('\0') != std::string::npos;
    }

    bool isValidNodeLifecycleOperationId(const std::string& value) {
        if (value.empty() || value.size() > MAX_NODE_LIFECYCLE_OPERATION_ID_BYTES) {
            return false;
        }
        return std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return std::isalnum(character) || character == '.' || character == '_' || character == ':' ||
                   character == '-';
        });
    }

    bool parseLifecycleUnsigned(const json& value, std::uint64_t& output) {
        if (value.is_number_unsigned()) {
            output = value.get<std::uint64_t>();
            return true;
        }
        if (value.is_number_integer()) {
            const auto signed_value = value.get<std::int64_t>();
            if (signed_value >= 0) {
                output = static_cast<std::uint64_t>(signed_value);
                return true;
            }
        }
        return false;
    }

    bool isLifecycleVersionOne(const json& value) {
        std::uint64_t version = 0;
        return parseLifecycleUnsigned(value, version) && version == 1;
    }

    json nodeLifecycleError(const std::string& code, const std::string& message, const std::int64_t at_ms) {
        return {
            {"code", code},
            {"message", message},
            {"at_ms", at_ms},
        };
    }

    // Rust `File::open` / `deserialize_config_at_path` only accept real filesystem paths. QML often
    // passes `file:///...` URLs; strip to a local path when applicable.
    std::string localPathFromFileUrl(const std::string& s) {
        if (s.size() >= 7 && s.substr(0, 7) == "file://")
            return s.substr(7);
        if (s.size() >= 5 && s.substr(0, 5) == "file:")
            return s.substr(5);
        return s;
    }

    std::string resolvedConfigOutputPath(
        const json& config,
        const bool use_persistence_paths,
        const std::string& persistence_path
    ) {
        const auto output = config.find("output");
        const bool has_output = output != config.end() && output->is_string() && !output->get<std::string>().empty();
        if (!use_persistence_paths || persistence_path.empty()) {
            return has_output ? output->get<std::string>() : std::string();
        }

        fs::path output_relative = "user_config.yaml";
        if (has_output) {
            const fs::path given(localPathFromFileUrl(output->get<std::string>()));
            const fs::path relative = given.relative_path();
            output_relative = relative.empty() ? given.filename() : relative;
            if (output_relative.empty())
                output_relative = "user_config.yaml";
        }
        return (fs::path(persistence_path) / output_relative).lexically_normal().string();
    }

    std::string existingRegularFilePath(const std::string& candidate) {
        if (candidate.empty() || containsEmbeddedNul(candidate))
            return {};
        const fs::path path(localPathFromFileUrl(candidate));
        std::error_code error;
        if (!fs::is_regular_file(path, error) || error)
            return {};
        return path.lexically_normal().string();
    }

    // Use the C API type Hash (from logos_blockchain.h) to define address/hash byte size.
    constexpr int ADDRESS_BYTES = sizeof(Hash);
    constexpr int TX_HASH_BYTES = sizeof(TxHash);
    constexpr int ADDRESS_HEX_LEN = ADDRESS_BYTES * 2;
    constexpr uint32_t EXPECTED_CRYPTARCHIA_INFO_ABI_VERSION = CRYPTARCHIA_INFO_ABI_VERSION;

    std::vector<uint8_t> parse_address_hex(const std::string& address_hex) {
        std::string hex = address_hex;
        boost::algorithm::trim(hex);
        if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
            hex = hex.substr(2);
        if (static_cast<int>(hex.size()) != ADDRESS_HEX_LEN)
            return {};
        try {
            std::string decoded;
            boost::algorithm::unhex(hex.begin(), hex.end(), std::back_inserter(decoded));
            return {decoded.begin(), decoded.end()};
        } catch (const boost::algorithm::non_hex_input&) {
            return {};
        }
    }

    std::string configured_blend_public_key(const std::string& config_path, const char* key) {
        if (config_path.empty() || !key || !*key) {
            return {};
        }

        std::ifstream input(config_path, std::ios::binary);
        if (!input) {
            return {};
        }

        std::string line;
        while (std::getline(input, line)) {
            const auto comment = line.find('#');
            if (comment != std::string::npos) {
                line.erase(comment);
            }
            const std::vector<std::string> key_tokens = {
                std::string(key),
                "\"" + std::string(key) + "\"",
                "'" + std::string(key) + "'",
            };
            std::string::size_type key_position = std::string::npos;
            std::string::size_type prefix_size = 0;
            for (const auto& token : key_tokens) {
                const auto candidate = line.find(token);
                if (candidate == std::string::npos ||
                    (candidate > 0 &&
                     (std::isalnum(static_cast<unsigned char>(line[candidate - 1])) ||
                      line[candidate - 1] == '_'))) {
                    continue;
                }
                auto colon = candidate + token.size();
                while (colon < line.size() && std::isspace(static_cast<unsigned char>(line[colon]))) {
                    ++colon;
                }
                if (colon >= line.size() || line[colon] != ':') {
                    continue;
                }
                key_position = candidate;
                prefix_size = colon - candidate + 1;
                break;
            }
            if (key_position == std::string::npos) {
                continue;
            }

            std::string value = line.substr(key_position + prefix_size);
            const auto delimiter = value.find_first_of(",}");
            if (delimiter != std::string::npos) {
                value.erase(delimiter);
            }
            boost::algorithm::trim(value);
            if (value.size() >= 2 &&
                ((value.front() == '\'' && value.back() == '\'') ||
                 (value.front() == '"' && value.back() == '"'))) {
                value = value.substr(1, value.size() - 2);
            }
            return value;
        }
        return {};
    }

    // Parse arbitrary-length hex (optional 0x prefix) into bytes. Unlike
    // parse_address_hex this does not enforce a fixed length; used for the
    // variable-length channel deposit metadata. Returns false on odd length or
    // non-hex input.
    bool parse_hex_bytes(const std::string& hex_in, std::vector<uint8_t>& out) {
        std::string hex = hex_in;
        boost::algorithm::trim(hex);
        if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
            hex = hex.substr(2);
        if (hex.size() % 2 != 0)
            return false;
        try {
            std::string decoded;
            boost::algorithm::unhex(hex.begin(), hex.end(), std::back_inserter(decoded));
            out.assign(decoded.begin(), decoded.end());
            return true;
        } catch (const boost::algorithm::non_hex_input&) {
            return false;
        }
    }

    std::string bytes_to_hex(const uint8_t* data, const size_t len) {
        std::string out;
        out.reserve(len * 2);
        boost::algorithm::hex_lower(data, data + len, std::back_inserter(out));
        return out;
    }

    StdLogosResult copy_cstring_result(char* value, const char* label) {
        if (!value) {
            return result::err(std::string(label) + " returned an empty response.");
        }

        std::string out(value);
        OperationStatus free_status = free_cstring(value);
        if (!is_ok(&free_status)) {
            fprintf(
                stderr,
                "Failed to free %s string: %s\n",
                label,
                operation_status::take_message(free_status).c_str()
            );
        }
        return result::ok(std::move(out));
    }

    bool canonicalize_hash_field(
        json& object,
        const char* field,
        const std::string& requested_id,
        const char* label,
        std::string& error
    ) {
        const auto existing = object.find(field);
        if (existing == object.end() || existing->is_null() ||
            (existing->is_string() && existing->get<std::string>().empty())) {
            object[field] = requested_id;
            return true;
        }
        if (!existing->is_string()) {
            error = std::string(label) + " returned a non-string " + field + ".";
            return false;
        }

        const std::vector<uint8_t> bytes = parse_address_hex(existing->get<std::string>());
        if (bytes.empty()) {
            error = std::string(label) + " returned an invalid " + field + ".";
            return false;
        }
        const std::string actual_id = bytes_to_hex(bytes.data(), bytes.size());
        if (actual_id != requested_id) {
            error = std::string(label) + " returned " + field + " `" + actual_id +
                    "` for requested id `" + requested_id + "`.";
            return false;
        }

        object[field] = actual_id;
        return true;
    }

    // Core block serializers expose canonical transaction identity as the
    // envelope `id`; keep the module's established `mantle_tx.hash` shape.
    bool normalize_block_transaction_hashes(json& block) {
        if (!block.is_object()) {
            return false;
        }
        auto transactions = block.find("transactions");
        if (transactions == block.end() || !transactions->is_array()) {
            return false;
        }

        bool normalized = false;
        for (json& transaction : *transactions) {
            if (!transaction.is_object()) {
                continue;
            }
            const auto id = transaction.find("id");
            auto mantle_transaction = transaction.find("mantle_tx");
            if (id == transaction.end() || !id->is_string() || mantle_transaction == transaction.end() ||
                !mantle_transaction->is_object()) {
                continue;
            }

            const auto hash = mantle_transaction->find("hash");
            const bool missing_hash = hash == mantle_transaction->end() || hash->is_null() ||
                                      (hash->is_string() && hash->get<std::string>().empty());
            if (!missing_hash) {
                continue;
            }

            const std::vector<uint8_t> hash_bytes = parse_address_hex(id->get<std::string>());
            if (hash_bytes.empty()) {
                continue;
            }
            (*mantle_transaction)["hash"] = bytes_to_hex(hash_bytes.data(), hash_bytes.size());
            normalized = true;
        }
        return normalized;
    }

    StdLogosResult normalize_block_json(const std::string& raw, const std::string& requested_id) {
        json block;
        try {
            block = json::parse(raw);
        } catch (const json::parse_error&) {
            return result::ok(raw);
        }

        if (!block.is_object()) {
            return result::ok(raw);
        }
        auto header = block.find("header");
        if (header == block.end() || !header->is_object()) {
            return result::ok(raw);
        }

        std::string error;
        json& header_object = *header;
        if (header_object.contains("id")) {
            if (!canonicalize_hash_field(header_object, "id", requested_id, "get_block", error)) {
                return result::err(std::move(error));
            }
        } else if (header_object.contains("hash")) {
            if (!canonicalize_hash_field(header_object, "hash", requested_id, "get_block", error)) {
                return result::err(std::move(error));
            }
        }
        header_object["id"] = requested_id;
        normalize_block_transaction_hashes(block);
        return result::ok(block.dump());
    }

    StdLogosResult normalize_transaction_json(const std::string& raw, const std::string& requested_id) {
        json transaction;
        try {
            transaction = json::parse(raw);
        } catch (const json::parse_error&) {
            return result::ok(raw);
        }

        if (!transaction.is_object()) {
            return result::ok(raw);
        }

        json* transaction_object = &transaction;
        if (auto mantle_transaction = transaction.find("mantle_tx");
            mantle_transaction != transaction.end() && mantle_transaction->is_object()) {
            transaction_object = &*mantle_transaction;
        }

        std::string error;
        if (!canonicalize_hash_field(*transaction_object, "hash", requested_id, "get_transaction", error)) {
            return result::err(std::move(error));
        }
        return result::ok(transaction.dump());
    }

    bool block_slot(const json& block, uint64_t& slot) {
        if (!block.is_object()) {
            return false;
        }
        const auto header = block.find("header");
        if (header == block.end() || !header->is_object()) {
            return false;
        }
        const auto value = header->find("slot");
        if (value == header->end()) {
            return false;
        }
        if (value->is_number_unsigned()) {
            slot = value->get<uint64_t>();
            return true;
        }
        if (value->is_number_integer()) {
            const int64_t signed_slot = value->get<int64_t>();
            if (signed_slot >= 0) {
                slot = static_cast<uint64_t>(signed_slot);
                return true;
            }
        }
        return false;
    }

    bool parent_block_id(const json& block, std::string& parent_id) {
        if (!block.is_object()) {
            return false;
        }
        const auto header = block.find("header");
        if (header == block.end() || !header->is_object()) {
            return false;
        }
        const auto parent = header->find("parent_block");
        if (parent == header->end() || !parent->is_string()) {
            return false;
        }
        const std::vector<uint8_t> bytes = parse_address_hex(parent->get<std::string>());
        if (bytes.empty()) {
            return false;
        }
        parent_id = bytes_to_hex(bytes.data(), bytes.size());
        return true;
    }

    // Maps an `ed25519`/`zk` string (case-insensitive) to the C KeyType enum.
    bool parse_key_type(const std::string& s, KeyType& out) {
        std::string lower = s;
        boost::algorithm::trim(lower);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) {
            return std::tolower(c);
        });
        if (lower == "ed25519") {
            out = KeyType::Ed25519;
            return true;
        }
        if (lower == "zk") {
            out = KeyType::Zk;
            return true;
        }
        return false;
    }

    // Wrapper that owns data and provides GenerateConfigArgs
    struct OwnedGenerateConfigArgs {
        std::vector<std::string> initial_peers_data;
        std::vector<const char*> initial_peers_ptrs;
        uint32_t initial_peers_count_val;
        std::string output_data;
        uint16_t net_port_val;
        uint16_t blend_port_val;
        std::string http_addr_data;
        std::string external_address_data;
        std::string state_path_data;
        std::string storage_path_data;
        std::string logs_path_data;
        bool skip_ibd_val = false;
        std::string log_filter_data;
        std::string kms_file_data;

        // The FFI struct with pointers into owned data
        GenerateConfigArgs ffi_args{};

        // Constructor that populates both owned data and FFI struct from JSON
        explicit OwnedGenerateConfigArgs(const json& args) {
            // initial_peers (JSON array -> const char**)
            if (args.contains("initial_peers") && args["initial_peers"].is_array()) {
                for (const auto& peer : args["initial_peers"]) {
                    initial_peers_data.push_back(peer.get<std::string>());
                }
                initial_peers_count_val = static_cast<uint32_t>(initial_peers_data.size());

                for (const std::string& data : initial_peers_data) {
                    initial_peers_ptrs.push_back(data.c_str());
                }

                ffi_args.initial_peers = initial_peers_ptrs.data();
                ffi_args.initial_peers_count = &initial_peers_count_val;
            } else {
                ffi_args.initial_peers = nullptr;
                ffi_args.initial_peers_count = nullptr;
            }

            // output (string -> const char*)
            if (args.contains("output") && args["output"].is_string()) {
                output_data = args["output"].get<std::string>();
                ffi_args.output = output_data.c_str();
            } else {
                ffi_args.output = nullptr;
            }

            // net_port (int -> const uint16_t*)
            if (args.contains("net_port") && args["net_port"].is_number_integer()) {
                net_port_val = static_cast<uint16_t>(args["net_port"].get<int>());
                ffi_args.net_port = &net_port_val;
            } else {
                ffi_args.net_port = nullptr;
            }

            // blend_port (int -> const uint16_t*)
            if (args.contains("blend_port") && args["blend_port"].is_number_integer()) {
                blend_port_val = static_cast<uint16_t>(args["blend_port"].get<int>());
                ffi_args.blend_port = &blend_port_val;
            } else {
                ffi_args.blend_port = nullptr;
            }

            // http_addr (string -> const char*)
            if (args.contains("http_addr") && args["http_addr"].is_string()) {
                http_addr_data = args["http_addr"].get<std::string>();
                ffi_args.http_addr = http_addr_data.c_str();
            } else {
                ffi_args.http_addr = nullptr;
            }

            // external_address (string -> const char*)
            if (args.contains("external_address") && args["external_address"].is_string()) {
                external_address_data = args["external_address"].get<std::string>();
                ffi_args.external_address = external_address_data.c_str();
            } else {
                ffi_args.external_address = nullptr;
            }

            // state_path (string -> const char*)
            if (args.contains("state_path") && args["state_path"].is_string()) {
                state_path_data = args["state_path"].get<std::string>();
                ffi_args.state_path = state_path_data.c_str();
            } else {
                ffi_args.state_path = nullptr;
            }

            // storage_path (string -> const char*) — maps to storage.backend.folder_name
            if (args.contains("storage_path") && args["storage_path"].is_string()) {
                storage_path_data = args["storage_path"].get<std::string>();
                ffi_args.storage_path = storage_path_data.c_str();
            } else {
                ffi_args.storage_path = nullptr;
            }

            // logs_path (string -> const char*) — maps to tracing.logger.file.directory
            if (args.contains("logs_path") && args["logs_path"].is_string()) {
                logs_path_data = args["logs_path"].get<std::string>();
                ffi_args.logs_path = logs_path_data.c_str();
            } else {
                ffi_args.logs_path = nullptr;
            }

            // The C API accepts `skip_ibd`, while Inspector also accepts the
            // legacy inverse `ibd` JSON field. Preserve both forms and default
            // to IBD so a generated Testnet configuration synchronizes.
            if (args.contains("skip_ibd") && args["skip_ibd"].is_boolean()) {
                skip_ibd_val = args["skip_ibd"].get<bool>();
            } else if (args.contains("ibd") && args["ibd"].is_boolean()) {
                skip_ibd_val = !args["ibd"].get<bool>();
            }
            ffi_args.skip_ibd = &skip_ibd_val;

            // log_filter (string -> const char*)
            if (args.contains("log_filter") && args["log_filter"].is_string()) {
                log_filter_data = args["log_filter"].get<std::string>();
                ffi_args.log_filter = log_filter_data.c_str();
            } else {
                ffi_args.log_filter = nullptr;
            }

            // kms_file (string -> const char*)
            if (args.contains("kms_file") && args["kms_file"].is_string()) {
                kms_file_data = args["kms_file"].get<std::string>();
                ffi_args.kms_file = kms_file_data.c_str();
            } else {
                ffi_args.kms_file = nullptr;
            }
        }
    };
} // namespace

void LogosBlockchainModule::on_new_block_callback(const char* block) {
    LogosBlockchainModule* instance = nullptr;
    std::shared_ptr<CallbackLifetime> lifetime;
    {
        std::lock_guard<std::mutex> instance_lock(s_instanceMutex);
        instance = s_instance;
        if (!instance || !block) {
            return;
        }

        std::lock_guard<std::recursive_mutex> node_lock(instance->nodeMutex);
        if (!instance->node) {
            return;
        }
        lifetime = instance->callbackLifetime;
        std::lock_guard<std::mutex> callback_lock(lifetime->mutex);
        ++lifetime->inFlight;
    }

    struct CallbackScope {
        std::shared_ptr<CallbackLifetime> lifetime;
        const void* previous = nullptr;
        ~CallbackScope() {
            LogosBlockchainNode* deferred_node = nullptr;
            {
                std::lock_guard<std::mutex> lock(lifetime->mutex);
                deferred_node = lifetime->deferredNode;
                lifetime->deferredNode = nullptr;
            }
            if (deferred_node) {
                OperationStatus status = shutdown_node(deferred_node);
                if (!is_ok(&status)) {
                    (void)operation_status::take_message(status);
                }
            }
            {
                std::lock_guard<std::mutex> lock(lifetime->mutex);
                --lifetime->inFlight;
                lifetime->condition.notify_all();
            }
            active_callback_lifetime = previous;
        }
    } callback_scope{lifetime, active_callback_lifetime};
    active_callback_lifetime = lifetime.get();

    // The C API borrows this pointer for the duration of the callback. Copy the
    // JSON synchronously and avoid calling back into the node from this stream
    // callback, which runs on the core runtime.
    try {
        json parsed_block = json::parse(block);
        normalize_block_transaction_hashes(parsed_block);

        json event;
        event["block"] = std::move(parsed_block);
        instance->newBlock(event.dump());
    } catch (const json::parse_error& error) {
        // Keep the legacy fallback for an invalid payload, but never emit the
        // full payload to stderr: a busy block stream must not stall on logs.
        fprintf(stderr, "Failed to parse new block event JSON: %s\n", error.what());
        json event;
        event["block"] = std::string(block);
        instance->newBlock(event.dump());
    }
}

LogosBlockchainModule::LogosBlockchainModule()
    : lifecycleInstanceId(makeNodeLifecycleInstanceId()), lifecycleUpdatedAtMs(nodeLifecycleTimestampMs()) {}

LogosBlockchainModule::~LogosBlockchainModule() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        ++lifecycleGeneration;
        lifecyclePending = false;
        activeLifecycleOperationId.clear();
        activeLifecycleAction.clear();
        activeLifecycleGeneration = 0;
        worker = std::move(lifecycleWorker);
    }

    if (worker.joinable())
        worker.join();

    // A module teardown has no consumer to observe a lifecycle event. Preserve
    // the existing best-effort cleanup without emitting into a dying context.
    LogosBlockchainNode* node_to_shutdown = nullptr;
    {
        std::lock_guard<std::mutex> instance_lock(s_instanceMutex);
        std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
        s_instance = nullptr;
        node_to_shutdown = node;
        node = nullptr;
        blendProviderIdentity.clear();
        blendZkIdentity.clear();
    }
    const auto lifetime = callbackLifetime;
    const bool callback_reentrant = lifetime && lifetime.get() == active_callback_lifetime;
    if (!callback_reentrant) {
        waitForCallbacks(lifetime);
    }
    // shutdown_node consumes the handle and may wait for callback work. Do not
    // hold either lifetime lock while it runs, or a callback waiting on the
    // singleton mutex could deadlock shutdown.
    if (node_to_shutdown) {
        if (callback_reentrant) {
            std::lock_guard<std::mutex> lock(lifetime->mutex);
            lifetime->deferredNode = node_to_shutdown;
            return;
        }
        OperationStatus status = shutdown_node(node_to_shutdown);
        if (!is_ok(&status)) {
            (void)operation_status::take_message(status);
        }
    }
}

void LogosBlockchainModule::waitForCallbacks(const std::shared_ptr<CallbackLifetime>& lifetime) {
    if (!lifetime || lifetime.get() == active_callback_lifetime) {
        return;
    }
    std::unique_lock<std::mutex> lock(lifetime->mutex);
    lifetime->condition.wait(lock, [&lifetime] { return lifetime->inFlight == 0; });
}

const char* LogosBlockchainModule::lifecycleStateName(const LifecycleState state) {
    switch (state) {
    case LifecycleState::Uninitialized:
        return "uninitialized";
    case LifecycleState::Initializing:
        return "initializing";
    case LifecycleState::Stopped:
        return "stopped";
    case LifecycleState::Starting:
        return "starting";
    case LifecycleState::Running:
        return "running";
    case LifecycleState::Stopping:
        return "stopping";
    case LifecycleState::Destroying:
        return "destroying";
    }
    return "uninitialized";
}

std::vector<std::string> LogosBlockchainModule::lifecycleActions(const LifecycleState state) {
    switch (state) {
    case LifecycleState::Uninitialized:
        return {"initialize"};
    case LifecycleState::Stopped:
        return {"start"};
    case LifecycleState::Running:
        return {"stop"};
    case LifecycleState::Initializing:
    case LifecycleState::Starting:
    case LifecycleState::Stopping:
    case LifecycleState::Destroying:
        return {};
    }
    return {};
}

const char* LogosBlockchainModule::lifecycleFailureCode(const std::string& action) {
    if (action == "initialize")
        return "initialize_failed";
    if (action == "start")
        return "start_failed";
    if (action == "stop")
        return "stop_failed";
    return "lifecycle_action_failed";
}

const char* LogosBlockchainModule::lifecycleFailureMessage(const std::string& action) {
    if (action == "initialize")
        return "Bedrock initialization failed.";
    if (action == "start")
        return "Bedrock start failed.";
    if (action == "stop")
        return "Bedrock stop failed.";
    return "Bedrock lifecycle action failed.";
}

std::string LogosBlockchainModule::lifecycleSnapshotLocked() const {
    json snapshot;
    snapshot["schema"] = NODE_LIFECYCLE_SNAPSHOT_SCHEMA;
    snapshot["version"] = 1;
    snapshot["instance_id"] = lifecycleInstanceId;
    snapshot["epoch"] = lifecycleEpoch;
    snapshot["sequence"] = lifecycleSequence;
    snapshot["scope"] = {{"kind", "bedrock"}};
    snapshot["state"] = lifecycleStateName(lifecycleState);
    snapshot["health"] = !lifecycleError.empty()                     ? "degraded"
                         : lifecycleState == LifecycleState::Running ? "healthy"
                                                                     : "unknown";
    snapshot["supported_actions"] = lifecycleActions(lifecycleState);
    if (lifecyclePending) {
        snapshot["pending_operation"] = {
            {"operation_id", activeLifecycleOperationId.empty() ? json(nullptr) : json(activeLifecycleOperationId)},
            {"action", activeLifecycleAction},
        };
    } else {
        snapshot["pending_operation"] = nullptr;
    }

    const auto completed = lifecycleOperations.find(lastCompletedLifecycleOperationId);
    if (completed != lifecycleOperations.end() && completed->second.settled) {
        snapshot["last_completed_operation"] = {
            {"operation_id", lastCompletedLifecycleOperationId},
            {"action", completed->second.action},
            {"outcome", completed->second.outcome},
        };
    } else {
        snapshot["last_completed_operation"] = nullptr;
    }
    snapshot["last_error"] = lifecycleError.empty()
                                 ? json(nullptr)
                                 : nodeLifecycleError(lifecycleErrorCode, lifecycleError, lifecycleErrorAtMs);
    snapshot["updated_at_ms"] = lifecycleUpdatedAtMs;
    return snapshot.dump();
}

std::string LogosBlockchainModule::lifecycleEventLocked(
    const std::string& action,
    const std::string& operation_id,
    const std::string& phase,
    const std::string& outcome,
    const LifecycleState previous_state,
    const std::string& error_code
) const {
    json event;
    event["schema"] = NODE_LIFECYCLE_EVENT_SCHEMA;
    event["version"] = 1;
    event["instance_id"] = lifecycleInstanceId;
    event["epoch"] = lifecycleEpoch;
    event["sequence"] = lifecycleSequence;
    event["scope"] = {{"kind", "bedrock"}};
    event["operation_id"] = operation_id.empty() ? json(nullptr) : json(operation_id);
    event["action"] = action;
    event["phase"] = phase;
    event["outcome"] = outcome;
    event["previous_state"] = lifecycleStateName(previous_state);
    event["status"] = json::parse(lifecycleSnapshotLocked());
    event["error"] = error_code.empty()
                         ? json(nullptr)
                         : nodeLifecycleError(error_code, lifecycleFailureMessage(action), nodeLifecycleTimestampMs());
    event["emitted_at_ms"] = nodeLifecycleTimestampMs();
    return event.dump();
}

void LogosBlockchainModule::emitLifecycleEvents(const std::vector<std::string>& events) {
    for (const std::string& event : events) {
        nodeChanged(event);
    }
}

void LogosBlockchainModule::joinSettledLifecycleWorker() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (lifecyclePending)
            return;
        worker = std::move(lifecycleWorker);
    }
    if (worker.joinable())
        worker.join();
}

void LogosBlockchainModule::rememberCompletedLifecycleOperationLocked(const std::string& operation_id) {
    if (operation_id.empty())
        return;
    lastCompletedLifecycleOperationId = operation_id;
    completedLifecycleOperationIds.push_back(operation_id);
    while (completedLifecycleOperationIds.size() > MAX_COMPLETED_NODE_LIFECYCLE_OPERATIONS) {
        const std::string expired = completedLifecycleOperationIds.front();
        completedLifecycleOperationIds.pop_front();
        const auto found = lifecycleOperations.find(expired);
        if (found != lifecycleOperations.end() && found->second.settled &&
            expired != lastCompletedLifecycleOperationId) {
            lifecycleOperations.erase(found);
        }
    }
}

std::string LogosBlockchainModule::lifecycleInitializationConfigPath(const std::string& config) const {
    try {
        const json parsed = json::parse(config);
        if (!parsed.is_object())
            return {};
        const auto use_persistence_paths = parsed.find("use_persistence_paths");
        const bool use_persistence = use_persistence_paths != parsed.end() && use_persistence_paths->is_boolean() &&
                                     use_persistence_paths->get<bool>();
        return resolvedConfigOutputPath(parsed, use_persistence, instancePersistencePath());
    } catch (const json::exception&) {
        return {};
    }
}

std::string LogosBlockchainModule::restoredLifecycleConfigPath() const {
    const std::string& persistence_path = instancePersistencePath();
    if (persistence_path.empty())
        return {};

    const fs::path state_path = fs::path(persistence_path) / PERSISTED_LIFECYCLE_STATE_FILE;
    std::error_code error;
    if (!fs::is_regular_file(state_path, error) || error)
        return {};
    error.clear();
    const auto size = fs::file_size(state_path, error);
    if (error || size > MAX_PERSISTED_LIFECYCLE_STATE_BYTES)
        return {};

    std::ifstream input(state_path, std::ios::binary);
    if (!input)
        return {};
    const std::string serialized((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (serialized.empty() || serialized.size() > MAX_PERSISTED_LIFECYCLE_STATE_BYTES)
        return {};

    try {
        const json state = json::parse(serialized);
        if (!state.is_object() || state.size() != 3)
            return {};
        const auto schema = state.find("schema");
        const auto version = state.find("version");
        const auto config_path = state.find("config_path");
        if (schema == state.end() || !schema->is_string() ||
            schema->get<std::string>() != PERSISTED_LIFECYCLE_STATE_SCHEMA || version == state.end() ||
            !isLifecycleVersionOne(*version) || config_path == state.end() || !config_path->is_string()) {
            return {};
        }
        return existingRegularFilePath(config_path->get<std::string>());
    } catch (const json::exception&) {
        return {};
    }
}

void LogosBlockchainModule::persistLifecycleConfigLocked() {
    const std::string& persistence_path = instancePersistencePath();
    const std::string config_path = existingRegularFilePath(lifecycleConfigPath);
    if (persistence_path.empty() || config_path.empty())
        return;

    const fs::path base(persistence_path);
    std::error_code error;
    fs::create_directories(base, error);
    if (error)
        return;

    const fs::path state_path = base / PERSISTED_LIFECYCLE_STATE_FILE;
    const fs::path temporary_path = base / (std::string(PERSISTED_LIFECYCLE_STATE_FILE) + ".tmp");
    // Keep the config reference private to the module persistence directory;
    // lifecycle snapshots and events intentionally never expose it.
    const json state = {
        {"schema", PERSISTED_LIFECYCLE_STATE_SCHEMA},
        {"version", 1},
        {"config_path", config_path},
    };
    const std::string serialized = state.dump();
    if (serialized.size() > MAX_PERSISTED_LIFECYCLE_STATE_BYTES)
        return;
    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output)
            return;
        output << serialized << '\n';
        output.close();
        if (!output) {
            fs::remove(temporary_path, error);
            return;
        }
    }

    error.clear();
    fs::rename(temporary_path, state_path, error);
    if (error) {
        fs::remove(temporary_path, error);
    }
}

void LogosBlockchainModule::onContextReady() {
    std::unique_lock<std::recursive_mutex> node_lock(nodeMutex);
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    if (lifecycleState != LifecycleState::Uninitialized || lifecyclePending || node || !lifecycleConfigPath.empty())
        return;

    const std::string config_path = restoredLifecycleConfigPath();
    if (config_path.empty())
        return;

    lifecycleConfigPath = config_path;
    lifecycleState = LifecycleState::Stopped;
    lifecycleEpoch = std::max<std::uint64_t>(lifecycleEpoch, 1);
    ++lifecycleSequence;
    lifecycleUpdatedAtMs = nodeLifecycleTimestampMs();
    lifecycleError.clear();
    lifecycleErrorCode.clear();
    lifecycleErrorAtMs = 0;
}

LogosBlockchainModule::LifecycleDispatch LogosBlockchainModule::beginLifecycleAction(
    const std::string& action,
    const std::string& operation_id,
    const std::string& request_fingerprint,
    const bool has_expected_snapshot,
    const std::string& expected_instance_id,
    const std::uint64_t expected_epoch,
    const std::uint64_t expected_sequence,
    const bool strict_action
) {
    LifecycleDispatch dispatch;
    dispatch.action = action;
    dispatch.operationId = operation_id;

    std::lock_guard<std::mutex> lock(lifecycleMutex);
    const auto acknowledgement = [&](const bool accepted,
                                     const bool duplicate,
                                     const std::string& error_code,
                                     const std::string& error_message) {
        json result;
        result["schema"] = NODE_LIFECYCLE_ACK_SCHEMA;
        result["version"] = 1;
        result["operation_id"] = operation_id.empty() ? json(nullptr) : json(operation_id);
        result["accepted"] = accepted;
        result["duplicate"] = duplicate;
        result["instance_id"] = lifecycleInstanceId;
        result["epoch"] = lifecycleEpoch;
        result["sequence"] = lifecycleSequence;
        result["state"] = lifecycleStateName(lifecycleState);
        result["error"] = error_code.empty()
                              ? json(nullptr)
                              : nodeLifecycleError(error_code, error_message, nodeLifecycleTimestampMs());
        return result.dump();
    };
    const auto settle_without_dispatch = [&](const LifecycleDispatchDisposition disposition,
                                             const bool accepted,
                                             const std::string& outcome,
                                             const std::string& error_code,
                                             const std::string& error_message) {
        LifecycleOperation operation;
        operation.action = action;
        operation.requestFingerprint = request_fingerprint;
        operation.previousState = lifecycleState;
        operation.settled = true;
        operation.outcome = outcome;
        const auto inserted = lifecycleOperations.emplace(operation_id, std::move(operation));
        if (accepted) {
            ++lifecycleSequence;
            lifecycleUpdatedAtMs = nodeLifecycleTimestampMs();
            dispatch.events.push_back(
                lifecycleEventLocked(action, operation_id, "accepted", "accepted", lifecycleState)
            );
        }
        ++lifecycleSequence;
        lifecycleUpdatedAtMs = nodeLifecycleTimestampMs();
        dispatch.disposition = disposition;
        dispatch.events.push_back(lifecycleEventLocked(
            action, operation_id, "settled", outcome, lifecycleState, accepted ? std::string() : error_code
        ));
        dispatch.acknowledgement = acknowledgement(
            accepted, false, accepted ? std::string() : error_code, accepted ? std::string() : error_message
        );
        inserted.first->second.acknowledgement = dispatch.acknowledgement;
        rememberCompletedLifecycleOperationLocked(operation_id);
    };

    if (strict_action) {
        const auto existing = lifecycleOperations.find(operation_id);
        if (existing != lifecycleOperations.end()) {
            if (existing->second.requestFingerprint != request_fingerprint) {
                dispatch.disposition = LifecycleDispatchDisposition::Rejected;
                dispatch.acknowledgement = acknowledgement(
                    false, false, "operation_id_conflict", "operation_id was already used for a different request."
                );
                return dispatch;
            }
            dispatch.disposition = LifecycleDispatchDisposition::Duplicate;
            json duplicate = json::parse(existing->second.acknowledgement);
            duplicate["duplicate"] = true;
            dispatch.acknowledgement = duplicate.dump();
            return dispatch;
        }
    }
    if (lifecyclePending) {
        if (strict_action) {
            settle_without_dispatch(
                LifecycleDispatchDisposition::Rejected,
                false,
                "rejected",
                "operation_in_progress",
                "A lifecycle operation is already in progress."
            );
        }
        return dispatch;
    }
    if (strict_action && has_expected_snapshot &&
        (expected_instance_id != lifecycleInstanceId || expected_epoch != lifecycleEpoch ||
         expected_sequence != lifecycleSequence)) {
        settle_without_dispatch(
            LifecycleDispatchDisposition::Rejected,
            false,
            "rejected",
            "state_mismatch",
            "The lifecycle snapshot is stale."
        );
        return dispatch;
    }
    if (strict_action) {
        if (action == "initialize") {
            if (lifecycleState != LifecycleState::Uninitialized) {
                settle_without_dispatch(
                    LifecycleDispatchDisposition::Rejected,
                    false,
                    "rejected",
                    "invalid_state",
                    "Bedrock is already initialized."
                );
                return dispatch;
            }
        } else if (action == "start") {
            if (lifecycleState == LifecycleState::Running) {
                settle_without_dispatch(LifecycleDispatchDisposition::Noop, true, "no_op", {}, {});
                return dispatch;
            }
            if (lifecycleState != LifecycleState::Stopped || lifecycleConfigPath.empty()) {
                settle_without_dispatch(
                    LifecycleDispatchDisposition::Rejected,
                    false,
                    "rejected",
                    "invalid_state",
                    "Bedrock must be initialized before it can start."
                );
                return dispatch;
            }
        } else if (action == "stop") {
            if (lifecycleState == LifecycleState::Stopped) {
                settle_without_dispatch(LifecycleDispatchDisposition::Noop, true, "no_op", {}, {});
                return dispatch;
            }
            if (lifecycleState != LifecycleState::Running) {
                settle_without_dispatch(
                    LifecycleDispatchDisposition::Rejected,
                    false,
                    "rejected",
                    "invalid_state",
                    "Bedrock is not in a stoppable state."
                );
                return dispatch;
            }
        }
    }

    dispatch.previousState = lifecycleState;
    dispatch.generation = ++lifecycleGeneration;
    lifecycleState = action == "initialize" ? LifecycleState::Initializing
                     : action == "start"    ? LifecycleState::Starting
                                            : LifecycleState::Stopping;
    lifecycleError.clear();
    lifecycleErrorCode.clear();
    lifecycleErrorAtMs = 0;
    lifecyclePending = true;
    activeLifecycleOperationId = strict_action ? operation_id : std::string();
    activeLifecycleAction = action;
    activeLifecycleGeneration = dispatch.generation;
    ++lifecycleSequence;
    lifecycleUpdatedAtMs = nodeLifecycleTimestampMs();
    dispatch.disposition = LifecycleDispatchDisposition::Dispatch;
    if (strict_action) {
        LifecycleOperation operation;
        operation.action = action;
        operation.requestFingerprint = request_fingerprint;
        operation.previousState = dispatch.previousState;
        const auto inserted = lifecycleOperations.emplace(operation_id, std::move(operation));
        dispatch.events.push_back(
            lifecycleEventLocked(action, operation_id, "accepted", "accepted", dispatch.previousState)
        );
        dispatch.acknowledgement = acknowledgement(true, false, {}, {});
        inserted.first->second.acknowledgement = dispatch.acknowledgement;
    } else {
        dispatch.events.push_back(lifecycleEventLocked(action, {}, "accepted", "accepted", dispatch.previousState));
    }
    return dispatch;
}

void LogosBlockchainModule::settleLifecycleAction(
    const LifecycleDispatch& dispatch,
    const bool success,
    const LifecycleState success_state,
    const LifecycleState failure_state,
    const std::string& error_code
) {
    std::string event;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (!lifecyclePending || activeLifecycleGeneration != dispatch.generation ||
            activeLifecycleAction != dispatch.action) {
            return;
        }
        lifecycleState = success ? success_state : failure_state;
        lifecycleErrorCode = success              ? std::string()
                             : error_code.empty() ? lifecycleFailureCode(dispatch.action)
                                                  : error_code;
        lifecycleError = success ? std::string() : lifecycleFailureMessage(dispatch.action);
        lifecycleErrorAtMs = success ? 0 : nodeLifecycleTimestampMs();
        if (success && dispatch.action == "initialize") {
            ++lifecycleEpoch;
        }
        if (!dispatch.operationId.empty()) {
            const auto operation = lifecycleOperations.find(dispatch.operationId);
            if (operation != lifecycleOperations.end()) {
                operation->second.settled = true;
                operation->second.outcome = success ? "succeeded" : "failed";
                rememberCompletedLifecycleOperationLocked(dispatch.operationId);
            }
        }
        lifecyclePending = false;
        activeLifecycleOperationId.clear();
        activeLifecycleAction.clear();
        activeLifecycleGeneration = 0;
        ++lifecycleSequence;
        lifecycleUpdatedAtMs = nodeLifecycleTimestampMs();
        event = lifecycleEventLocked(
            dispatch.action,
            dispatch.operationId,
            "settled",
            success ? "succeeded" : "failed",
            dispatch.previousState,
            success ? std::string() : lifecycleErrorCode
        );
    }
    nodeChanged(event);
}

StdLogosResult LogosBlockchainModule::startPrepared(const std::string& config_path, const std::string& deployment) {
    std::unique_lock<std::recursive_mutex> node_lock(nodeMutex);
    if (node) {
        return result::err("The node is already running.");
    }
    {
        std::lock_guard<std::mutex> lock(callbackLifetime->mutex);
        if (callbackLifetime->deferredNode) {
            return result::err("A callback shutdown is still in progress.");
        }
    }

    std::string effective_config_path = config_path;
    if (effective_config_path.empty()) {
        const char* env = std::getenv("LB_CONFIG_PATH");
        if (env && *env) {
            effective_config_path = env;
        } else {
            return result::err("Config path was not specified and LB_CONFIG_PATH is not set.");
        }
    }

    effective_config_path = localPathFromFileUrl(effective_config_path);
    const std::string deployment_path = localPathFromFileUrl(deployment);
    const char* config_path_ptr = effective_config_path.empty() ? nullptr : effective_config_path.c_str();
    const char* deployment_ptr = deployment_path.empty() ? nullptr : deployment_path.c_str();
    // Capture explicit identities before starting the node. The snapshot
    // remains authoritative if the configuration file changes later. KMS-only
    // configurations remain empty and are rejected by the join path because
    // this C ABI does not expose the resolved public identities.
    const std::vector<uint8_t> startup_blend_provider_identity = parse_address_hex(
        configured_blend_public_key(effective_config_path, "BlendSigning")
    );
    const std::vector<uint8_t> startup_blend_zk_identity = parse_address_hex(
        configured_blend_public_key(effective_config_path, "BlendZk")
    );

    auto [value, error] = start_lb_node(config_path_ptr, deployment_ptr);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }
    if (!value) {
        return result::err("Could not subscribe to block events: the node is not running.");
    }

    node = value;
    node_lock.unlock();
    {
        std::lock_guard<std::mutex> instance_lock(s_instanceMutex);
        s_instance = this;
    }
    node_lock.lock();
    OperationStatus subscribe_status = subscribe_to_new_blocks(node, on_new_block_callback);
    if (is_ok(&subscribe_status)) {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        lifecycleConfigPath = effective_config_path;
        blendProviderIdentity = startup_blend_provider_identity;
        blendZkIdentity = startup_blend_zk_identity;
        persistLifecycleConfigLocked();
        return result::ok();
    }

    const std::string message = operation_status::take_message(subscribe_status);
    node_lock.unlock();
    LogosBlockchainNode* node_to_shutdown = nullptr;
    {
        std::lock_guard<std::mutex> instance_lock(s_instanceMutex);
        std::lock_guard<std::recursive_mutex> node_guard(nodeMutex);
        s_instance = nullptr;
        // shutdown_node consumes the node handle even when shutdown reports
        // an error. Clear both aliases before dispatch so a later start or
        // teardown cannot reuse the consumed pointer.
        node_to_shutdown = node;
        node = nullptr;
        blendProviderIdentity.clear();
        blendZkIdentity.clear();
    }
    OperationStatus stop_status = shutdown_node(node_to_shutdown);
    if (!is_ok(&stop_status)) {
        (void)operation_status::take_message(stop_status);
    }
    return result::err(
        message.empty() ? "Could not subscribe to block events." : "Could not subscribe to block events: " + message
    );
}

StdLogosResult LogosBlockchainModule::stopPrepared(bool* shutdown_attempted) {
    if (shutdown_attempted) {
        *shutdown_attempted = false;
    }
    LogosBlockchainNode* node_to_shutdown = nullptr;
    {
        std::lock_guard<std::mutex> instance_lock(s_instanceMutex);
        std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
        if (!node) {
            return result::err("The node is not running.");
        }

        // shutdown_node consumes the node handle even when shutdown reports
        // an error. Clear both aliases before dispatch so no later operation
        // can use the consumed pointer or publish block events into this
        // module instance.
        node_to_shutdown = node;
        node = nullptr;
        s_instance = nullptr;
        blendProviderIdentity.clear();
        blendZkIdentity.clear();
    }

    const auto lifetime = callbackLifetime;
    const bool callback_reentrant = lifetime && lifetime.get() == active_callback_lifetime;
    if (!callback_reentrant) {
        waitForCallbacks(lifetime);
    }
    if (shutdown_attempted) {
        *shutdown_attempted = true;
    }
    if (callback_reentrant) {
        std::lock_guard<std::mutex> lock(lifetime->mutex);
        lifetime->deferredNode = node_to_shutdown;
        return result::ok();
    }
    OperationStatus status = shutdown_node(node_to_shutdown);
    if (!is_ok(&status)) {
        return result::err(operation_status::take_message(status));
    }
    return result::ok();
}

// ---- Node ----

StdLogosResult LogosBlockchainModule::start(const std::string& config_path, const std::string& deployment) {
    const LifecycleDispatch dispatch = beginLifecycleAction("start", {}, {}, false, {}, 0, 0, false);
    if (dispatch.disposition != LifecycleDispatchDisposition::Dispatch) {
        return result::err("A lifecycle operation is already in progress.");
    }
    emitLifecycleEvents(dispatch.events);
    const StdLogosResult started = startPrepared(config_path, deployment);
    settleLifecycleAction(dispatch, started.success, LifecycleState::Running, dispatch.previousState);
    return started;
}

StdLogosResult LogosBlockchainModule::stop() {
    const LifecycleDispatch dispatch = beginLifecycleAction("stop", {}, {}, false, {}, 0, 0, false);
    if (dispatch.disposition != LifecycleDispatchDisposition::Dispatch) {
        return result::err("A lifecycle operation is already in progress.");
    }
    emitLifecycleEvents(dispatch.events);
    bool shutdown_attempted = false;
    const StdLogosResult stopped = stopPrepared(&shutdown_attempted);
    // The consumed handle cannot be retried after a shutdown error. Report a
    // terminal stopped state only after shutdown consumed a handle. If no
    // handle existed, preserve the state that preceded this legacy request.
    settleLifecycleAction(
        dispatch,
        stopped.success,
        LifecycleState::Stopped,
        shutdown_attempted ? LifecycleState::Stopped : dispatch.previousState
    );
    return stopped;
}

std::string LogosBlockchainModule::nodeStatus() {
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    return lifecycleSnapshotLocked();
}

void LogosBlockchainModule::dispatchLifecycleAction(
    const LifecycleDispatch& dispatch,
    const std::string& initialization_config,
    const std::string& deployment
) {
    if (dispatch.action == "initialize") {
        const std::string existing_config_path =
            existingRegularFilePath(lifecycleInitializationConfigPath(initialization_config));
        StdLogosResult initialized = existing_config_path.empty() ? generate_user_config(initialization_config)
                                                                  : result::ok(existing_config_path);
        std::string generated_config_path;
        if (initialized.success) {
            generated_config_path = initialized.value.get<std::string>();
            if (generated_config_path.empty())
                initialized = result::err("Generated configuration path is unavailable.");
        }
        if (initialized.success) {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            if (activeLifecycleGeneration == dispatch.generation) {
                lifecycleConfigPath = generated_config_path;
                persistLifecycleConfigLocked();
            }
        }
        settleLifecycleAction(dispatch, initialized.success, LifecycleState::Stopped, LifecycleState::Uninitialized);
        return;
    }

    if (dispatch.action == "start") {
        std::string config_path;
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            config_path = lifecycleConfigPath;
        }
        const StdLogosResult started = startPrepared(config_path, deployment);
        settleLifecycleAction(dispatch, started.success, LifecycleState::Running, LifecycleState::Stopped);
        return;
    }

    bool shutdown_attempted = false;
    const StdLogosResult stopped = stopPrepared(&shutdown_attempted);
    // The consumed handle cannot be retried after a shutdown error. Report a
    // terminal stopped state only after shutdown consumed a handle. A stale
    // lifecycle snapshot with no live handle remains in its previous state.
    settleLifecycleAction(
        dispatch,
        stopped.success,
        LifecycleState::Stopped,
        shutdown_attempted ? LifecycleState::Stopped : dispatch.previousState
    );
}

std::string LogosBlockchainModule::nodeAction(const std::string& request) {
    const auto rejected = [this](const std::string& code, const std::string& message) {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        json acknowledgement;
        acknowledgement["schema"] = NODE_LIFECYCLE_ACK_SCHEMA;
        acknowledgement["version"] = 1;
        acknowledgement["operation_id"] = nullptr;
        acknowledgement["accepted"] = false;
        acknowledgement["duplicate"] = false;
        acknowledgement["instance_id"] = lifecycleInstanceId;
        acknowledgement["epoch"] = lifecycleEpoch;
        acknowledgement["sequence"] = lifecycleSequence;
        acknowledgement["state"] = lifecycleStateName(lifecycleState);
        acknowledgement["error"] = nodeLifecycleError(code, message, nodeLifecycleTimestampMs());
        return acknowledgement.dump();
    };

    if (request.size() > MAX_NODE_LIFECYCLE_REQUEST_BYTES) {
        return rejected("request_too_large", "Lifecycle request exceeds the supported size.");
    }
    json input;
    try {
        input = json::parse(request);
    } catch (const json::exception&) {
        return rejected("invalid_request", "Lifecycle request must be a JSON object.");
    }
    if (!input.is_object()) {
        return rejected("invalid_request", "Lifecycle request must be a JSON object.");
    }
    for (const auto& item : input.items()) {
        const std::string& key = item.key();
        if (key != "schema" && key != "version" && key != "operation_id" && key != "action" && key != "expected" &&
            key != "parameters") {
            return rejected("invalid_request", "Lifecycle request contains an unsupported field.");
        }
    }
    const auto schema = input.find("schema");
    if (schema == input.end() || !schema->is_string() || schema->get<std::string>() != NODE_LIFECYCLE_COMMAND_SCHEMA) {
        return rejected("invalid_request", "Unsupported lifecycle request schema.");
    }
    const auto version = input.find("version");
    if (version == input.end() || !isLifecycleVersionOne(*version)) {
        return rejected("invalid_request", "Unsupported lifecycle request version.");
    }
    const auto operation_id = input.find("operation_id");
    if (operation_id == input.end() || !operation_id->is_string()) {
        return rejected("invalid_request", "Lifecycle request requires an operation_id.");
    }
    const std::string operation = operation_id->get<std::string>();
    if (!isValidNodeLifecycleOperationId(operation)) {
        return rejected("invalid_request", "Lifecycle operation_id is invalid.");
    }
    const auto action_value = input.find("action");
    if (action_value == input.end() || !action_value->is_string()) {
        return rejected("invalid_request", "Lifecycle request requires an action.");
    }
    const std::string action = action_value->get<std::string>();
    if (action != "initialize" && action != "start" && action != "stop") {
        return rejected("invalid_request", "Unsupported lifecycle action.");
    }

    bool has_expected_snapshot = false;
    std::string expected_instance_id;
    std::uint64_t expected_epoch = 0;
    std::uint64_t expected_sequence = 0;
    const auto expected = input.find("expected");
    if (expected != input.end()) {
        if (!expected->is_object() || expected->size() != 3 || !expected->contains("instance_id") ||
            !expected->contains("epoch") || !expected->contains("sequence") ||
            !expected->at("instance_id").is_string() ||
            !parseLifecycleUnsigned(expected->at("epoch"), expected_epoch) ||
            !parseLifecycleUnsigned(expected->at("sequence"), expected_sequence)) {
            return rejected(
                "invalid_request", "Lifecycle expected snapshot must contain instance_id, epoch, and sequence."
            );
        }
        expected_instance_id = expected->at("instance_id").get<std::string>();
        has_expected_snapshot = true;
    }

    json parameters = json::object();
    const auto parameters_value = input.find("parameters");
    if (parameters_value != input.end()) {
        if (!parameters_value->is_object()) {
            return rejected("invalid_request", "Lifecycle parameters must be an object.");
        }
        parameters = *parameters_value;
    }

    std::string initialization_config;
    std::string deployment;
    if (action == "initialize") {
        const auto config = parameters.find("config");
        if (config == parameters.end() || !config->is_string() || parameters.size() != 1) {
            return rejected("invalid_request", "Initialize requires only parameters.config.");
        }
        initialization_config = config->get<std::string>();
        if (initialization_config.empty() || initialization_config.size() > MAX_NODE_LIFECYCLE_CONFIG_BYTES ||
            containsEmbeddedNul(initialization_config)) {
            return rejected("invalid_request", "Initialize config is invalid or exceeds the supported size.");
        }
        try {
            const json config_object = json::parse(initialization_config);
            if (!config_object.is_object()) {
                return rejected("invalid_request", "Initialize config must be a JSON object.");
            }
            const auto output = config_object.find("output");
            if (output == config_object.end() || !output->is_string() || output->get<std::string>().empty()) {
                return rejected("invalid_request", "Initialize config requires a non-empty output path.");
            }
        } catch (const json::exception&) {
            return rejected("invalid_request", "Initialize config must be a JSON object.");
        }
    } else if (action == "start") {
        if (parameters.empty()) {
            deployment.clear();
        } else if (parameters.size() == 1 && parameters.contains("deployment") &&
                   parameters.at("deployment").is_string()) {
            deployment = parameters.at("deployment").get<std::string>();
            if (deployment.size() > MAX_NODE_LIFECYCLE_DEPLOYMENT_BYTES || containsEmbeddedNul(deployment)) {
                return rejected("invalid_request", "Start deployment is invalid or exceeds the supported size.");
            }
        } else {
            return rejected("invalid_request", "Start accepts only optional parameters.deployment.");
        }
    } else if (!parameters.empty()) {
        return rejected("invalid_request", "Stop does not accept parameters.");
    }

    joinSettledLifecycleWorker();
    const LifecycleDispatch dispatch = beginLifecycleAction(
        action,
        operation,
        input.dump(),
        has_expected_snapshot,
        expected_instance_id,
        expected_epoch,
        expected_sequence,
        true
    );
    emitLifecycleEvents(dispatch.events);
    if (dispatch.disposition != LifecycleDispatchDisposition::Dispatch) {
        return dispatch.acknowledgement;
    }

    const auto settle_worker_failure = [this, dispatch, action] {
        settleLifecycleAction(
            dispatch,
            false,
            action == "initialize" ? LifecycleState::Uninitialized : LifecycleState::Stopped,
            action == "stop" ? LifecycleState::Running : LifecycleState::Uninitialized,
            "lifecycle_worker_failed"
        );
    };
    try {
        std::thread worker(
            [this,
             dispatch,
             initialization_config = std::move(initialization_config),
             deployment = std::move(deployment),
             settle_worker_failure] {
                try {
                    dispatchLifecycleAction(dispatch, initialization_config, deployment);
                } catch (const std::exception&) {
                    settle_worker_failure();
                } catch (...) {
                    settle_worker_failure();
                }
            }
        );
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        lifecycleWorker = std::move(worker);
    } catch (const std::exception&) {
        settle_worker_failure();
    }
    return dispatch.acknowledgement;
}

StdLogosResult LogosBlockchainModule::generate_user_config(const std::string& json_args) const {
    json parsed_args;
    try {
        parsed_args = json::parse(json_args);
    } catch (const json::parse_error& e) {
        fprintf(stderr, "Failed to parse JSON args: %s\n", e.what());
        return result::err(std::string("Failed to parse JSON args: ") + e.what());
    }

    // The module-context getters are populated by every logos-core host
    // (logoscore-cli and Basecamp alike), so their mere presence can't tell the
    // two apart. The bundled app therefore opts in explicitly by passing
    // "use_persistence_paths": true; only then do we route the node's runtime
    // directories — state, storage (db) and logs — under the host-owned
    // per-instance persistence dir, so they all share one writable base. CLI and
    // standalone callers omit the flag and keep their own paths (or the node
    // defaults). Any path the caller set explicitly is left untouched.
    bool use_persistence_paths = false;
    if (const auto it = parsed_args.find("use_persistence_paths"); it != parsed_args.end() && it->is_boolean()) {
        use_persistence_paths = it->get<bool>();
    }
    parsed_args.erase("use_persistence_paths"); // not an FFI field

    if (use_persistence_paths) {
        const std::string& persistence = instancePersistencePath();
        if (!persistence.empty()) {
            const fs::path base(persistence);
            // Only fill a path the caller didn't pin (non-empty string wins).
            const auto set_if_absent = [&parsed_args](const char* key, const std::string& value) {
                const bool provided = parsed_args.contains(key) && parsed_args[key].is_string() &&
                                      !parsed_args[key].get<std::string>().empty();
                if (!provided)
                    parsed_args[key] = value;
            };
            set_if_absent("state_path", (base / "state").string());
            set_if_absent("storage_path", (base / "db").string());
            set_if_absent("logs_path", (base / "logs").string());

            // The config file itself is written under the same base, using the
            // caller's path as the relative part below it. Keep this resolution
            // shared with lifecycle recovery so an existing Basecamp config is
            // never regenerated merely because the host was recreated.
            parsed_args["output"] = resolvedConfigOutputPath(parsed_args, true, persistence);

            const fs::path output_parent = fs::path(parsed_args["output"].get<std::string>()).parent_path();
            std::error_code create_directories_error;
            fs::create_directories(output_parent, create_directories_error);
            if (create_directories_error) {
                return result::err(
                    "Failed to create configuration directory '" + output_parent.string() +
                    "': " + create_directories_error.message()
                );
            }

            fprintf(
                stderr,
                "generate_user_config: routing output/state/storage/logs under instance persistence path: %s\n",
                persistence.c_str()
            );
        } else {
            fprintf(
                stderr,
                "generate_user_config: use_persistence_paths requested but no instance persistence path is set; "
                "leaving paths unchanged.\n"
            );
        }
    }

    // The path the config is actually written to (after any persistence routing).
    // Returned to the caller so it can hand the exact path to start(). Empty only
    // when no output was given and no routing applied (the node wrote its own
    // default relative to the cwd, which the module can't resolve).
    std::string resolved_output;
    if (parsed_args.contains("output") && parsed_args["output"].is_string())
        resolved_output = parsed_args["output"].get<std::string>();

    const OwnedGenerateConfigArgs owned_args(parsed_args);

    OperationStatus status = ::generate_user_config(owned_args.ffi_args);
    if (!is_ok(&status)) {
        return result::err(operation_status::take_message(status));
    }

    return result::ok(resolved_output);
}

// Config management

StdLogosResult LogosBlockchainModule::update_user_config(
    const std::string& user_config_path,
    const std::string& keystore_path
) {
    const std::string config = localPathFromFileUrl(user_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);

    OperationStatus status = ::update_user_config(config.c_str(), keystore.c_str());
    return result::from_operation_status(status);
}

StdLogosResult LogosBlockchainModule::migrate_user_config(
    const std::string& output_path,
    const std::string& keystore_path
) {
    const std::string output = localPathFromFileUrl(output_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);

    OperationStatus status = ::migrate_user_config(output.c_str(), keystore.c_str());
    return result::from_operation_status(status);
}

StdLogosResult LogosBlockchainModule::migrate_user_config_0_1_2(
    const std::string& new_config_path,
    const std::string& old_config_path,
    const std::string& keystore_path
) {
    const std::string new_config = localPathFromFileUrl(new_config_path);
    const std::string old_config = localPathFromFileUrl(old_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);

    OperationStatus status = ::migrate_user_config_0_1_2(new_config.c_str(), old_config.c_str(), keystore.c_str());
    return result::from_operation_status(status);
}

StdLogosResult LogosBlockchainModule::participate(
    const std::string& config_path,
    const std::string& keystore_path,
    const std::string& output_dir,
    const std::string& external_address
) {
    const std::string config = localPathFromFileUrl(config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);
    const std::string output = localPathFromFileUrl(output_dir);
    const char* external_address_ptr = external_address.empty() ? nullptr : external_address.c_str();

    OperationStatus status = ::participate(config.c_str(), keystore.c_str(), output.c_str(), external_address_ptr);
    return result::from_operation_status(status);
}

// Keystore

StdLogosResult LogosBlockchainModule::generate_key(
    const std::string& user_config_path,
    const std::string& keystore_path,
    const std::string& key_type,
    const std::string& key_title
) {
    KeyType type{};
    if (!parse_key_type(key_type, type)) {
        return result::err(R"(Invalid key_type (expected "ed25519" or "zk").)");
    }

    const std::string config = localPathFromFileUrl(user_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);
    const char* key_title_ptr = key_title.empty() ? nullptr : key_title.c_str();

    auto [value, error] = ::generate_key(config.c_str(), keystore.c_str(), type, key_title_ptr);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    const std::string out(value);
    OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free key id string: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(out);
}

StdLogosResult LogosBlockchainModule::add_key(
    const std::string& user_config_path,
    const std::string& keystore_path,
    const std::string& key_type,
    const std::string& key_hex,
    const std::string& key_title
) {
    KeyType type{};
    if (!parse_key_type(key_type, type)) {
        fprintf(stderr, "Invalid key_type (expected \"ed25519\" or \"zk\").\n");
        return result::err(R"(Invalid key_type (expected "ed25519" or "zk").)");
    }

    const std::string config = localPathFromFileUrl(user_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);
    const char* key_title_ptr = key_title.empty() ? nullptr : key_title.c_str();

    OperationStatus status = ::add_key(config.c_str(), keystore.c_str(), type, key_hex.c_str(), key_title_ptr);
    return result::from_operation_status(status);
}

StdLogosResult LogosBlockchainModule::remove_key(
    const std::string& user_config_path,
    const std::string& keystore_path,
    const std::string& key_title
) {
    const std::string config = localPathFromFileUrl(user_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);

    OperationStatus status = ::remove_key(config.c_str(), keystore.c_str(), key_title.c_str());
    return result::from_operation_status(status);
}

// Identity

StdLogosResult LogosBlockchainModule::get_peer_id(const std::string& config_path) {
    const std::string config = localPathFromFileUrl(config_path);

    auto [value, error] = ::get_peer_id(config.c_str());
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    const std::string out(value);
    OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free peer id string: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(out);
}

// Wallet

StdLogosResult LogosBlockchainModule::wallet_get_balance(const std::string& address_hex) const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    fprintf(stderr, "wallet_get_balance: address_hex=%s\n", address_hex.c_str());
    if (!node) {
        return result::err("The node is not running.");
    }

    const std::vector<uint8_t> bytes = parse_address_hex(address_hex);
    if (bytes.empty() || static_cast<int>(bytes.size()) != ADDRESS_BYTES) {
        return result::err("Address must be 64 hex characters (32 bytes).");
    }

    auto [value, error] = get_balance(node, bytes.data(), nullptr);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    return result::ok(std::to_string(value));
}

StdLogosResult LogosBlockchainModule::wallet_transfer_funds(
    const std::string& change_public_key,
    const std::vector<std::string>& sender_addresses,
    const std::string& recipient_address,
    const std::string& amount,
    const std::string& optional_tip_hex
) const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    std::string amount_trimmed = amount;
    boost::algorithm::trim(amount_trimmed);
    uint64_t amount_val = 0;
    auto [ptr, ec] = std::from_chars(amount_trimmed.data(), amount_trimmed.data() + amount_trimmed.size(), amount_val);
    if (ec != std::errc{} || ptr != amount_trimmed.data() + amount_trimmed.size() || amount_trimmed.empty()) {
        return result::err("Invalid amount (positive integer required).");
    }

    const std::vector<uint8_t> change_bytes = parse_address_hex(change_public_key);
    if (change_bytes.empty() || static_cast<int>(change_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid change_public_key (64 hex characters required).");
    }
    const std::vector<uint8_t> recipient_bytes = parse_address_hex(recipient_address);
    if (recipient_bytes.empty() || static_cast<int>(recipient_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid recipient_address (64 hex characters required).");
    }
    if (sender_addresses.empty()) {
        return result::err("At least one sender address is required.");
    }
    std::vector<std::vector<uint8_t>> funding_bytes;
    for (const std::string& hex : sender_addresses) {
        std::vector<uint8_t> b = parse_address_hex(hex);
        if (b.empty() || static_cast<int>(b.size()) != ADDRESS_BYTES) {
            return result::err("Invalid sender address (64 hex characters required).");
        }
        funding_bytes.push_back(std::move(b));
    }
    std::vector<const uint8_t*> funding_ptrs;
    for (const auto& b : funding_bytes)
        funding_ptrs.push_back(b.data());

    std::vector<uint8_t> tip_bytes;
    const HeaderId* optional_tip = nullptr;
    if (!optional_tip_hex.empty()) {
        tip_bytes = parse_address_hex(optional_tip_hex);
        if (tip_bytes.empty() || static_cast<int>(tip_bytes.size()) != ADDRESS_BYTES) {
            return result::err("Invalid optional tip (64 hex characters or empty).");
        }
        optional_tip = reinterpret_cast<const HeaderId*>(tip_bytes.data());
    }

    TransferFundsArguments args{};
    args.optional_tip = optional_tip;
    args.change_public_key = change_bytes.data();
    args.funding_public_keys = funding_ptrs.data();
    args.funding_public_keys_len = funding_ptrs.size();
    args.recipient_public_key = recipient_bytes.data();
    args.amount = amount_val;

    auto [value, error] = transfer_funds(node, &args);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }
    return result::ok(bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), ADDRESS_BYTES));
}

StdLogosResult LogosBlockchainModule::wallet_get_known_addresses() const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        fprintf(stderr, "Could not execute the operation: The node is not running.\n");
        return result::err("The node is not running.");
    }
    auto [value, error] = get_known_addresses(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }
    std::vector<std::string> out;
    for (size_t i = 0; i < value.len; ++i) {
        // ReSharper disable once CppTooWideScope
        const uint8_t* ptr = value.addresses[i];
        if (ptr) {
            out.push_back(bytes_to_hex(ptr, ADDRESS_BYTES));
        }
    }
    OperationStatus free_status = free_known_addresses(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free known addresses: %s\n", operation_status::take_message(free_status).c_str());
    }
    fprintf(
        stderr,
        "blockchain lib: known addresses, count=%zu sample:%s\n",
        out.size(),
        out.empty() ? "(none)" : out.front().c_str()
    );
    return result::ok(std::move(out));
}

StdLogosResult LogosBlockchainModule::wallet_get_notes(
    const std::string& wallet_address_hex,
    const std::string& optional_tip_hex
) const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    const std::vector<uint8_t> address_bytes = parse_address_hex(wallet_address_hex);
    if (address_bytes.empty() || static_cast<int>(address_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid wallet address (64 hex characters required).");
    }

    std::vector<uint8_t> tip_bytes;
    const HeaderId* optional_tip = nullptr;
    if (!optional_tip_hex.empty()) {
        tip_bytes = parse_address_hex(optional_tip_hex);
        if (tip_bytes.empty() || static_cast<int>(tip_bytes.size()) != ADDRESS_BYTES) {
            return result::err("Invalid optional tip (64 hex characters or empty).");
        }
        optional_tip = reinterpret_cast<const HeaderId*>(tip_bytes.data());
    }

    auto [value, error] = get_wallet_notes(node, address_bytes.data(), optional_tip);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    json obj;
    obj["tip"] = bytes_to_hex(value.tip, TX_HASH_BYTES);
    json notes = json::array();
    for (size_t i = 0; i < value.len; ++i) {
        const auto& [note_id, note_value] = value.notes[i];
        json n;
        n["id"] = bytes_to_hex(note_id, TX_HASH_BYTES);
        // Value is u64; serialized as a string to avoid JSON number precision loss.
        n["value"] = std::to_string(note_value);
        notes.push_back(std::move(n));
    }
    obj["notes"] = std::move(notes);

    OperationStatus free_status = free_wallet_notes(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free wallet notes: %s\n", operation_status::take_message(free_status).c_str());
    }
    return result::ok(obj.dump());
}

StdLogosResult LogosBlockchainModule::leader_claim() const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::leader_claim(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    return result::ok(bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), TX_HASH_BYTES));
}

// Channel

StdLogosResult LogosBlockchainModule::channel_deposit(
    const std::string& channel_id_hex,
    const std::string& funding_public_key_hex,
    const std::string& amount,
    const std::string& metadata_hex,
    const std::string& optional_tip_hex
) const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    std::string amount_trimmed = amount;
    boost::algorithm::trim(amount_trimmed);
    uint64_t amount_val = 0;
    auto [ptr, ec] = std::from_chars(amount_trimmed.data(), amount_trimmed.data() + amount_trimmed.size(), amount_val);
    if (ec != std::errc{} || ptr != amount_trimmed.data() + amount_trimmed.size() || amount_trimmed.empty()) {
        return result::err("Invalid amount (positive integer required).");
    }
    if (amount_val == 0) {
        return result::err("Invalid amount (must be greater than zero).");
    }

    const std::vector<uint8_t> channel_bytes = parse_address_hex(channel_id_hex);
    if (channel_bytes.empty() || static_cast<int>(channel_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid channel_id (64 hex characters required).");
    }

    const std::vector<uint8_t> funding_bytes = parse_address_hex(funding_public_key_hex);
    if (funding_bytes.empty() || static_cast<int>(funding_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid funding_public_key (64 hex characters required).");
    }

    std::vector<uint8_t> metadata_bytes;
    if (!metadata_hex.empty() && !parse_hex_bytes(metadata_hex, metadata_bytes)) {
        return result::err("Invalid metadata (even-length hex string required).");
    }

    std::vector<uint8_t> tip_bytes;
    const HeaderId* optional_tip = nullptr;
    if (!optional_tip_hex.empty()) {
        tip_bytes = parse_address_hex(optional_tip_hex);
        if (tip_bytes.empty() || static_cast<int>(tip_bytes.size()) != ADDRESS_BYTES) {
            return result::err("Invalid optional tip (64 hex characters or empty).");
        }
        optional_tip = reinterpret_cast<const HeaderId*>(tip_bytes.data());
    }

    ChannelDepositArguments args{};
    args.optional_tip = optional_tip;
    args.channel_id = channel_bytes.data();
    args.funding_public_key = funding_bytes.data();
    args.amount = amount_val;
    args.metadata = metadata_bytes.empty() ? nullptr : metadata_bytes.data();
    args.metadata_len = metadata_bytes.size();

    auto [value, error] = ::channel_deposit(node, &args);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }
    return result::ok(bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), ADDRESS_BYTES));
}

StdLogosResult LogosBlockchainModule::channel_deposit_with_notes(
    const std::string& channel_id_hex,
    const std::vector<std::string>& input_note_id_hexes,
    const std::string& metadata_hex,
    const std::string& change_public_key_hex,
    const std::vector<std::string>& funding_public_key_hexes,
    const std::string& max_tx_fee,
    const std::string& optional_tip_hex
) const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    const std::vector<uint8_t> channel_bytes = parse_address_hex(channel_id_hex);
    if (channel_bytes.empty() || static_cast<int>(channel_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid channel_id (64 hex characters required).");
    }

    if (input_note_id_hexes.empty()) {
        return result::err("At least one input note is required.");
    }
    // Note IDs are 32-byte values stored contiguously so the buffer can be passed
    // as a `NoteId` (uint8_t[32]) array.
    std::vector<uint8_t> note_ids_flat;
    note_ids_flat.reserve(input_note_id_hexes.size() * ADDRESS_BYTES);
    for (const std::string& hex : input_note_id_hexes) {
        const std::vector<uint8_t> b = parse_address_hex(hex);
        if (b.empty() || static_cast<int>(b.size()) != ADDRESS_BYTES) {
            return result::err("Invalid input note id (64 hex characters required).");
        }
        note_ids_flat.insert(note_ids_flat.end(), b.begin(), b.end());
    }

    const std::vector<uint8_t> change_bytes = parse_address_hex(change_public_key_hex);
    if (change_bytes.empty() || static_cast<int>(change_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid change_public_key (64 hex characters required).");
    }

    if (funding_public_key_hexes.empty()) {
        return result::err("At least one funding public key is required.");
    }
    std::vector<std::vector<uint8_t>> funding_bytes;
    for (const std::string& hex : funding_public_key_hexes) {
        std::vector<uint8_t> b = parse_address_hex(hex);
        if (b.empty() || static_cast<int>(b.size()) != ADDRESS_BYTES) {
            return result::err("Invalid funding public key (64 hex characters required).");
        }
        funding_bytes.push_back(std::move(b));
    }
    std::vector<const uint8_t*> funding_ptrs;
    funding_ptrs.reserve(funding_bytes.size());
    for (const auto& b : funding_bytes)
        funding_ptrs.push_back(b.data());

    std::string fee_trimmed = max_tx_fee;
    boost::algorithm::trim(fee_trimmed);
    uint64_t max_tx_fee_val = 0;
    auto [ptr, ec] = std::from_chars(fee_trimmed.data(), fee_trimmed.data() + fee_trimmed.size(), max_tx_fee_val);
    if (ec != std::errc{} || ptr != fee_trimmed.data() + fee_trimmed.size() || fee_trimmed.empty()) {
        return result::err("Invalid max_tx_fee (non-negative integer required).");
    }

    std::vector<uint8_t> metadata_bytes;
    if (!metadata_hex.empty() && !parse_hex_bytes(metadata_hex, metadata_bytes)) {
        return result::err("Invalid metadata (even-length hex string required).");
    }

    std::vector<uint8_t> tip_bytes;
    const HeaderId* optional_tip = nullptr;
    if (!optional_tip_hex.empty()) {
        tip_bytes = parse_address_hex(optional_tip_hex);
        if (tip_bytes.empty() || static_cast<int>(tip_bytes.size()) != ADDRESS_BYTES) {
            return result::err("Invalid optional tip (64 hex characters or empty).");
        }
        optional_tip = reinterpret_cast<const HeaderId*>(tip_bytes.data());
    }

    ChannelDepositWithNotesArguments args{};
    args.optional_tip = optional_tip;
    args.channel_id = channel_bytes.data();
    args.input_note_ids = reinterpret_cast<const NoteId*>(note_ids_flat.data());
    args.input_note_ids_len = input_note_id_hexes.size();
    args.metadata = metadata_bytes.empty() ? nullptr : metadata_bytes.data();
    args.metadata_len = metadata_bytes.size();
    args.change_public_key = change_bytes.data();
    args.funding_public_keys = funding_ptrs.data();
    args.funding_public_keys_len = funding_ptrs.size();
    args.max_tx_fee = max_tx_fee_val;

    auto [value, error] = ::channel_deposit_with_notes(node, &args);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }
    return result::ok(bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), ADDRESS_BYTES));
}

StdLogosResult LogosBlockchainModule::wallet_get_claimable_vouchers() const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = get_claimable_vouchers(node, nullptr);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    json obj;
    obj["tip"] = bytes_to_hex(reinterpret_cast<const uint8_t*>(&value.tip), ADDRESS_BYTES);
    obj["vouchers"] = json::array();

    for (size_t i = 0; i < value.len; ++i) {
        const auto& [commitment, nullifier] = value.vouchers[i];
        obj["vouchers"].push_back({
            {"commitment", bytes_to_hex(reinterpret_cast<const uint8_t*>(&commitment), ADDRESS_BYTES)},
            {"nullifier", bytes_to_hex(reinterpret_cast<const uint8_t*>(&nullifier), ADDRESS_BYTES)},
        });
    }

    OperationStatus free_status = free_claimable_vouchers(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free claimable vouchers: %s\n", operation_status::take_message(free_status).c_str());
    }

    return result::ok(obj.dump());
}

// Blend

StdLogosResult LogosBlockchainModule::blend_join_as_core_node(
    const std::string& provider_id_hex,
    const std::string& zk_id_hex,
    const std::string& locked_note_id_hex,
    const std::vector<std::string>& locators
) const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    const std::vector<uint8_t> provider_id_bytes = parse_address_hex(provider_id_hex);
    if (provider_id_bytes.empty() || static_cast<int>(provider_id_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid provider_id_hex (64 hex characters required).");
    }

    const std::vector<uint8_t> zk_id_bytes = parse_address_hex(zk_id_hex);
    if (zk_id_bytes.empty() || static_cast<int>(zk_id_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid zk_id_hex (64 hex characters required).");
    }

    const std::vector<uint8_t> locked_note_id_bytes = parse_address_hex(locked_note_id_hex);
    if (locked_note_id_bytes.empty() || static_cast<int>(locked_note_id_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid locked_note_id_hex (64 hex characters required).");
    }

    if (locators.empty()) {
        return result::err("At least one Blend locator is required.");
    }
    if (locators.size() != 1) {
        return result::err("The current Blend binding accepts exactly one locator per join.");
    }

    const std::vector<uint8_t> configured_provider_id = blendProviderIdentity;
    const std::vector<uint8_t> configured_zk_id = blendZkIdentity;
    if (configured_provider_id.empty() || configured_zk_id.empty()) {
        return result::err("Unable to verify Blend identities from the running node configuration.");
    }
    // The current C binding derives KMS-backed identities inside the running
    // node but does not expose the resolved public keys. Reject that form until
    // the binding can return those identities, rather than accepting a caller
    // supplied ID that cannot be verified against the declaration.
    if (!configured_provider_id.empty() && provider_id_bytes != configured_provider_id) {
        return result::err("provider_id_hex does not match the running node's BlendSigning identity.");
    }
    if (!configured_zk_id.empty() && zk_id_bytes != configured_zk_id) {
        return result::err("zk_id_hex does not match the running node's BlendZk identity.");
    }

    auto [value, error] = ::blend_join_as_core_node(
        node,
        locators.front().c_str(),
        locked_note_id_bytes.data()
    );
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    std::string declaration_id = bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), sizeof(value));
    fprintf(stderr, "Successfully joined as a core node. DeclarationId: %s\n", declaration_id.c_str());
    return result::ok(std::move(declaration_id));
}

// Explorer

StdLogosResult LogosBlockchainModule::get_block(const std::string& header_id_hex) const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    const std::vector<uint8_t> bytes = parse_address_hex(header_id_hex);
    if (bytes.empty() || static_cast<int>(bytes.size()) != ADDRESS_BYTES) {
        return result::err("Header ID must be 64 hex characters (32 bytes).");
    }

    auto [value, error] = ::get_block(node, reinterpret_cast<const HeaderId*>(bytes.data()));
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    StdLogosResult raw = copy_cstring_result(value, "get_block");
    if (!raw.success) {
        return raw;
    }
    return normalize_block_json(raw.value.get<std::string>(), bytes_to_hex(bytes.data(), bytes.size()));
}

StdLogosResult LogosBlockchainModule::get_blocks(const uint64_t from_slot, const uint64_t to_slot) const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }
    if (from_slot > to_slot) {
        return result::err("from_slot must not be greater than to_slot.");
    }

    auto [value, error] = ::get_blocks(node, from_slot, to_slot);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    StdLogosResult raw = copy_cstring_result(value, "get_blocks");
    if (!raw.success) {
        return raw;
    }

    const std::string raw_json = raw.value.get<std::string>();
    json immutable_blocks;
    try {
        immutable_blocks = json::parse(raw_json);
    } catch (const json::parse_error&) {
        return raw;
    }
    if (!immutable_blocks.is_array()) {
        return raw;
    }
    if (!immutable_blocks.empty()) {
        bool normalized = false;
        for (json& block : immutable_blocks) {
            if (normalize_block_transaction_hashes(block)) {
                normalized = true;
            }
        }
        if (!normalized) {
            return raw;
        }
        return result::ok(immutable_blocks.dump());
    }

    // The existing C API's range query reads immutable blocks only. For a
    // bounded range near the live tip, fill the expected explorer data through
    // the already available individual-block read and parent links.
    auto [info, info_error] = ::get_cryptarchia_info(node);
    if (!is_ok(&info_error)) {
        return result::err(
            "get_blocks returned no blocks and could not read the current tip: " +
            operation_status::take_message(info_error)
        );
    }
    if (!info) {
        return result::err("get_blocks returned no blocks and cryptarchia info was empty.");
    }

    const uint64_t tip_slot = info->slot;
    const std::string tip_id = bytes_to_hex(reinterpret_cast<const uint8_t*>(info->tip), ADDRESS_BYTES);
    OperationStatus free_info_status = free_cryptarchia_info(info);
    if (!is_ok(&free_info_status)) {
        fprintf(
            stderr,
            "Failed to free cryptarchia info after block range read: %s\n",
            operation_status::take_message(free_info_status).c_str()
        );
    }

    if (tip_slot < from_slot || tip_slot - from_slot >= MAX_TIP_PARENT_WALK_BLOCKS) {
        return raw;
    }

    std::unordered_set<std::string> visited;
    std::vector<json> reverse_blocks;
    bool reached_lower_bound = false;
    std::string current_id = tip_id;
    for (size_t walked = 0; walked < MAX_TIP_PARENT_WALK_BLOCKS; ++walked) {
        if (!visited.insert(current_id).second) {
            return result::err("get_blocks encountered a repeated parent block id during live-tip traversal.");
        }

        StdLogosResult block_result = get_block(current_id);
        if (!block_result.success) {
            return result::err("get_blocks could not read live block `" + current_id + "`: " + block_result.error);
        }

        json block;
        try {
            block = json::parse(block_result.value.get<std::string>());
        } catch (const json::parse_error& parse_error) {
            return result::err(
                "get_blocks received invalid JSON while traversing live blocks: " + std::string(parse_error.what())
            );
        }

        uint64_t slot = 0;
        if (!block_slot(block, slot)) {
            return result::err("get_blocks received a live block without a valid header slot.");
        }
        if (slot < from_slot) {
            reached_lower_bound = true;
            break;
        }
        std::string parent_id;
        const bool needs_parent = slot > from_slot;
        if (needs_parent && !parent_block_id(block, parent_id)) {
            return result::err("get_blocks received a live block without a valid parent block id.");
        }
        if (slot <= to_slot) {
            reverse_blocks.push_back(std::move(block));
        }
        if (!needs_parent) {
            reached_lower_bound = true;
            break;
        }
        current_id = std::move(parent_id);
    }

    if (!reached_lower_bound) {
        return result::err("get_blocks reached the live-tip traversal limit before the requested lower slot.");
    }

    std::reverse(reverse_blocks.begin(), reverse_blocks.end());
    return result::ok(json(reverse_blocks).dump());
}

StdLogosResult LogosBlockchainModule::get_time_info() const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::get_time_info(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    return copy_cstring_result(value, "get_time_info");
}

StdLogosResult LogosBlockchainModule::get_finalized_blocks_range(
    const uint64_t from_slot,
    const uint64_t to_slot,
    const uint64_t blocks_limit
) const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::get_finalized_blocks_range(node, from_slot, to_slot, blocks_limit);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    return copy_cstring_result(value, "get_finalized_blocks_range");
}

StdLogosResult LogosBlockchainModule::get_cryptarchia_headers() const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::get_cryptarchia_headers(node, nullptr, nullptr);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    return copy_cstring_result(value, "get_cryptarchia_headers");
}

StdLogosResult LogosBlockchainModule::get_network_info() const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::get_network_info(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    return copy_cstring_result(value, "get_network_info");
}

StdLogosResult LogosBlockchainModule::get_mantle_metrics() const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::get_mantle_metrics(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    return copy_cstring_result(value, "get_mantle_metrics");
}

StdLogosResult LogosBlockchainModule::get_transaction(const std::string& tx_hash_hex) const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    const std::vector<uint8_t> bytes = parse_address_hex(tx_hash_hex);
    if (bytes.empty() || static_cast<int>(bytes.size()) != ADDRESS_BYTES) {
        return result::err("Transaction hash must be 64 hex characters (32 bytes).");
    }

    auto [value, error] = ::get_transaction(node, reinterpret_cast<const TxHash*>(bytes.data()));
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    StdLogosResult raw = copy_cstring_result(value, "get_transaction");
    if (!raw.success) {
        return raw;
    }
    return normalize_transaction_json(raw.value.get<std::string>(), bytes_to_hex(bytes.data(), bytes.size()));
}

// Cryptarchia

StdLogosResult LogosBlockchainModule::get_cryptarchia_info() const {
    std::lock_guard<std::recursive_mutex> node_lock(nodeMutex);
    if (!node) {
        return result::err("The node is not running.");
    }

    const uint32_t abi_version = ::cryptarchia_info_abi_version();
    if (abi_version != EXPECTED_CRYPTARCHIA_INFO_ABI_VERSION) {
        return result::err(
            "Incompatible CryptarchiaInfo C ABI: expected version " +
            std::to_string(EXPECTED_CRYPTARCHIA_INFO_ABI_VERSION) + ", got " + std::to_string(abi_version) +
            ". Use a matching logos-blockchain C library."
        );
    }

    auto [value, error] = ::get_cryptarchia_info(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    if (!value) {
        return result::err("get_cryptarchia_info returned an empty response.");
    }

    json obj;
    obj["lib"] = bytes_to_hex(reinterpret_cast<const uint8_t*>(value->lib), ADDRESS_BYTES);
    obj["tip"] = bytes_to_hex(reinterpret_cast<const uint8_t*>(value->tip), ADDRESS_BYTES);
    obj["slot"] = static_cast<int64_t>(value->slot);
    obj["height"] = static_cast<int64_t>(value->height);
    obj["lib_slot"] = static_cast<int64_t>(value->lib_slot);
    obj["genesis_id"] = bytes_to_hex(reinterpret_cast<const uint8_t*>(value->genesis_id), sizeof(value->genesis_id));
    switch (value->mode) {
    case State::Online:
        obj["mode"] = "Online";
        break;
    case State::Bootstrapping:
        obj["mode"] = "Bootstrapping";
        break;
    case State::NotStarted:
        obj["mode"] = "NotStarted";
        break;
    default:
        obj["mode"] = "Unknown";
        break;
    }

    OperationStatus free_status = free_cryptarchia_info(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free cryptarchia info: %s\n", operation_status::take_message(free_status).c_str());
    }

    return result::ok(obj.dump());
}
