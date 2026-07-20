#include "logos_blockchain_module.h"

#include <algorithm>
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <cctype>
#include <charconv>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Define static member
LogosBlockchainModule* LogosBlockchainModule::s_instance = nullptr;

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

    // Rust `File::open` / `deserialize_config_at_path` only accept real filesystem paths. QML often
    // passes `file:///...` URLs; strip to a local path when applicable.
    std::string localPathFromFileUrl(const std::string& s) {
        if (s.size() >= 7 && s.substr(0, 7) == "file://")
            return s.substr(7);
        if (s.size() >= 5 && s.substr(0, 5) == "file:")
            return s.substr(5);
        return s;
    }

    // Use the C API type Hash (from logos_blockchain.h) to define address/hash byte size.
    constexpr int ADDRESS_BYTES = sizeof(Hash);
    constexpr int TX_HASH_BYTES = sizeof(TxHash);
    constexpr int ADDRESS_HEX_LEN = ADDRESS_BYTES * 2;

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

    void normalize_event_transaction_hashes(json& block) {
        if (!block.is_object()) {
            return;
        }
        auto transactions = block.find("transactions");
        if (transactions == block.end() || !transactions->is_array()) {
            return;
        }

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
            (*mantle_transaction)["hash"] = hash_bytes.empty()
                                                  ? id->get<std::string>()
                                                  : bytes_to_hex(hash_bytes.data(), hash_bytes.size());
        }
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
        bool skip_ibd_val;
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

            // skip_ibd (bool -> const bool*)
            if (args.contains("skip_ibd") && args["skip_ibd"].is_boolean()) {
                skip_ibd_val = args["skip_ibd"].get<bool>();
                ffi_args.skip_ibd = &skip_ibd_val;
            } else {
                ffi_args.skip_ibd = nullptr;
            }

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
    if (!s_instance || !block) {
        return;
    }

    // The C API borrows this pointer for the duration of the callback. Copy the
    // JSON synchronously and avoid calling back into the node from this stream
    // callback, which runs on the core runtime.
    try {
        json parsed_block = json::parse(block);
        normalize_event_transaction_hashes(parsed_block);

        json event;
        event["block"] = std::move(parsed_block);
        s_instance->newBlock(event.dump());
    } catch (const json::parse_error& error) {
        // Keep the legacy fallback for an invalid payload, but never emit the
        // full payload to stderr: a busy block stream must not stall on logs.
        fprintf(stderr, "Failed to parse new block event JSON: %s\n", error.what());
        json event;
        event["block"] = std::string(block);
        s_instance->newBlock(event.dump());
    }
}

LogosBlockchainModule::LogosBlockchainModule() {
    node = nullptr;
}

LogosBlockchainModule::~LogosBlockchainModule() {
    s_instance = nullptr;
    if (node) {
        (void)stop();
    }
}

// ---- Node ----

// Lifecycle

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
            // caller's path as the relative part below it ("config/user_config.yaml"
            // → "<base>/config/user_config.yaml"). Absolute / root-anchored inputs
            // (e.g. "//user_config.yaml" from QDir::currentPath()=="/") are treated
            // as relative to the base; a missing output defaults to
            // "<base>/user_config.yaml".
            fs::path output_rel = "user_config.yaml";
            if (parsed_args.contains("output") && parsed_args["output"].is_string() &&
                !parsed_args["output"].get<std::string>().empty()) {
                const fs::path given(localPathFromFileUrl(parsed_args["output"].get<std::string>()));
                const fs::path rel = given.relative_path();
                output_rel = rel.empty() ? given.filename() : rel;
                if (output_rel.empty())
                    output_rel = "user_config.yaml";
            }
            parsed_args["output"] = (base / output_rel).lexically_normal().string();

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

StdLogosResult LogosBlockchainModule::start(const std::string& config_path, const std::string& deployment) {
    if (node) {
        fprintf(stderr, "Could not execute the operation: The node is already running.\n");
        return result::err("The node is already running.");
    }

    std::string effective_config_path = config_path;

    if (effective_config_path.empty()) {
        const char* env = std::getenv("LB_CONFIG_PATH");
        if (env && *env) {
            effective_config_path = env;
            fprintf(stderr, "Using config from LB_CONFIG_PATH: %s\n", effective_config_path.c_str());
        } else {
            fprintf(stderr, "Config path was not specified and LB_CONFIG_PATH is not set.\n");
            return result::err("Config path was not specified and LB_CONFIG_PATH is not set.");
        }
    }

    effective_config_path = localPathFromFileUrl(effective_config_path);
    const std::string deployment_path = localPathFromFileUrl(deployment);

    const char* config_path_ptr = effective_config_path.empty() ? nullptr : effective_config_path.c_str();
    const char* deployment_ptr = deployment_path.empty() ? nullptr : deployment_path.c_str();

    auto [value, error] = start_lb_node(config_path_ptr, deployment_ptr);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    node = value;

    if (!node) {
        return result::err("Could not subscribe to block events: the node is not running.");
    }

    s_instance = this;
    OperationStatus subscribe_status = subscribe_to_new_blocks(node, on_new_block_callback);
    return result::from_operation_status(subscribe_status);
}

StdLogosResult LogosBlockchainModule::stop() {
    if (!node) {
        fprintf(stderr, "Could not execute the operation: The node is not running.\n");
        return result::err("The node is not running.");
    }

    s_instance = nullptr;

    OperationStatus status = shutdown_node(node);
    if (!is_ok(&status)) {
        fprintf(stderr, "Could not stop the node: %s\n", operation_status::take_message(status).c_str());
    }

    node = nullptr;
    return result::ok();
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
    const std::string& locator,
    const std::string& locked_note_id_hex
) const {
    if (!node) {
        return result::err("The node is not running.");
    }

    if (locator.empty()) {
        return result::err("Invalid locator (must not be empty).");
    }

    const std::vector<uint8_t> locked_note_id_bytes = parse_address_hex(locked_note_id_hex);
    if (locked_note_id_bytes.empty() || static_cast<int>(locked_note_id_bytes.size()) != ADDRESS_BYTES) {
        return result::err("Invalid locked_note_id_hex (64 hex characters required).");
    }

    auto [value, error] = ::blend_join_as_core_node(
        node,
        locator.c_str(),
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

    StdLogosResult raw = copy_cstring_result(value, "block");
    if (!raw.success) {
        return raw;
    }
    return normalize_block_json(raw.value.get<std::string>(), bytes_to_hex(bytes.data(), bytes.size()));
}

StdLogosResult LogosBlockchainModule::get_blocks(const uint64_t from_slot, const uint64_t to_slot) const {
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

    StdLogosResult raw = copy_cstring_result(value, "blocks");
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
    if (!immutable_blocks.is_array() || !immutable_blocks.empty()) {
        return raw;
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
    std::string current_id = tip_id;
    for (size_t walked = 0; walked < MAX_TIP_PARENT_WALK_BLOCKS; ++walked) {
        if (!visited.insert(current_id).second) {
            return result::err(
                "get_blocks encountered a repeated parent block id during live-tip traversal."
            );
        }

        StdLogosResult block_result = get_block(current_id);
        if (!block_result.success) {
            return result::err(
                "get_blocks could not read live block `" + current_id + "`: " + block_result.error
            );
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
            break;
        }
        const bool reached_lower_bound = slot <= from_slot || slot == 0;
        std::string parent_id;
        if (!reached_lower_bound && !parent_block_id(block, parent_id)) {
            return result::err("get_blocks received a live block without a valid parent block id.");
        }
        if (slot <= to_slot) {
            reverse_blocks.push_back(std::move(block));
        }
        if (reached_lower_bound) {
            break;
        }
        current_id = std::move(parent_id);
    }

    std::reverse(reverse_blocks.begin(), reverse_blocks.end());
    return result::ok(json(reverse_blocks).dump());
}

StdLogosResult LogosBlockchainModule::get_transaction(const std::string& tx_hash_hex) const {
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

    StdLogosResult raw = copy_cstring_result(value, "transaction");
    if (!raw.success) {
        return raw;
    }
    return normalize_transaction_json(raw.value.get<std::string>(), bytes_to_hex(bytes.data(), bytes.size()));
}

// Cryptarchia

StdLogosResult LogosBlockchainModule::get_cryptarchia_info() const {
    if (!node) {
        return result::err("The node is not running.");
    }

    auto [value, error] = ::get_cryptarchia_info(node);
    if (!is_ok(&error)) {
        return result::err(operation_status::take_message(error));
    }

    if (!value) {
        return result::err("get_cryptarchia_info returned an empty response.");
    }

    HeaderId lib_header{};
    std::memcpy(lib_header, value->lib, sizeof(lib_header));

    json obj;
    obj["lib"] = bytes_to_hex(reinterpret_cast<const uint8_t*>(value->lib), ADDRESS_BYTES);
    obj["tip"] = bytes_to_hex(reinterpret_cast<const uint8_t*>(value->tip), ADDRESS_BYTES);
    obj["slot"] = static_cast<int64_t>(value->slot);
    obj["height"] = static_cast<int64_t>(value->height);
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

    // `CryptarchiaInfo` has no LIB slot in the current C ABI. The block is
    // already addressable, so derive it without changing the node interface.
    const std::string lib_id = bytes_to_hex(reinterpret_cast<const uint8_t*>(lib_header), sizeof(lib_header));
    StdLogosResult lib_block = get_block(lib_id);
    if (!lib_block.success) {
        fprintf(stderr, "Could not derive LIB slot: %s\n", lib_block.error.c_str());
        return result::ok(obj.dump());
    }
    try {
        const json block = json::parse(lib_block.value.get<std::string>());
        uint64_t lib_slot = 0;
        if (block_slot(block, lib_slot)) {
            obj["lib_slot"] = lib_slot;
        } else {
            fprintf(stderr, "Could not derive LIB slot: block response has no valid header slot.\n");
        }
    } catch (const json::parse_error& parse_error) {
        fprintf(stderr, "Could not derive LIB slot: invalid block JSON: %s\n", parse_error.what());
    }
    return result::ok(obj.dump());
}
