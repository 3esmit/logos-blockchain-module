#pragma once

#include "i_logos_blockchain_module.h"

class LogosBlockchainModule final : public ILogosBlockchainModule {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ILogosBlockchainModule_iid FILE "../metadata.json")

public:
    LogosBlockchainModule();
    ~LogosBlockchainModule() override;

    [[nodiscard]] QString name() const override;
    [[nodiscard]] QString version() const override;

    Q_INVOKABLE void initLogos(LogosAPI*) override;
    Q_INVOKABLE int start(const QString&) override;
    Q_INVOKABLE int stop() override;
    Q_INVOKABLE int subscribe() override;
    Q_INVOKABLE int wallet_get_balance(const uint8_t*, const HeaderId*, BalanceResult*) override;
    Q_INVOKABLE int wallet_transfer_funds(const TransferFundsArguments*, Hash*) override;

private:
    LogosBlockchainNode* node = nullptr;
};
