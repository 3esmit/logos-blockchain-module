// Stub header for logos_blockchain — provides the same declarations as the real
// Rust-generated header so that logos_blockchain_module sources compile in tests.

#ifndef LOGOS_BLOCKCHAIN_H
#define LOGOS_BLOCKCHAIN_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 32-byte hash/address types
typedef uint8_t Hash[32];
typedef uint8_t HeaderId[32];
typedef uint8_t TxHash[32];
typedef Hash NoteId;

// Opaque node handle
typedef struct LogosBlockchainNode LogosBlockchainNode;

// Operation status code (0 = OK)
typedef enum OperationStatusCode {
    OperationStatusCode_Ok = 0,
    OperationStatusCode_NotFound = 1,
    OperationStatusCode_NullPointer = 2,
    OperationStatusCode_RelayError = 3,
    OperationStatusCode_ChannelSendError = 4,
    OperationStatusCode_ChannelReceiveError = 5,
    OperationStatusCode_ServiceError = 6,
    OperationStatusCode_RuntimeError = 7,
    OperationStatusCode_DynError = 8,
    OperationStatusCode_InitializationError = 9,
    OperationStatusCode_StopError = 10,
    OperationStatusCode_ConfigurationError = 11,
    OperationStatusCode_ValidationError = 12,
} OperationStatusCode;

// Operation status: code + Rust-allocated error message (free with free_cstring).
typedef struct {
    OperationStatusCode code;
    char* message;
} OperationStatus;

// Consensus state enum
typedef enum { Bootstrapping, Online, NotStarted } State;

// Key type for generate_key / add_key
typedef enum { Ed25519, Zk } KeyType;

// Arguments for generate_user_config
typedef struct {
    const char** initial_peers;
    const uint32_t* initial_peers_count;
    const char* output;
    const uint16_t* net_port;
    const uint16_t* blend_port;
    const char* http_addr;
    const char* external_address;
    const char* state_path;
    const char* storage_path;
    const char* logs_path;
    const bool* skip_ibd;
    const char* log_filter;
    const char* kms_file;
} GenerateConfigArgs;

// Arguments for transfer_funds
typedef struct {
    const HeaderId* optional_tip;
    const uint8_t* change_public_key;
    const uint8_t* const* funding_public_keys;
    size_t funding_public_keys_len;
    const uint8_t* recipient_public_key;
    uint64_t amount;
} TransferFundsArguments;

// Arguments for channel_deposit (amount-based)
typedef struct {
    const HeaderId* optional_tip;
    const uint8_t* channel_id;
    const uint8_t* funding_public_key;
    uint64_t amount;
    const uint8_t* metadata;
    size_t metadata_len;
} ChannelDepositArguments;

// Arguments for channel_deposit_with_notes (note-based)
typedef struct {
    const HeaderId* optional_tip;
    const uint8_t* channel_id;
    const NoteId* input_note_ids;
    size_t input_note_ids_len;
    const uint8_t* metadata;
    size_t metadata_len;
    const uint8_t* change_public_key;
    const uint8_t* const* funding_public_keys;
    size_t funding_public_keys_len;
    uint64_t max_tx_fee;
} ChannelDepositWithNotesArguments;

// Known addresses result container
typedef struct {
    uint8_t** addresses;
    size_t len;
} KnownAddresses;

typedef struct {
    Hash commitment;
    Hash nullifier;
} ClaimableVoucher;

typedef struct {
    HeaderId tip;
    ClaimableVoucher* vouchers;
    size_t len;
    uint64_t reward_amount;
    uint64_t total_claimable;
} ClaimableVouchers;

// A single spendable wallet note (UTXO): its note ID and value.
typedef struct {
    NoteId id;
    uint64_t value;
} WalletNote;

// The set of spendable notes for a wallet address at a given tip.
typedef struct {
    HeaderId tip;
    WalletNote* notes;
    size_t len;
} WalletNotes;

// A wallet note old enough to take part in the leadership lottery.
typedef struct {
    NoteId id;
    uint64_t value;
    uint8_t public_key[32];
} LeaderAgedNote;

// The wallet's notes eligible to lead at a given tip.
typedef struct {
    HeaderId tip;
    LeaderAgedNote* notes;
    size_t len;
    uint64_t total_value;
} LeaderAgedNotes;

// Cryptarchia consensus info
typedef struct {
    uint8_t lib[32];
    uint64_t lib_slot;
    uint8_t tip[32];
    uint64_t slot;
    uint64_t height;
    State mode;
} CryptarchiaInfo;

// Time service info
typedef struct {
    size_t claimable_tickets;
    uint64_t* slots_until_expiry;
    size_t len;
} PoWClaimableRewards;

typedef struct {
    uint64_t slot_duration_ms;
    int64_t genesis_time_unix_ms;
    uint64_t current_slot;
    uint32_t current_epoch;
} TimeInfo;

// libp2p connectivity counters (plain values, no free call needed)
typedef struct {
    size_t n_peers;
    uint32_t n_connections;
    uint32_t n_pending_connections;
    size_t n_discovered_peers;
} NetworkInfo;

// Result types (C++ structured bindings decompose these)
typedef struct { LogosBlockchainNode* value; OperationStatus error; } NodeResult;
typedef struct { uint64_t value; OperationStatus error; } BalanceResult;
typedef struct { Hash value; OperationStatus error; } TransferHashResult;
typedef struct { TxHash value; OperationStatus error; } FfiLeaderClaimResult;
typedef struct { Hash value; OperationStatus error; } FfiChannelDepositResult;
typedef struct { KnownAddresses value; OperationStatus error; } KnownAddressesResult;
typedef struct { WalletNotes value; OperationStatus error; } FfiWalletNotesResult;
typedef struct { LeaderAgedNotes value; OperationStatus error; } FfiLeaderAgedNotesResult;
typedef struct { ClaimableVouchers value; OperationStatus error; } FfiClaimableVouchersResult;
typedef struct { Hash value; OperationStatus error; } BlendHashResult;
typedef struct { char* value; OperationStatus error; } StringResult;
typedef struct { CryptarchiaInfo* value; OperationStatus error; } CryptarchiaInfoResult;
typedef struct { TimeInfo* value; OperationStatus error; } TimeInfoResult;
typedef struct { Hash value; OperationStatus error; } SubmitTransactionResult;
typedef struct { Hash value; OperationStatus error; } FfiPoWClaimResult;
typedef struct { PoWClaimableRewards value; OperationStatus error; } FfiPoWClaimableRewardsResult;
typedef struct { char* value; OperationStatus error; } FfiGetChainIdResult;
typedef struct { NetworkInfo value; OperationStatus error; } FfiNetworkInfoResult;

// Block event callback
typedef void (*BlockCallback)(const char* block_json);

// Status check
bool is_ok(const OperationStatus* status);

// Lifecycle
OperationStatus generate_user_config(GenerateConfigArgs args);
NodeResult start_lb_node(const char* config_path, const char* deployment);
OperationStatus shutdown_node(LogosBlockchainNode* node);
OperationStatus subscribe_to_new_blocks(LogosBlockchainNode* node, BlockCallback callback);
// Streams: each event is a JSON C string; the callback is invoked exactly once
// with NULL when the stream ends.
OperationStatus subscribe_to_processed_blocks(LogosBlockchainNode* node, BlockCallback callback);
OperationStatus subscribe_to_lib_blocks(LogosBlockchainNode* node, BlockCallback callback);

// Config management
OperationStatus update_user_config(const char* user_config_path, const char* keystore_path);
OperationStatus migrate_user_config(const char* output_path, const char* keystore_path);
OperationStatus migrate_user_config_0_1_2(
    const char* new_config_path,
    const char* old_config_path,
    const char* keystore_path);
OperationStatus participate(
    const char* config_path,
    const char* keystore_path,
    const char* output_dir,
    const char* external_address);

// Keystore
StringResult generate_key(
    const char* user_config_path,
    const char* keystore_path,
    KeyType key_type,
    const char* key_title);
OperationStatus add_key(
    const char* user_config_path,
    const char* keystore_path,
    KeyType key_type,
    const char* key_hex,
    const char* key_title);
OperationStatus remove_key(
    const char* user_config_path,
    const char* keystore_path,
    const char* key_title);

// Identity
StringResult get_peer_id(const char* config_path);

// Wallet
BalanceResult get_balance(LogosBlockchainNode* node, const uint8_t* address, const void* reserved);
TransferHashResult transfer_funds(LogosBlockchainNode* node, const TransferFundsArguments* args);
FfiLeaderClaimResult leader_claim(LogosBlockchainNode* node);
KnownAddressesResult get_known_addresses(LogosBlockchainNode* node);
OperationStatus free_known_addresses(KnownAddresses addrs);
FfiWalletNotesResult get_wallet_notes(
    LogosBlockchainNode* node,
    const uint8_t* wallet_address,
    const HeaderId* optional_tip);
OperationStatus free_wallet_notes(WalletNotes notes);
FfiLeaderAgedNotesResult get_leader_aged_notes(
    const LogosBlockchainNode* node,
    const HeaderId* optional_tip);
OperationStatus free_leader_aged_notes(LeaderAgedNotes notes);
StringResult wallet_fund_tx(LogosBlockchainNode* node, const char* request_json);

// Transactions
SubmitTransactionResult submit_signed_transaction(LogosBlockchainNode* node, const char* signed_tx_json);

// Channel
FfiChannelDepositResult channel_deposit(LogosBlockchainNode* node, const ChannelDepositArguments* arguments);
FfiChannelDepositResult channel_deposit_with_notes(
    LogosBlockchainNode* node,
    const ChannelDepositWithNotesArguments* arguments);
FfiClaimableVouchersResult get_claimable_vouchers(LogosBlockchainNode* node, const HeaderId* optional_tip);
OperationStatus free_claimable_vouchers(ClaimableVouchers vouchers);
StringResult get_channel_state(LogosBlockchainNode* node, const uint8_t* channel_id);

// Blend
BlendHashResult blend_join_as_core_node(
    LogosBlockchainNode* node,
    const char* locator,
    const uint8_t* locked_note_id);
StringResult blend_info(LogosBlockchainNode* node);

// Chain
FfiGetChainIdResult get_chain_id(const LogosBlockchainNode* node);

// Network
FfiNetworkInfoResult get_network_info(const LogosBlockchainNode* node);

// Explorer
StringResult get_block(LogosBlockchainNode* node, const HeaderId* header_id);
StringResult get_blocks(LogosBlockchainNode* node, uint64_t from_slot, uint64_t to_slot);
StringResult get_transaction(LogosBlockchainNode* node, const TxHash* tx_hash);

// Cryptarchia
CryptarchiaInfoResult get_cryptarchia_info(LogosBlockchainNode* node);
OperationStatus free_cryptarchia_info(CryptarchiaInfo* info);
StringResult get_block_events(LogosBlockchainNode* node, const HeaderId* header_id);

// Time
TimeInfoResult get_time_info(LogosBlockchainNode* node);
OperationStatus free_time_info(TimeInfo* info);

// Memory management
OperationStatus pow_start_mining(LogosBlockchainNode* node);
OperationStatus pow_stop_mining(LogosBlockchainNode* node);
OperationStatus pow_start_auto_claim(LogosBlockchainNode* node);
OperationStatus pow_stop_auto_claim(LogosBlockchainNode* node);
FfiPoWClaimResult pow_claim(LogosBlockchainNode* node, const uint8_t* claim_address);
FfiPoWClaimableRewardsResult pow_claimable_rewards(LogosBlockchainNode* node);
OperationStatus free_pow_claimable_rewards(PoWClaimableRewards rewards);

OperationStatus free_cstring(char* s);

#ifdef __cplusplus
}
#endif

#endif // LOGOS_BLOCKCHAIN_H
