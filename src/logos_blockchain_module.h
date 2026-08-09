#pragma once

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <logos_module_context.h>
#include <logos_result.h>

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
    ~LogosBlockchainModule() override;

    // ---- Node ----

    // Lifecycle
    [[nodiscard]] StdLogosResult start(const std::string& config_path, const std::string& deployment);
    [[nodiscard]] StdLogosResult stop();

    /// Return the versioned, bounded Bedrock node lifecycle snapshot.
    ///
    /// This is callable before initialization. It intentionally does not expose
    /// configuration paths, configuration contents, or raw backend errors.
    [[nodiscard]] std::string nodeStatus();

    /// Accept a versioned, caller-correlated node lifecycle command.
    ///
    /// The request uses the shared managed-node V1 JSON envelope. It returns a
    /// small acknowledgement; accepted and settled observations are emitted via
    /// nodeChanged(). Existing start(), stop(), and configuration APIs remain
    /// compatible and independent.
    [[nodiscard]] std::string nodeAction(const std::string& request);

    // Config management
    // Not static: when the JSON args set "use_persistence_paths": true it routes
    // the node's output/state/storage/logs paths under instancePersistencePath()
    // (config at <base>/<relative output>; state/db/logs at <base>/state, /db,
    // /logs), so it needs the instance context. Basecamp opts in via that flag;
    // logoscore-cli/standalone omit it and keep their own paths. An explicit
    // state/storage/logs path always wins; output is always re-anchored under the
    // base when flagged. On success the result value is the path the config was
    // written to — pass it straight to start().
    [[nodiscard]] StdLogosResult generate_user_config(const std::string& json_args) const;
    [[nodiscard]] static StdLogosResult update_user_config(
        const std::string& user_config_path,
        const std::string& keystore_path
    );
    [[nodiscard]] static StdLogosResult migrate_user_config(
        const std::string& output_path,
        const std::string& keystore_path
    );
    [[nodiscard]] static StdLogosResult migrate_user_config_0_1_2(
        const std::string& new_config_path,
        const std::string& old_config_path,
        const std::string& keystore_path
    );
    [[nodiscard]] static StdLogosResult participate(
        const std::string& config_path,
        const std::string& keystore_path,
        const std::string& output_dir,
        const std::string& external_address
    );

    // Keystore
    // key_type is "ed25519" or "zk". key_title may be empty (auto-generated).
    [[nodiscard]] static StdLogosResult generate_key(
        const std::string& user_config_path,
        const std::string& keystore_path,
        const std::string& key_type,
        const std::string& key_title
    );
    [[nodiscard]] static StdLogosResult add_key(
        const std::string& user_config_path,
        const std::string& keystore_path,
        const std::string& key_type,
        const std::string& key_hex,
        const std::string& key_title
    );
    [[nodiscard]] static StdLogosResult remove_key(
        const std::string& user_config_path,
        const std::string& keystore_path,
        const std::string& key_title
    );

    // Identity
    [[nodiscard]] static StdLogosResult get_peer_id(const std::string& config_path);

    // Wallet
    [[nodiscard]] StdLogosResult wallet_get_balance(const std::string& address_hex) const;
    [[nodiscard]] StdLogosResult wallet_transfer_funds(
        const std::string& change_public_key,
        const std::vector<std::string>& sender_addresses,
        const std::string& recipient_address,
        const std::string& amount,
        const std::string& optional_tip_hex
    ) const;
    [[nodiscard]] StdLogosResult wallet_get_known_addresses() const;
    // Spendable notes (UTXOs) of a wallet address, as a JSON string:
    //   { "tip": "<hex>", "notes": [ { "id": "<hex>", "value": "<u64>" }, ... ] }
    // optional_tip_hex may be empty to query at the current tip. Note IDs round-trip
    // into channel_deposit_with_notes.
    [[nodiscard]] StdLogosResult wallet_get_notes(
        const std::string& wallet_address_hex,
        const std::string& optional_tip_hex
    ) const;
    [[nodiscard]] StdLogosResult leader_claim() const;
    [[nodiscard]] StdLogosResult wallet_get_claimable_vouchers() const;

    // Channel
    // Amount-based deposit: the binding selects funding notes itself (splitting a
    // note via a transfer when no exact-value note exists) so the channel receives
    // exactly `amount`. funding_public_key owns the funding notes, the deposit note
    // and any change. metadata_hex may be empty; optional_tip_hex may be empty to
    // build against the current tip. Returns the transaction hash hex on success.
    [[nodiscard]] StdLogosResult channel_deposit(
        const std::string& channel_id_hex,
        const std::string& funding_public_key_hex,
        const std::string& amount,
        const std::string& metadata_hex,
        const std::string& optional_tip_hex
    ) const;
    // Note-based deposit: the caller supplies the exact notes to consume (their
    // whole value enters the channel), so amount = sum of the notes' values. Use
    // wallet_get_notes to obtain note IDs. The gas fee is funded from
    // funding_public_keys (change to change_public_key), capped at max_tx_fee.
    // metadata_hex / optional_tip_hex may be empty. Returns the tx hash hex.
    [[nodiscard]] StdLogosResult channel_deposit_with_notes(
        const std::string& channel_id_hex,
        const std::vector<std::string>& input_note_id_hexes,
        const std::string& metadata_hex,
        const std::string& change_public_key_hex,
        const std::vector<std::string>& funding_public_key_hexes,
        const std::string& max_tx_fee,
        const std::string& optional_tip_hex
    ) const;

    // Blend
    [[nodiscard]] StdLogosResult blend_join_as_core_node(
        const std::string& provider_id_hex,
        const std::string& zk_id_hex,
        const std::string& locked_note_id_hex,
        const std::vector<std::string>& locators
    ) const;

    // Explorer. Direct reads normalize known request identities into the
    // response: block.header.id and transaction.mantle_tx.hash when mantle_tx
    // is an object, otherwise transaction.hash. get_blocks uses a bounded
    // tip-parent walk when the C API's immutable-only range read is empty near
    // the live tip.
    [[nodiscard]] StdLogosResult get_block(const std::string& header_id_hex) const;
    [[nodiscard]] StdLogosResult get_blocks(uint64_t from_slot, uint64_t to_slot) const;
    [[nodiscard]] StdLogosResult get_transaction(const std::string& tx_hash_hex) const;

    // Catalog. These are direct projections of the matching logos-blockchain
    // C API calls so every module host can read the snapshot-safe catalog data.
    [[nodiscard]] StdLogosResult get_time_info() const;
    [[nodiscard]] StdLogosResult get_finalized_blocks_range(
        uint64_t from_slot,
        uint64_t to_slot,
        uint64_t blocks_limit
    ) const;

    // Bedrock diagnostics. These use the same typed C API reads as the
    // direct-node diagnostic endpoints, without forwarding HTTP requests.
    [[nodiscard]] StdLogosResult get_cryptarchia_info() const;
    [[nodiscard]] StdLogosResult get_cryptarchia_headers() const;
    [[nodiscard]] StdLogosResult get_network_info() const;
    [[nodiscard]] StdLogosResult get_mantle_metrics() const;

    // clang-format off
// Clang-format only handles public/private/protected, so it miss-indents this section.
// Guard kept until https://github.com/llvm/llvm-project/issues/64763 lands.
logos_events:
    /// Emits ordered versioned lifecycle observations for nodeStatus/nodeAction.
    void nodeChanged(const std::string& event);

    // Fired by on_new_block_callback when the Rust node delivers a new block.
    // blockJson is a JSON envelope. Valid payloads have an object-valued
    // block: {"block": <block object>}; malformed payloads preserve the raw
    // JSON string instead. Transaction IDs carried by valid core events are
    // copied into mantle_tx.hash when absent.
    // ReSharper disable once CppFunctionIsNotImplemented
    void newBlock(const std::string& blockJson);
    // clang-format on

protected:
    void onContextReady() override;

private:
    enum class LifecycleState : std::uint8_t {
        Uninitialized,
        Initializing,
        Stopped,
        Starting,
        Running,
        Stopping,
        Destroying,
    };

    enum class LifecycleDispatchDisposition : std::uint8_t {
        Dispatch,
        Duplicate,
        Rejected,
        Noop,
    };

    struct LifecycleOperation {
        std::string action;
        std::string requestFingerprint;
        std::string acknowledgement;
        LifecycleState previousState = LifecycleState::Uninitialized;
        bool settled = false;
        std::string outcome;
    };

    struct LifecycleDispatch {
        LifecycleDispatchDisposition disposition = LifecycleDispatchDisposition::Rejected;
        std::string action;
        std::string operationId;
        LifecycleState previousState = LifecycleState::Uninitialized;
        std::uint64_t generation = 0;
        std::string acknowledgement;
        std::vector<std::string> events;
    };

    struct DeferredLifecycle {
        LifecycleDispatch dispatch;
    };

    LogosBlockchainNode* node = nullptr;

    // Serializes every call that borrows the opaque node handle with lifecycle
    // start/stop/destruction, so shutdown cannot consume it mid-operation.
    mutable std::recursive_mutex nodeMutex;
    mutable std::mutex lifecycleMutex;
    LifecycleState lifecycleState = LifecycleState::Uninitialized;
    std::uint64_t lifecycleGeneration = 0;
    std::string lifecycleInstanceId;
    std::uint64_t lifecycleEpoch = 0;
    std::uint64_t lifecycleSequence = 0;
    std::int64_t lifecycleUpdatedAtMs = 0;
    std::int64_t lifecycleErrorAtMs = 0;
    std::string lifecycleErrorCode;
    std::string lifecycleError;
    bool lifecyclePending = false;
    std::string activeLifecycleOperationId;
    std::string activeLifecycleAction;
    std::uint64_t activeLifecycleGeneration = 0;
    std::string lastCompletedLifecycleOperationId;
    std::string lifecycleConfigPath;
    std::unordered_map<std::string, LifecycleOperation> lifecycleOperations;
    std::deque<std::string> completedLifecycleOperationIds;
    std::thread lifecycleWorker;
    struct CallbackLifetime {
        mutable std::mutex mutex;
        mutable std::condition_variable condition;
        std::size_t inFlight = 0;
        LogosBlockchainNode* deferredNode = nullptr;
        LogosBlockchainModule* owner = nullptr;
        std::optional<DeferredLifecycle> deferredLifecycle;
        bool shutdownInProgress = false;
        bool settlementInProgress = false;
    };
    std::shared_ptr<CallbackLifetime> callbackLifetime = std::make_shared<CallbackLifetime>();

    // Capture explicit identities at node startup; do not reread a mutable
    // configuration file while a node is running.
    std::vector<uint8_t> blendProviderIdentity;
    std::vector<uint8_t> blendZkIdentity;

    LifecycleDispatch beginLifecycleAction(
        const std::string& action,
        const std::string& operation_id,
        const std::string& request_fingerprint,
        bool has_expected_snapshot,
        const std::string& expected_instance_id,
        std::uint64_t expected_epoch,
        std::uint64_t expected_sequence,
        bool strict_action
    );
    void settleLifecycleAction(
        const LifecycleDispatch& dispatch,
        bool success,
        LifecycleState success_state,
        LifecycleState failure_state,
        const std::string& error_code = {}
    );
    [[nodiscard]] std::string lifecycleSnapshotLocked() const;
    [[nodiscard]] std::string lifecycleEventLocked(
        const std::string& action,
        const std::string& operation_id,
        const std::string& phase,
        const std::string& outcome,
        LifecycleState previous_state,
        const std::string& error_code = {}
    ) const;
    void emitLifecycleEvents(const std::vector<std::string>& events);
    void rememberCompletedLifecycleOperationLocked(const std::string& operation_id);
    void joinSettledLifecycleWorker();
    void dispatchLifecycleAction(
        const LifecycleDispatch& dispatch,
        const std::string& initialization_config,
        const std::string& deployment
    );

    [[nodiscard]] std::string lifecycleInitializationConfigPath(const std::string& config) const;
    [[nodiscard]] std::string restoredLifecycleConfigPath() const;
    void persistLifecycleConfigLocked();
    void waitForCallbacks(const std::shared_ptr<CallbackLifetime>& lifetime);
    void waitForDeferredShutdown(const std::shared_ptr<CallbackLifetime>& lifetime);
    static void dispatchDeferredShutdown(
        const std::shared_ptr<CallbackLifetime>& lifetime,
        LogosBlockchainNode* node,
        std::optional<DeferredLifecycle> lifecycle
    );

    [[nodiscard]] StdLogosResult startPrepared(const std::string& config_path, const std::string& deployment);
    [[nodiscard]] StdLogosResult stopPrepared(
        bool* shutdown_attempted = nullptr,
        const LifecycleDispatch* deferred_dispatch = nullptr,
        bool* shutdown_deferred = nullptr
    );

    [[nodiscard]] static const char* lifecycleStateName(LifecycleState state);
    [[nodiscard]] static std::vector<std::string> lifecycleActions(LifecycleState state);
    [[nodiscard]] static const char* lifecycleFailureCode(const std::string& action);
    [[nodiscard]] static const char* lifecycleFailureMessage(const std::string& action);

    // Static instance for C callback (C API doesn't support user data)
    static LogosBlockchainModule* s_instance;
    static std::mutex s_instanceMutex;

    // C-compatible callback function
    static void on_new_block_callback(const char* block);
};
