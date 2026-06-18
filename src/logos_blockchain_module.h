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
    std::string generate_user_config(const std::string& json_args);
    std::string start(const std::string& config_path, const std::string& deployment);
    std::string stop();

    // Config management
    std::string update_user_config(const std::string& user_config_path, const std::string& keystore_path);
    std::string migrate_user_config(const std::string& output_path, const std::string& keystore_path);
    std::string migrate_user_config_0_1_2(
        const std::string& new_config_path,
        const std::string& old_config_path,
        const std::string& keystore_path
    );
    std::string participate(
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
    std::string add_key(
        const std::string& user_config_path,
        const std::string& keystore_path,
        const std::string& key_type,
        const std::string& key_hex,
        const std::string& key_title
    );
    std::string remove_key(
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
    // Spendable notes (UTXOs) of a wallet address, as a JSON string:
    //   { "tip": "<hex>", "notes": [ { "id": "<hex>", "value": "<u64>" }, ... ] }
    // optional_tip_hex may be empty to query at the current tip. Note IDs round-trip
    // into channel_deposit_with_notes.
    std::string wallet_get_notes(
        const std::string& wallet_address_hex,
        const std::string& optional_tip_hex
    );
    std::string leader_claim();

    // Channel
    // Amount-based deposit: the binding selects funding notes itself (splitting a
    // note via a transfer when no exact-value note exists) so the channel receives
    // exactly `amount`. funding_public_key owns the funding notes, the deposit note
    // and any change. metadata_hex may be empty; optional_tip_hex may be empty to
    // build against the current tip. Returns the transaction hash hex on success.
    std::string channel_deposit(
        const std::string& channel_id_hex,
        const std::string& funding_public_key_hex,
        const std::string& amount,
        const std::string& metadata_hex,
        const std::string& optional_tip_hex
    );
    // Note-based deposit: the caller supplies the exact notes to consume (their
    // whole value enters the channel), so amount = sum of the notes' values. Use
    // wallet_get_notes to obtain note IDs. The gas fee is funded from
    // funding_public_keys (change to change_public_key), capped at max_tx_fee.
    // metadata_hex / optional_tip_hex may be empty. Returns the tx hash hex.
    std::string channel_deposit_with_notes(
        const std::string& channel_id_hex,
        const std::vector<std::string>& input_note_id_hexes,
        const std::string& metadata_hex,
        const std::string& change_public_key_hex,
        const std::vector<std::string>& funding_public_key_hexes,
        const std::string& max_tx_fee,
        const std::string& optional_tip_hex
    );

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
