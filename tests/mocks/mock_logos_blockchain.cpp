// Mock implementation of logos_blockchain C functions.
// Replaces the real Rust library at link time during unit tests.
// Return values are controlled via LogosCMockStore.

#include <logos_clib_mock.h>
#include <logos_blockchain.h>
#include <cstring>
#include <cstdlib>

static char s_fakeNode = 0;
static CryptarchiaInfo s_fakeCryptarchiaInfo = {};

// Known-address mock storage (up to 4 addresses)
static uint8_t s_mockAddr0[32];
static uint8_t s_mockAddr1[32];
static uint8_t s_mockAddr2[32];
static uint8_t s_mockAddr3[32];
static uint8_t* s_mockAddrs[] = { s_mockAddr0, s_mockAddr1, s_mockAddr2, s_mockAddr3 };

extern "C" {

bool is_ok(const OperationStatus* status) {
    return status && *status == 0;
}

OperationStatus generate_user_config(GenerateConfigArgs args) {
    LOGOS_CMOCK_RECORD("generate_user_config");
    return LOGOS_CMOCK_RETURN(int, "generate_user_config");
}

NodeResult start_lb_node(const char* config_path, const char* deployment) {
    LOGOS_CMOCK_RECORD("start_lb_node");
    int ok = LOGOS_CMOCK_RETURN(int, "start_lb_node");
    NodeResult result;
    result.value = ok ? reinterpret_cast<LogosBlockchainNode*>(&s_fakeNode) : nullptr;
    result.error = ok ? 0 : 1;
    return result;
}

OperationStatus stop_node(LogosBlockchainNode* node) {
    LOGOS_CMOCK_RECORD("stop_node");
    return 0;
}

OperationStatus update_user_config(const char* user_config_path, const char* keystore_path) {
    LOGOS_CMOCK_RECORD("update_user_config");
    return LOGOS_CMOCK_RETURN(int, "update_user_config");
}

OperationStatus migrate_user_config(const char* output_path, const char* keystore_path) {
    LOGOS_CMOCK_RECORD("migrate_user_config");
    return LOGOS_CMOCK_RETURN(int, "migrate_user_config");
}

OperationStatus migrate_user_config_0_1_2(
    const char* new_config_path,
    const char* old_config_path,
    const char* keystore_path)
{
    LOGOS_CMOCK_RECORD("migrate_user_config_0_1_2");
    return LOGOS_CMOCK_RETURN(int, "migrate_user_config_0_1_2");
}

OperationStatus participate(
    const char* config_path,
    const char* keystore_path,
    const char* output_dir,
    const char* external_address)
{
    LOGOS_CMOCK_RECORD("participate");
    return LOGOS_CMOCK_RETURN(int, "participate");
}

StringResult generate_key(
    const char* user_config_path,
    const char* keystore_path,
    KeyType key_type,
    const char* key_title)
{
    LOGOS_CMOCK_RECORD("generate_key");
    StringResult result;
    const char* id = LOGOS_CMOCK_RETURN_STRING("generate_key");
    result.value = id ? strdup(id) : nullptr;
    result.error = LOGOS_CMOCK_RETURN(int, "generate_key_error");
    return result;
}

OperationStatus add_key(
    const char* user_config_path,
    const char* keystore_path,
    KeyType key_type,
    const char* key_hex,
    const char* key_title)
{
    LOGOS_CMOCK_RECORD("add_key");
    return LOGOS_CMOCK_RETURN(int, "add_key");
}

OperationStatus remove_key(
    const char* user_config_path,
    const char* keystore_path,
    const char* key_title)
{
    LOGOS_CMOCK_RECORD("remove_key");
    return LOGOS_CMOCK_RETURN(int, "remove_key");
}

StringResult get_peer_id(const char* config_path) {
    LOGOS_CMOCK_RECORD("get_peer_id");
    StringResult result;
    const char* id = LOGOS_CMOCK_RETURN_STRING("get_peer_id");
    result.value = id ? strdup(id) : nullptr;
    result.error = LOGOS_CMOCK_RETURN(int, "get_peer_id_error");
    return result;
}

OperationStatus subscribe_to_new_blocks(LogosBlockchainNode* node, BlockCallback callback) {
    LOGOS_CMOCK_RECORD("subscribe_to_new_blocks");
    return LOGOS_CMOCK_RETURN(int, "subscribe_to_new_blocks");
}

BalanceResult get_balance(LogosBlockchainNode* node, const uint8_t* address, const void* reserved) {
    LOGOS_CMOCK_RECORD("get_balance");
    BalanceResult result;
    result.value = static_cast<uint64_t>(LOGOS_CMOCK_RETURN(int, "get_balance_value"));
    result.error = LOGOS_CMOCK_RETURN(int, "get_balance_error");
    return result;
}

TransferHashResult transfer_funds(LogosBlockchainNode* node, const TransferFundsArguments* args) {
    LOGOS_CMOCK_RECORD("transfer_funds");
    TransferHashResult result;
    memset(result.value, 0xAB, sizeof(Hash));
    result.error = LOGOS_CMOCK_RETURN(int, "transfer_funds_error");
    return result;
}

KnownAddressesResult get_known_addresses(LogosBlockchainNode* node) {
    LOGOS_CMOCK_RECORD("get_known_addresses");
    KnownAddressesResult result;
    int err = LOGOS_CMOCK_RETURN(int, "get_known_addresses_error");
    result.error = err;
    if (err == 0) {
        int count = LOGOS_CMOCK_RETURN(int, "get_known_addresses_count");
        if (count > 4) count = 4;
        memset(s_mockAddr0, 0x11, 32);
        memset(s_mockAddr1, 0x22, 32);
        memset(s_mockAddr2, 0x33, 32);
        memset(s_mockAddr3, 0x44, 32);
        result.value.addresses = s_mockAddrs;
        result.value.len = static_cast<size_t>(count);
    } else {
        result.value.addresses = nullptr;
        result.value.len = 0;
    }
    return result;
}

OperationStatus free_known_addresses(KnownAddresses addrs) {
    LOGOS_CMOCK_RECORD("free_known_addresses");
    return 0;
}

BlendHashResult blend_join_as_core_node(
    LogosBlockchainNode* node,
    const uint8_t* provider_id,
    const uint8_t* zk_id,
    const uint8_t* locked_note_id,
    const char** locators,
    size_t locators_count)
{
    LOGOS_CMOCK_RECORD("blend_join_as_core_node");
    BlendHashResult result;
    memset(result.value, 0xCD, sizeof(Hash));
    result.error = LOGOS_CMOCK_RETURN(int, "blend_join_as_core_node_error");
    return result;
}

StringResult get_block(LogosBlockchainNode* node, const HeaderId* header_id) {
    LOGOS_CMOCK_RECORD("get_block");
    StringResult result;
    const char* json = LOGOS_CMOCK_RETURN_STRING("get_block");
    result.value = json ? strdup(json) : nullptr;
    result.error = LOGOS_CMOCK_RETURN(int, "get_block_error");
    return result;
}

StringResult get_blocks(LogosBlockchainNode* node, uint64_t from_slot, uint64_t to_slot) {
    LOGOS_CMOCK_RECORD("get_blocks");
    StringResult result;
    const char* json = LOGOS_CMOCK_RETURN_STRING("get_blocks");
    result.value = json ? strdup(json) : nullptr;
    result.error = LOGOS_CMOCK_RETURN(int, "get_blocks_error");
    return result;
}

StringResult get_transaction(LogosBlockchainNode* node, const TxHash* tx_hash) {
    LOGOS_CMOCK_RECORD("get_transaction");
    StringResult result;
    const char* json = LOGOS_CMOCK_RETURN_STRING("get_transaction");
    result.value = json ? strdup(json) : nullptr;
    result.error = LOGOS_CMOCK_RETURN(int, "get_transaction_error");
    return result;
}

CryptarchiaInfoResult get_cryptarchia_info(LogosBlockchainNode* node) {
    LOGOS_CMOCK_RECORD("get_cryptarchia_info");
    CryptarchiaInfoResult result;
    memset(&s_fakeCryptarchiaInfo, 0, sizeof(s_fakeCryptarchiaInfo));
    s_fakeCryptarchiaInfo.slot = static_cast<uint64_t>(LOGOS_CMOCK_RETURN(int, "cryptarchia_slot"));
    s_fakeCryptarchiaInfo.height = static_cast<uint64_t>(LOGOS_CMOCK_RETURN(int, "cryptarchia_height"));
    s_fakeCryptarchiaInfo.mode = static_cast<State>(LOGOS_CMOCK_RETURN(int, "cryptarchia_mode"));
    memset(s_fakeCryptarchiaInfo.lib, 0xEE, 32);
    memset(s_fakeCryptarchiaInfo.tip, 0xFF, 32);
    result.value = &s_fakeCryptarchiaInfo;
    result.error = LOGOS_CMOCK_RETURN(int, "get_cryptarchia_info_error");
    return result;
}

OperationStatus free_cryptarchia_info(CryptarchiaInfo* info) {
    LOGOS_CMOCK_RECORD("free_cryptarchia_info");
    return 0;
}

OperationStatus free_cstring(char* s) {
    LOGOS_CMOCK_RECORD("free_cstring");
    free(s);
    return 0;
}

} // extern "C"
