#pragma once
#include "i_logos_blockchain_module.h"
#include <QtCore/QDebug>
#include <iostream>

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
    Q_INVOKABLE int start(const QString& config_path, const QString& deployment) override;
    Q_INVOKABLE int stop() override;
    Q_INVOKABLE int wallet_get_balance(const uint8_t*, const HeaderId*, BalanceResult*) override;
    Q_INVOKABLE int wallet_transfer_funds(const TransferFundsArguments*, Hash*) override;
    Q_INVOKABLE int wallet_get_known_addresses(lb::KnownAddresses*) override;

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
