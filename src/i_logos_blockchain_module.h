#ifndef I_LOGOS_BLOCKCHAIN_MODULE_API_H
#define I_LOGOS_BLOCKCHAIN_MODULE_API_H

#include <core/interface.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <logos_blockchain.h>
#ifdef __cplusplus
}
#endif

class ILogosBlockchainModule : public QObject, public PluginInterface {
    Q_OBJECT
    Q_INTERFACES(PluginInterface)

public:
    using QObject::QObject;
    ~ILogosBlockchainModule() override = default;

    // Logos Core
    Q_INVOKABLE virtual void initLogos(LogosAPI* logosAPIInstance) = 0;

    // Node
    Q_INVOKABLE virtual int start(const QString& config_path) = 0;
    Q_INVOKABLE virtual int stop() = 0;
    Q_INVOKABLE virtual int subscribe() = 0;
    Q_INVOKABLE virtual int wallet_get_balance(const uint8_t* wallet_address, const HeaderId* optional_tip, BalanceResult* output_balance) = 0;
    Q_INVOKABLE virtual int wallet_transfer_funds(const TransferFundsArguments* transfer_funds_arguments, Hash* output_hash) = 0;

    signals:
        void eventResponse(const QString& eventName, const QVariantList& data);
};

#define ILogosBlockchainModule_iid "org.logos.ilogosblockchainmodule"
Q_DECLARE_INTERFACE(ILogosBlockchainModule, ILogosBlockchainModule_iid)

#endif
