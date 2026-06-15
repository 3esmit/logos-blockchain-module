#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <logos_module_context.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <logos_blockchain.h>
#ifdef __cplusplus
}
#endif

class LogosBlockchainModule : public LogosModuleContext {
public:
    LogosBlockchainModule();
    ~LogosBlockchainModule();

    // ---- Node ----

    // Lifecycle
    int generate_user_config(const std::string& json_args);
    int start(const std::string& config_path, const std::string& deployment);
    int stop();

    // Config management
    int update_user_config(const std::string& user_config_path, const std::string& keystore_path);
    int migrate_user_config(const std::string& output_path, const std::string& keystore_path);
    int migrate_user_config_0_1_2(
        const std::string& new_config_path,
        const std::string& old_config_path,
        const std::string& keystore_path
    );
    int participate(
        const std::string& config_path,
        const std::string& keystore_path,
        const std::string& output_dir,
        const std::string& external_address
    );

    // Keystore
    // key_type is "ed25519" or "zk". key_title may be empty (auto-generated).
    std::string generate_key(
        const std::string& user_config_path,
        const std::string& keystore_path,
        const std::string& key_type,
        const std::string& key_title
    );
    int add_key(
        const std::string& user_config_path,
        const std::string& keystore_path,
        const std::string& key_type,
        const std::string& key_hex,
        const std::string& key_title
    );
    int remove_key(
        const std::string& user_config_path,
        const std::string& keystore_path,
        const std::string& key_title
    );

    // Identity
    std::string get_peer_id(const std::string& config_path);

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
    std::string leader_claim();

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

logos_events:
    // Fired by on_new_block_callback when the Rust node delivers a new block.
    // blockJson is the full block serialized as JSON.
    void newBlock(const std::string& blockJson);

private:
    LogosBlockchainNode* node = nullptr;

    // Static instance for C callback (C API doesn't support user data)
    static LogosBlockchainModule* s_instance;

    // C-compatible callback function
    static void on_new_block_callback(const char* block);
};
