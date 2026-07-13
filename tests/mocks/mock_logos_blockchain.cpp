// Mock implementation of logos_blockchain C functions.
// Replaces the real Rust library at link time during unit tests.
// Return values are controlled via LogosCMockStore.

#include <logos_clib_mock.h>
#include <logos_blockchain.h>
#include <cstring>
#include <cstdlib>
#include <string>

// Captures the paths passed to the most recent generate_user_config call so tests
// can assert the module's path routing. A null pointer is recorded as the
// sentinel "<null>".
std::string g_lastGeneratedOutput;
std::string g_lastGeneratedStatePath;
std::string g_lastGeneratedStoragePath;
std::string g_lastGeneratedLogsPath;

static char s_fakeNode = 0;
static CryptarchiaInfo s_fakeCryptarchiaInfo = {};

// Known-address mock storage (up to 4 addresses)
static uint8_t s_mockAddr0[32];
static uint8_t s_mockAddr1[32];
static uint8_t s_mockAddr2[32];
static uint8_t s_mockAddr3[32];
static uint8_t* s_mockAddrs[] = { s_mockAddr0, s_mockAddr1, s_mockAddr2, s_mockAddr3 };
static ClaimableVoucher s_mockClaimableVouchers[4];

static OperationStatus make_status(int code) {
    OperationStatus s;
    s.code = static_cast<OperationStatusCode>(code);
    s.message = code != 0 ? strdup("mock error") : nullptr;
    return s;
}

extern "C" {

bool is_ok(const OperationStatus* status) {
    return status && status->code == OperationStatusCode_Ok;
}

OperationStatus generate_user_config(GenerateConfigArgs args) {
    LOGOS_CMOCK_RECORD("generate_user_config");
    g_lastGeneratedOutput = args.output ? args.output : "<null>";
    g_lastGeneratedStatePath = args.state_path ? args.state_path : "<null>";
    g_lastGeneratedStoragePath = args.storage_path ? args.storage_path : "<null>";
    g_lastGeneratedLogsPath = args.logs_path ? args.logs_path : "<null>";
    return make_status(LOGOS_CMOCK_RETURN(int, "generate_user_config"));
}

NodeResult start_lb_node(const char* config_path, const char* deployment) {
    LOGOS_CMOCK_RECORD("start_lb_node");
    int ok = LOGOS_CMOCK_RETURN(int, "start_lb_node");
    NodeResult result;
    result.value = ok ? reinterpret_cast<LogosBlockchainNode*>(&s_fakeNode) : nullptr;
    result.error = make_status(ok ? 0 : 1);
    return result;
}

OperationStatus shutdown_node(LogosBlockchainNode* node) {
    LOGOS_CMOCK_RECORD("shutdown_node");
    return make_status(0);
}

OperationStatus update_user_config(const char* user_config_path, const char* keystore_path) {
    LOGOS_CMOCK_RECORD("update_user_config");
    return make_status(LOGOS_CMOCK_RETURN(int, "update_user_config"));
}

OperationStatus migrate_user_config(const char* output_path, const char* keystore_path) {
    LOGOS_CMOCK_RECORD("migrate_user_config");
    return make_status(LOGOS_CMOCK_RETURN(int, "migrate_user_config"));
}

OperationStatus migrate_user_config_0_1_2(
    const char* new_config_path,
    const char* old_config_path,
    const char* keystore_path)
{
    LOGOS_CMOCK_RECORD("migrate_user_config_0_1_2");
    return make_status(LOGOS_CMOCK_RETURN(int, "migrate_user_config_0_1_2"));
}

OperationStatus participate(
    const char* config_path,
    const char* keystore_path,
    const char* output_dir,
    const char* external_address)
{
    LOGOS_CMOCK_RECORD("participate");
    return make_status(LOGOS_CMOCK_RETURN(int, "participate"));
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
    result.error = make_status(LOGOS_CMOCK_RETURN(int, "generate_key_error"));
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
    return make_status(LOGOS_CMOCK_RETURN(int, "add_key"));
}

OperationStatus remove_key(
    const char* user_config_path,
    const char* keystore_path,
    const char* key_title)
{
    LOGOS_CMOCK_RECORD("remove_key");
    return make_status(LOGOS_CMOCK_RETURN(int, "remove_key"));
}

StringResult get_peer_id(const char* config_path) {
    LOGOS_CMOCK_RECORD("get_peer_id");
    StringResult result;
    const char* id = LOGOS_CMOCK_RETURN_STRING("get_peer_id");
    result.value = id ? strdup(id) : nullptr;
    result.error = make_status(LOGOS_CMOCK_RETURN(int, "get_peer_id_error"));
    return result;
}

OperationStatus subscribe_to_new_blocks(LogosBlockchainNode* node, BlockCallback callback) {
    LOGOS_CMOCK_RECORD("subscribe_to_new_blocks");
    return make_status(LOGOS_CMOCK_RETURN(int, "subscribe_to_new_blocks"));
}

BalanceResult get_balance(LogosBlockchainNode* node, const uint8_t* address, const void* reserved) {
    LOGOS_CMOCK_RECORD("get_balance");
    BalanceResult result;
    result.value = static_cast<uint64_t>(LOGOS_CMOCK_RETURN(int, "get_balance_value"));
    result.error = make_status(LOGOS_CMOCK_RETURN(int, "get_balance_error"));
    return result;
}

TransferHashResult transfer_funds(LogosBlockchainNode* node, const TransferFundsArguments* args) {
    LOGOS_CMOCK_RECORD("transfer_funds");
    TransferHashResult result;
    memset(result.value, 0xAB, sizeof(Hash));
    result.error = make_status(LOGOS_CMOCK_RETURN(int, "transfer_funds_error"));
    return result;
}

FfiLeaderClaimResult leader_claim(LogosBlockchainNode* node) {
    LOGOS_CMOCK_RECORD("leader_claim");
    FfiLeaderClaimResult result;
    memset(result.value, 0xEF, sizeof(TxHash));
    result.error = make_status(LOGOS_CMOCK_RETURN(int, "leader_claim_error"));
    return result;
}

KnownAddressesResult get_known_addresses(LogosBlockchainNode* node) {
    LOGOS_CMOCK_RECORD("get_known_addresses");
    KnownAddressesResult result;
    int err = LOGOS_CMOCK_RETURN(int, "get_known_addresses_error");
    result.error = make_status(err);
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
    return make_status(0);
}

FfiClaimableVouchersResult get_claimable_vouchers(LogosBlockchainNode* node, const HeaderId* optional_tip) {
    LOGOS_CMOCK_RECORD("get_claimable_vouchers");
    FfiClaimableVouchersResult result;
    int err = LOGOS_CMOCK_RETURN(int, "get_claimable_vouchers_error");
    result.error = make_status(err);
    if (err == 0) {
        int count = LOGOS_CMOCK_RETURN(int, "get_claimable_vouchers_count");
        if (count > 4) count = 4;
        memset(result.value.tip, 0xAA, sizeof(HeaderId));
        for (int i = 0; i < count; ++i) {
            memset(s_mockClaimableVouchers[i].commitment, 0x10 + i, sizeof(Hash));
            memset(s_mockClaimableVouchers[i].nullifier, 0x20 + i, sizeof(Hash));
        }
        result.value.vouchers = s_mockClaimableVouchers;
        result.value.len = static_cast<size_t>(count);
    } else {
        memset(result.value.tip, 0, sizeof(HeaderId));
        result.value.vouchers = nullptr;
        result.value.len = 0;
    }
    return result;
}

OperationStatus free_claimable_vouchers(ClaimableVouchers vouchers) {
    LOGOS_CMOCK_RECORD("free_claimable_vouchers");
    return make_status(0);
}

// Wallet-notes mock storage (up to 4 notes)
static WalletNote s_mockNotes[4];

FfiWalletNotesResult get_wallet_notes(
    LogosBlockchainNode* node,
    const uint8_t* wallet_address,
    const HeaderId* optional_tip)
{
    LOGOS_CMOCK_RECORD("get_wallet_notes");
    FfiWalletNotesResult result;
    memset(&result.value, 0, sizeof(WalletNotes));
    int err = LOGOS_CMOCK_RETURN(int, "get_wallet_notes_error");
    result.error = make_status(err);
    if (err == 0) {
        int count = LOGOS_CMOCK_RETURN(int, "get_wallet_notes_count");
        if (count > 4) count = 4;
        if (count < 0) count = 0;
        for (int i = 0; i < count; ++i) {
            memset(s_mockNotes[i].id, 0x10 + i, sizeof(NoteId));
            s_mockNotes[i].value = static_cast<uint64_t>(100 * (i + 1));
        }
        memset(result.value.tip, 0xFF, sizeof(HeaderId));
        result.value.notes = count > 0 ? s_mockNotes : nullptr;
        result.value.len = static_cast<size_t>(count);
    }
    return result;
}

OperationStatus free_wallet_notes(WalletNotes notes) {
    LOGOS_CMOCK_RECORD("free_wallet_notes");
    return make_status(0);
}

FfiChannelDepositResult channel_deposit(LogosBlockchainNode* node, const ChannelDepositArguments* arguments) {
    LOGOS_CMOCK_RECORD("channel_deposit");
    FfiChannelDepositResult result;
    memset(result.value, 0xBC, sizeof(Hash));
    result.error = make_status(LOGOS_CMOCK_RETURN(int, "channel_deposit_error"));
    return result;
}

FfiChannelDepositResult channel_deposit_with_notes(
    LogosBlockchainNode* node,
    const ChannelDepositWithNotesArguments* arguments)
{
    LOGOS_CMOCK_RECORD("channel_deposit_with_notes");
    FfiChannelDepositResult result;
    memset(result.value, 0xDE, sizeof(Hash));
    result.error = make_status(LOGOS_CMOCK_RETURN(int, "channel_deposit_with_notes_error"));
    return result;
}

BlendHashResult blend_join_as_core_node(
    LogosBlockchainNode* node,
    const char* locator,
    const uint8_t* locked_note_id)
{
    LOGOS_CMOCK_RECORD("blend_join_as_core_node");
    BlendHashResult result;
    memset(result.value, 0xCD, sizeof(Hash));
    result.error = make_status(LOGOS_CMOCK_RETURN(int, "blend_join_as_core_node_error"));
    return result;
}

StringResult get_block(LogosBlockchainNode* node, const HeaderId* header_id) {
    LOGOS_CMOCK_RECORD("get_block");
    StringResult result;
    const char* json = LOGOS_CMOCK_RETURN_STRING("get_block");
    result.value = json ? strdup(json) : nullptr;
    result.error = make_status(LOGOS_CMOCK_RETURN(int, "get_block_error"));
    return result;
}

StringResult get_blocks(LogosBlockchainNode* node, uint64_t from_slot, uint64_t to_slot) {
    LOGOS_CMOCK_RECORD("get_blocks");
    StringResult result;
    const char* json = LOGOS_CMOCK_RETURN_STRING("get_blocks");
    result.value = json ? strdup(json) : nullptr;
    result.error = make_status(LOGOS_CMOCK_RETURN(int, "get_blocks_error"));
    return result;
}

StringResult get_transaction(LogosBlockchainNode* node, const TxHash* tx_hash) {
    LOGOS_CMOCK_RECORD("get_transaction");
    StringResult result;
    const char* json = LOGOS_CMOCK_RETURN_STRING("get_transaction");
    result.value = json ? strdup(json) : nullptr;
    result.error = make_status(LOGOS_CMOCK_RETURN(int, "get_transaction_error"));
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
    result.error = make_status(LOGOS_CMOCK_RETURN(int, "get_cryptarchia_info_error"));
    return result;
}

OperationStatus free_cryptarchia_info(CryptarchiaInfo* info) {
    LOGOS_CMOCK_RECORD("free_cryptarchia_info");
    return make_status(0);
}

OperationStatus free_cstring(char* s) {
    LOGOS_CMOCK_RECORD("free_cstring");
    free(s);
    return make_status(0);
}

} // extern "C"
