#include "logos_blockchain_module.h"

#include <algorithm>
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Define static member
LogosBlockchainModule* LogosBlockchainModule::s_instance = nullptr;

namespace {
    // Rust `File::open` / `deserialize_config_at_path` only accept real filesystem paths. QML often
    // passes `file:///...` URLs; strip to a local path when applicable.
    std::string localPathFromFileUrl(const std::string& s) {
        if (s.size() >= 7 && s.substr(0, 7) == "file://") return s.substr(7);
        if (s.size() >= 5 && s.substr(0, 5) == "file:") return s.substr(5);
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

    std::string bytes_to_hex(const uint8_t* data, size_t len) {
        std::string out;
        out.reserve(len * 2);
        boost::algorithm::hex_lower(data, data + len, std::back_inserter(out));
        return out;
    }

    // Map a "ed25519" / "zk" string (case-insensitive) to the C KeyType enum.
    bool parse_key_type(const std::string& s, KeyType& out) {
        std::string lower = s;
        boost::algorithm::trim(lower);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
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
        bool ibd_val;
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

            // ibd (bool -> const bool*)
            if (args.contains("ibd") && args["ibd"].is_boolean()) {
                ibd_val = args["ibd"].get<bool>();
                ffi_args.ibd = &ibd_val;
            } else {
                ffi_args.ibd = nullptr;
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

namespace environment {
    constexpr auto LOGOS_BLOCKCHAIN_CIRCUITS = "LOGOS_BLOCKCHAIN_CIRCUITS";

    bool is_circuits_path_valid(const std::string& path) {
        std::error_code ec;
        if (!fs::is_directory(path, ec)) return false;
        for (const auto& entry : fs::directory_iterator(path, ec)) {
            if (entry.is_regular_file()) return true;
        }
        return false;
    }

    void setup_circuits_path(const std::string& module_path) {
        fs::path circuits_path = fs::path(module_path) / "circuits";
        std::string circuits_str = circuits_path.string();

        if (!is_circuits_path_valid(circuits_str)) {
            fprintf(stderr, "FATAL: The LOGOS_BLOCKCHAIN_CIRCUITS environment variable is not set or does not contain any files.\n");
            return;
        }

        setenv("LOGOS_BLOCKCHAIN_CIRCUITS", circuits_str.c_str(), 1);
        fprintf(stderr, "LOGOS_BLOCKCHAIN_CIRCUITS set to: %s\n", circuits_str.c_str());
    }
} // namespace environment

void LogosBlockchainModule::on_new_block_callback(const char* block) {
    if (s_instance) {
        fprintf(stderr, "Received new block: %s\n", block);
        json j;
        j["block"] = std::string(block);
        s_instance->newBlock(j.dump());
        // SAFETY:
        // We are getting an owned pointer here which is freed after this callback is called, so there is no need to
        // free the resource here as we are copying the data!
    }
}

LogosBlockchainModule::LogosBlockchainModule() {
    node = nullptr;
}

LogosBlockchainModule::~LogosBlockchainModule() {
    s_instance = nullptr;
    if (node) {
        stop();
    }
}

// ---- Node ----

// Lifecycle

std::string LogosBlockchainModule::generate_user_config(const std::string& json_args) {
    json parsed_args;
    try {
        parsed_args = json::parse(json_args);
    } catch (const json::parse_error& e) {
        fprintf(stderr, "Failed to parse JSON args: %s\n", e.what());
        return "1";
    }

    const OwnedGenerateConfigArgs owned_args(parsed_args);

    const OperationStatus status = ::generate_user_config(owned_args.ffi_args);
    if (!is_ok(&status)) {
        fprintf(stderr, "Failed to generate user config. Error: %d\n", status);
        return "1";
    }

    return "0";
}

std::string LogosBlockchainModule::start(const std::string& config_path, const std::string& deployment) {
    if (node) {
        fprintf(stderr, "Could not execute the operation: The node is already running.\n");
        return "1";
    }

    const char* module_path_env = std::getenv("LOGOS_MODULE_PATH");
    if (module_path_env && *module_path_env) {
        environment::setup_circuits_path(module_path_env);
    } else {
        fprintf(stderr, "Warning: LOGOS_MODULE_PATH not set, skipping circuits path setup.\n");
    }

    std::string effective_config_path = config_path;

    if (effective_config_path.empty()) {
        const char* env = std::getenv("LB_CONFIG_PATH");
        if (env && *env) {
            effective_config_path = env;
            fprintf(stderr, "Using config from LB_CONFIG_PATH: %s\n", effective_config_path.c_str());
        } else {
            fprintf(stderr, "Config path was not specified and LB_CONFIG_PATH is not set.\n");
            return "3";
        }
    }

    effective_config_path = localPathFromFileUrl(effective_config_path);
    const std::string deployment_path = localPathFromFileUrl(deployment);

    const char* config_path_ptr = effective_config_path.empty() ? nullptr : effective_config_path.c_str();
    const char* deployment_ptr = deployment_path.empty() ? nullptr : deployment_path.c_str();

    auto [value, error] = start_lb_node(config_path_ptr, deployment_ptr);
    fprintf(stderr, "Start node returned with value and error.\n");
    if (!is_ok(&error)) {
        fprintf(stderr, "Failed to start the node. Error: %d\n", error);
        return "4";
    }

    node = value;
    fprintf(stderr, "The node was started successfully.\n");

    if (!node) {
        fprintf(stderr, "Could not subscribe to block events: The node is not running.\n");
        return "4";
    }

    s_instance = this;
    const OperationStatus subscribe_status = subscribe_to_new_blocks(node, on_new_block_callback);
    if (!is_ok(&subscribe_status)) {
        fprintf(stderr, "Failed to subscribe to new blocks. Error: %d\n", subscribe_status);
        return "5";
    }

    return "0";
}

std::string LogosBlockchainModule::stop() {
    if (!node) {
        fprintf(stderr, "Could not execute the operation: The node is not running.\n");
        return "1";
    }

    s_instance = nullptr;

    const OperationStatus status = stop_node(node);
    if (is_ok(&status)) {
        fprintf(stderr, "The node was stopped successfully.\n");
    } else {
        fprintf(stderr, "Could not stop the node. Error: %d\n", status);
    }

    node = nullptr;
    return "0";
}

// Config management

std::string LogosBlockchainModule::update_user_config(const std::string& user_config_path, const std::string& keystore_path) {
    const std::string config = localPathFromFileUrl(user_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);

    const OperationStatus status = ::update_user_config(config.c_str(), keystore.c_str());
    if (!is_ok(&status)) {
        fprintf(stderr, "Failed to update user config. Error: %d\n", status);
        return "1";
    }
    return "0";
}

std::string LogosBlockchainModule::migrate_user_config(const std::string& output_path, const std::string& keystore_path) {
    const std::string output = localPathFromFileUrl(output_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);

    const OperationStatus status = ::migrate_user_config(output.c_str(), keystore.c_str());
    if (!is_ok(&status)) {
        fprintf(stderr, "Failed to migrate user config. Error: %d\n", status);
        return "1";
    }
    return "0";
}

std::string LogosBlockchainModule::migrate_user_config_0_1_2(
    const std::string& new_config_path,
    const std::string& old_config_path,
    const std::string& keystore_path
) {
    const std::string new_config = localPathFromFileUrl(new_config_path);
    const std::string old_config = localPathFromFileUrl(old_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);

    const OperationStatus status =
        ::migrate_user_config_0_1_2(new_config.c_str(), old_config.c_str(), keystore.c_str());
    if (!is_ok(&status)) {
        fprintf(stderr, "Failed to migrate 0.1.2 config. Error: %d\n", status);
        return "1";
    }
    return "0";
}

std::string LogosBlockchainModule::participate(
    const std::string& config_path,
    const std::string& keystore_path,
    const std::string& output_dir,
    const std::string& external_address
) {
    const std::string config = localPathFromFileUrl(config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);
    const std::string output = localPathFromFileUrl(output_dir);
    const char* external_address_ptr = external_address.empty() ? nullptr : external_address.c_str();

    const OperationStatus status =
        ::participate(config.c_str(), keystore.c_str(), output.c_str(), external_address_ptr);
    if (!is_ok(&status)) {
        fprintf(stderr, "Failed to generate participation data. Error: %d\n", status);
        return "1";
    }
    return "0";
}

// Keystore

std::string LogosBlockchainModule::generate_key(
    const std::string& user_config_path,
    const std::string& keystore_path,
    const std::string& key_type,
    const std::string& key_title
) {
    KeyType type{};
    if (!parse_key_type(key_type, type)) {
        return "Error: Invalid key_type (expected \"ed25519\" or \"zk\").";
    }

    const std::string config = localPathFromFileUrl(user_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);
    const char* key_title_ptr = key_title.empty() ? nullptr : key_title.c_str();

    auto [value, error] = ::generate_key(config.c_str(), keystore.c_str(), type, key_title_ptr);
    if (!is_ok(&error)) {
        fprintf(stderr, "Failed to generate key. Error: %d\n", error);
        return "Error: Failed to generate key: " + std::to_string(error);
    }

    std::string result(value);
    const OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free key id string. Error: %d\n", free_status);
    }
    return result;
}

std::string LogosBlockchainModule::add_key(
    const std::string& user_config_path,
    const std::string& keystore_path,
    const std::string& key_type,
    const std::string& key_hex,
    const std::string& key_title
) {
    KeyType type{};
    if (!parse_key_type(key_type, type)) {
        fprintf(stderr, "Invalid key_type (expected \"ed25519\" or \"zk\").\n");
        return "1";
    }

    const std::string config = localPathFromFileUrl(user_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);
    const char* key_title_ptr = key_title.empty() ? nullptr : key_title.c_str();

    const OperationStatus status =
        ::add_key(config.c_str(), keystore.c_str(), type, key_hex.c_str(), key_title_ptr);
    if (!is_ok(&status)) {
        fprintf(stderr, "Failed to add key. Error: %d\n", status);
        return "1";
    }
    return "0";
}

std::string LogosBlockchainModule::remove_key(
    const std::string& user_config_path,
    const std::string& keystore_path,
    const std::string& key_title
) {
    const std::string config = localPathFromFileUrl(user_config_path);
    const std::string keystore = localPathFromFileUrl(keystore_path);

    const OperationStatus status = ::remove_key(config.c_str(), keystore.c_str(), key_title.c_str());
    if (!is_ok(&status)) {
        fprintf(stderr, "Failed to remove key. Error: %d\n", status);
        return "1";
    }
    return "0";
}

// Identity

std::string LogosBlockchainModule::get_peer_id(const std::string& config_path) {
    const std::string config = localPathFromFileUrl(config_path);

    auto [value, error] = ::get_peer_id(config.c_str());
    if (!is_ok(&error)) {
        fprintf(stderr, "Failed to get peer id. Error: %d\n", error);
        return "Error: Failed to get peer id: " + std::to_string(error);
    }

    std::string result(value);
    const OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free peer id string. Error: %d\n", free_status);
    }
    return result;
}

// Wallet

std::string LogosBlockchainModule::wallet_get_balance(const std::string& address_hex) {
    fprintf(stderr, "wallet_get_balance: address_hex=%s\n", address_hex.c_str());
    if (!node) {
        return "Error: The node is not running.";
    }

    const std::vector<uint8_t> bytes = parse_address_hex(address_hex);
    if (bytes.empty() || static_cast<int>(bytes.size()) != ADDRESS_BYTES) {
        return "Error: Address must be 64 hex characters (32 bytes).";
    }

    auto [value, error] = get_balance(node, bytes.data(), nullptr);
    if (!is_ok(&error)) {
        return "Error: Failed to get balance: " + std::to_string(error);
    }

    return std::to_string(value);
}

std::string LogosBlockchainModule::wallet_transfer_funds(
    const std::string& change_public_key,
    const std::vector<std::string>& sender_addresses,
    const std::string& recipient_address,
    const std::string& amount,
    const std::string& optional_tip_hex
) {
    if (!node) {
        return "Error: The node is not running.";
    }

    std::string amount_trimmed = amount;
    boost::algorithm::trim(amount_trimmed);
    uint64_t amount_val = 0;
    auto [ptr, ec] = std::from_chars(amount_trimmed.data(), amount_trimmed.data() + amount_trimmed.size(), amount_val);
    if (ec != std::errc{} || ptr != amount_trimmed.data() + amount_trimmed.size() || amount_trimmed.empty()) {
        return "Error: Invalid amount (positive integer required).";
    }

    const std::vector<uint8_t> change_bytes = parse_address_hex(change_public_key);
    if (change_bytes.empty() || static_cast<int>(change_bytes.size()) != ADDRESS_BYTES) {
        return "Error: Invalid change_public_key (64 hex characters required).";
    }
    const std::vector<uint8_t> recipient_bytes = parse_address_hex(recipient_address);
    if (recipient_bytes.empty() || static_cast<int>(recipient_bytes.size()) != ADDRESS_BYTES) {
        return "Error: Invalid recipient_address (64 hex characters required).";
    }
    if (sender_addresses.empty()) {
        return "Error: At least one sender address required.";
    }
    std::vector<std::vector<uint8_t>> funding_bytes;
    for (const std::string& hex : sender_addresses) {
        std::vector<uint8_t> b = parse_address_hex(hex);
        if (b.empty() || static_cast<int>(b.size()) != ADDRESS_BYTES) {
            return "Error: Invalid sender address (64 hex characters required).";
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
            return "Error: Invalid optional tip (64 hex characters or empty).";
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
        return "Error: Failed to transfer funds: " + std::to_string(error);
    }
    return bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), ADDRESS_BYTES);
}

std::vector<std::string> LogosBlockchainModule::wallet_get_known_addresses() {
    std::vector<std::string> out;
    if (!node) {
        fprintf(stderr, "Could not execute the operation: The node is not running.\n");
        return out;
    }
    auto [value, error] = get_known_addresses(node);
    if (!is_ok(&error)) {
        fprintf(stderr, "Failed to get known addresses. Error: %d\n", error);
        return out;
    }
    for (size_t i = 0; i < value.len; ++i) {
        const uint8_t* ptr = value.addresses[i];
        if (ptr) {
            out.push_back(bytes_to_hex(ptr, ADDRESS_BYTES));
        }
    }
    const OperationStatus free_status = free_known_addresses(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free known addresses. Error: %d\n", free_status);
    }
    fprintf(stderr, "blockchain lib: known addresses, count=%zu sample:%s\n",
            out.size(), out.empty() ? "(none)" : out.front().c_str());
    return out;
}

std::string LogosBlockchainModule::leader_claim() {
    if (!node) {
        return "Error: The node is not running.";
    }

    auto [value, error] = ::leader_claim(node);
    if (!is_ok(&error)) {
        return "Error: Failed to claim leader rewards: " + std::to_string(error);
    }

    return bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), TX_HASH_BYTES);
}

// Blend

std::string LogosBlockchainModule::blend_join_as_core_node(
    const std::string& provider_id_hex,
    const std::string& zk_id_hex,
    const std::string& locked_note_id_hex,
    const std::vector<std::string>& locators
) {
    if (!node) {
        return "Error: The node is not running.";
    }

    const std::vector<uint8_t> provider_id_bytes = parse_address_hex(provider_id_hex);
    if (provider_id_bytes.empty() || static_cast<int>(provider_id_bytes.size()) != ADDRESS_BYTES) {
        return "Error: Invalid provider_id_hex (64 hex characters required).";
    }

    const std::vector<uint8_t> zk_id_bytes = parse_address_hex(zk_id_hex);
    if (zk_id_bytes.empty() || static_cast<int>(zk_id_bytes.size()) != ADDRESS_BYTES) {
        return "Error: Invalid zk_id_hex (64 hex characters required).";
    }

    const std::vector<uint8_t> locked_note_id_bytes = parse_address_hex(locked_note_id_hex);
    if (locked_note_id_bytes.empty() || static_cast<int>(locked_note_id_bytes.size()) != ADDRESS_BYTES) {
        return "Error: Invalid locked_note_id_hex (64 hex characters required).";
    }

    // locators_ptrs holds raw pointers into the std::strings (valid as long as locators lives).
    std::vector<const char*> locators_ptrs;
    locators_ptrs.reserve(locators.size());
    for (const std::string& locator : locators) {
        locators_ptrs.push_back(locator.c_str());
    }

    auto [value, error] = ::blend_join_as_core_node(
        node,
        provider_id_bytes.data(),
        zk_id_bytes.data(),
        locked_note_id_bytes.data(),
        locators_ptrs.data(),
        locators_ptrs.size()
    );
    if (!is_ok(&error)) {
        return "Error: Failed to join as core node: " + std::to_string(error);
    }

    std::string declaration_id = bytes_to_hex(reinterpret_cast<const uint8_t*>(&value), sizeof(value));
    fprintf(stderr, "Successfully joined as core node. DeclarationId: %s\n", declaration_id.c_str());
    return declaration_id;
}

// Explorer

std::string LogosBlockchainModule::get_block(const std::string& header_id_hex) {
    if (!node) {
        return "Error: The node is not running.";
    }

    const std::vector<uint8_t> bytes = parse_address_hex(header_id_hex);
    if (bytes.empty() || static_cast<int>(bytes.size()) != ADDRESS_BYTES) {
        return "Error: Header ID must be 64 hex characters (32 bytes).";
    }

    auto [value, error] = ::get_block(node, reinterpret_cast<const HeaderId*>(bytes.data()));
    if (!is_ok(&error)) {
        fprintf(stderr, "Failed to get block. Error: %d\n", error);
        return "Error: Failed to get block: " + std::to_string(error);
    }

    std::string result(value);
    const OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free block string. Error: %d\n", free_status);
    }
    return result;
}

std::string LogosBlockchainModule::get_blocks(const uint64_t from_slot, const uint64_t to_slot) {
    if (!node) {
        return "Error: The node is not running.";
    }

    auto [value, error] = ::get_blocks(node, from_slot, to_slot);
    if (!is_ok(&error)) {
        fprintf(stderr, "Failed to get blocks. Error: %d\n", error);
        return "Error: Failed to get blocks: " + std::to_string(error);
    }

    std::string result(value);
    const OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free blocks string. Error: %d\n", free_status);
    }
    return result;
}

std::string LogosBlockchainModule::get_transaction(const std::string& tx_hash_hex) {
    if (!node) {
        return "Error: The node is not running.";
    }

    const std::vector<uint8_t> bytes = parse_address_hex(tx_hash_hex);
    if (bytes.empty() || static_cast<int>(bytes.size()) != ADDRESS_BYTES) {
        return "Error: Transaction hash must be 64 hex characters (32 bytes).";
    }

    auto [value, error] = ::get_transaction(node, reinterpret_cast<const TxHash*>(bytes.data()));
    if (!is_ok(&error)) {
        fprintf(stderr, "Failed to get transaction. Error: %d\n", error);
        return "Error: Failed to get transaction: " + std::to_string(error);
    }

    std::string result(value);
    const OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free transaction string. Error: %d\n", free_status);
    }
    return result;
}

// Cryptarchia

std::string LogosBlockchainModule::get_cryptarchia_info() {
    if (!node) {
        return "Error: The node is not running.";
    }

    auto [value, error] = ::get_cryptarchia_info(node);
    if (!is_ok(&error)) {
        fprintf(stderr, "Failed to get cryptarchia info. Error: %d\n", error);
        return "Error: Failed to get cryptarchia info: " + std::to_string(error);
    }

    json obj;
    obj["lib"] = bytes_to_hex(reinterpret_cast<const uint8_t*>(value->lib), ADDRESS_BYTES);
    obj["tip"] = bytes_to_hex(reinterpret_cast<const uint8_t*>(value->tip), ADDRESS_BYTES);
    obj["slot"] = static_cast<int64_t>(value->slot);
    obj["height"] = static_cast<int64_t>(value->height);
    obj["mode"] = (value->mode == State::Online) ? "Online" : "Bootstrapping";

    const OperationStatus free_status = free_cryptarchia_info(value);
    if (!is_ok(&free_status)) {
        fprintf(stderr, "Failed to free cryptarchia info. Error: %d\n", free_status);
    }
    return obj.dump();
}

