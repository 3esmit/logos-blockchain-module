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

    // Logos Blockchain
    Q_INVOKABLE int generate_user_config(const QVariantMap& args) override;
    Q_INVOKABLE int generate_user_config_from_str(const QString& args) override;
    Q_INVOKABLE int start(const QString& config_path, const QString& deployment) override;
    Q_INVOKABLE int stop() override;
    Q_INVOKABLE QString wallet_get_balance(const QString& addressHex) override;
    Q_INVOKABLE QString wallet_transfer_funds(
        const QString& changePublicKey,
        const QStringList& senderAddresses,
        const QString& recipientAddress,
        const QString& amount,
        const QString& optionalTipHex
    ) override;
    Q_INVOKABLE QString wallet_transfer_funds(
        const QString& changePublicKey,
        const QString& senderAddress,
        const QString& recipientAddress,
        const QString& amount,
        const QString& optionalTipHex
    );
    Q_INVOKABLE QStringList wallet_get_known_addresses() override;
    Q_INVOKABLE int blend_join_as_core_node(
        const QString& providerIdHex,
        const QString& zkIdHex,
        const QString& lockedNoteIdHex,
        const QStringList& locators
    ) override;

signals:
    void eventResponse(const QString& eventName, const QVariantList& data);

private:
    LogosBlockchainNode* node = nullptr;
    LogosAPIClient* client = nullptr;

    // Static instance for C callback (C API doesn't support user data)
    static LogosBlockchainModule* s_instance;

    // C-compatible callback function
    static void onNewBlockCallback(const char* block);

    // Helper method for emitting events
    void emitEvent(const QString& eventName, const QVariantList& data);
};
