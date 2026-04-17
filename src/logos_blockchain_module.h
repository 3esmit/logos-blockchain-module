#pragma once

#include "i_logos_blockchain_module.h"

#include <iostream>
#ifdef __cplusplus
extern "C" {
#endif
#include <logos_blockchain.h>
#ifdef __cplusplus
}
#endif

class LogosBlockchainModule final : public QObject, public PluginInterface, public ILogosBlockchainModule {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ILogosBlockchainModule_iid FILE LOGOS_BLOCKCHAIN_MODULE_METADATA_FILE)
    Q_INTERFACES(PluginInterface)

public:
    LogosBlockchainModule();
    ~LogosBlockchainModule() override;

    // Logos Core
    [[nodiscard]] QString name() const override;
    [[nodiscard]] QString version() const override;
    Q_INVOKABLE void initLogos(LogosAPI*) override;

    // ---- Node ----

    // Lifecycle
    Q_INVOKABLE int generate_user_config(const QVariantMap& args) override;
    Q_INVOKABLE int generate_user_config_from_str(const QString& args) override;
    Q_INVOKABLE int start(const QString& config_path, const QString& deployment) override;
    Q_INVOKABLE int stop() override;

    // Wallet
    Q_INVOKABLE QString wallet_get_balance(const QString& address_hex) override;
    Q_INVOKABLE QString wallet_transfer_funds(
        const QString& change_public_key,
        const QStringList& sender_addresses,
        const QString& recipient_address,
        const QString& amount,
        const QString& optional_tip_hex
    ) override;
    Q_INVOKABLE QString wallet_transfer_funds(
        const QString& change_public_key,
        const QString& sender_address,
        const QString& recipient_address,
        const QString& amount,
        const QString& optional_tip_hex
    );
    Q_INVOKABLE QStringList wallet_get_known_addresses() override;

    // Blend
    Q_INVOKABLE QString blend_join_as_core_node(
        const QString& provider_id_hex,
        const QString& zk_id_hex,
        const QString& locked_note_id_hex,
        const QStringList& locators
    ) override;

    // Explorer
    Q_INVOKABLE QString get_block(const QString& header_id_hex) override;
    Q_INVOKABLE QString get_blocks(quint64 from_slot, quint64 to_slot) override;
    Q_INVOKABLE QString get_transaction(const QString& tx_hash_hex) override;

    // Cryptarchia
    Q_INVOKABLE QString get_cryptarchia_info() override;

signals:
    void eventResponse(const QString& event_name, const QVariantList& data);

private:
    LogosBlockchainNode* node = nullptr;
    LogosAPIClient* client = nullptr;

    // Static instance for C callback (C API doesn't support user data)
    static LogosBlockchainModule* s_instance;

    // C-compatible callback function
    static void on_new_block_callback(const char* block);

    // Helper method for emitting events
    void emit_event(const QString& event_name, const QVariantList& data);
};
