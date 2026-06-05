#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif
#include <logos_blockchain.h>
#ifdef __cplusplus
}
#endif

class LogosBlockchainModule {
public:
    LogosBlockchainModule();
    ~LogosBlockchainModule();

    // Wired automatically by the generated glue layer.
    // Call this to emit named events to other modules / the host application.
    // Data is a JSON-encoded string (object or array).
    std::function<void(const std::string& eventName, const std::string& data)> emitEvent;

    // ---- Node ----

    // Lifecycle
    int generate_user_config(const std::string& json_args);
    int start(const std::string& config_path, const std::string& deployment);
    int stop();

    // Wallet
    std::string wallet_get_balance(const std::string& address_hex);
    std::string wallet_transfer_funds(
        const std::string& change_public_key,
        const std::vector<std::string>& sender_addresses,
        const std::string& recipient_address,
        const std::string& amount,
        const std::string& optional_tip_hex
    );
    std::vector<std::string> wallet_get_known_addresses();

    // Blend
    std::string blend_join_as_core_node(
        const std::string& provider_id_hex,
        const std::string& zk_id_hex,
        const std::string& locked_note_id_hex,
        const std::vector<std::string>& locators
    );

    // Explorer
    std::string get_block(const std::string& header_id_hex);
    std::string get_blocks(uint64_t from_slot, uint64_t to_slot);
    std::string get_transaction(const std::string& tx_hash_hex);

    // Cryptarchia
    std::string get_cryptarchia_info();

private:
    LogosBlockchainNode* node = nullptr;

    // Static instance for C callback (C API doesn't support user data)
    static LogosBlockchainModule* s_instance;

    // C-compatible callback function
    static void on_new_block_callback(const char* block);
};
