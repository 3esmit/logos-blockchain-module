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

// Opaque node handle
typedef struct LogosBlockchainNode LogosBlockchainNode;

// Operation status (0 = OK)
typedef int OperationStatus;

// Deployment enums
typedef enum { WellKnown, Custom } DeploymentType;
typedef enum { Devnet } WellKnownDeployment;

// Consensus state enum
typedef enum { Bootstrapping, Online } State;

// Deployment configuration
typedef struct {
    DeploymentType deployment_type;
    WellKnownDeployment well_known_deployment;
    const char* custom_deployment_config_path;
} Deployment;

// Arguments for generate_user_config
typedef struct {
    const char** initial_peers;
    const uint32_t* initial_peers_count;
    const char* output;
    const uint16_t* net_port;
    const uint16_t* blend_port;
    const char* http_addr;
    const char* external_address;
    const bool* no_public_ip_check;
    const Deployment* deployment;
    const char* state_path;
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

// Known addresses result container
typedef struct {
    uint8_t** addresses;
    size_t len;
} KnownAddresses;

// Cryptarchia consensus info
typedef struct {
    uint8_t lib[32];
    uint8_t tip[32];
    uint64_t slot;
    uint64_t height;
    State mode;
} CryptarchiaInfo;

// Result types (C++ structured bindings decompose these)
typedef struct { LogosBlockchainNode* value; OperationStatus error; } NodeResult;
typedef struct { uint64_t value; OperationStatus error; } BalanceResult;
typedef struct { Hash value; OperationStatus error; } TransferHashResult;
typedef struct { KnownAddresses value; OperationStatus error; } KnownAddressesResult;
typedef struct { Hash value; OperationStatus error; } BlendHashResult;
typedef struct { char* value; OperationStatus error; } StringResult;
typedef struct { CryptarchiaInfo* value; OperationStatus error; } CryptarchiaInfoResult;

// Block event callback
typedef void (*BlockCallback)(const char* block_json);

// Status check
bool is_ok(const OperationStatus* status);

// Lifecycle
OperationStatus generate_user_config(GenerateConfigArgs args);
NodeResult start_lb_node(const char* config_path, const char* deployment);
OperationStatus stop_node(LogosBlockchainNode* node);
OperationStatus subscribe_to_new_blocks(LogosBlockchainNode* node, BlockCallback callback);

// Wallet
BalanceResult get_balance(LogosBlockchainNode* node, const uint8_t* address, const void* reserved);
TransferHashResult transfer_funds(LogosBlockchainNode* node, const TransferFundsArguments* args);
KnownAddressesResult get_known_addresses(LogosBlockchainNode* node);
OperationStatus free_known_addresses(KnownAddresses addrs);

// Blend
BlendHashResult blend_join_as_core_node(
    LogosBlockchainNode* node,
    const uint8_t* provider_id,
    const uint8_t* zk_id,
    const uint8_t* locked_note_id,
    const char** locators,
    size_t locators_count);

// Explorer
StringResult get_block(LogosBlockchainNode* node, const HeaderId* header_id);
StringResult get_blocks(LogosBlockchainNode* node, uint64_t from_slot, uint64_t to_slot);
StringResult get_transaction(LogosBlockchainNode* node, const TxHash* tx_hash);

// Cryptarchia
CryptarchiaInfoResult get_cryptarchia_info(LogosBlockchainNode* node);
OperationStatus free_cryptarchia_info(CryptarchiaInfo* info);

// Memory management
OperationStatus free_cstring(char* s);

#ifdef __cplusplus
}
#endif

#endif // LOGOS_BLOCKCHAIN_H
