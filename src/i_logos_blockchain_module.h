#ifndef I_LOGOS_BLOCKCHAIN_MODULE_API_H
#define I_LOGOS_BLOCKCHAIN_MODULE_API_H

#include <QString>
#include <QStringList>
#include <core/interface.h>

class ILogosBlockchainModule {
public:
    virtual ~ILogosBlockchainModule() = default;

    // Logos Core
    virtual void initLogos(LogosAPI* logosAPIInstance) = 0;

    // Node
    virtual int start(const QString& config_path, const QString& deployment) = 0;
    virtual int stop() = 0;
    virtual QString wallet_get_balance(const QString& addressHex) = 0;
    virtual QString wallet_transfer_funds(
        const QString& changePublicKey, 
        const QStringList& senderAddresses, 
        const QString& recipientAddress, 
        const QString& amount, 
        const QString& optionalTipHex) = 0;
    virtual QStringList wallet_get_known_addresses() = 0;
};

#define ILogosBlockchainModule_iid "org.logos.ilogosblockchainmodule"
Q_DECLARE_INTERFACE(ILogosBlockchainModule, ILogosBlockchainModule_iid)

#endif
