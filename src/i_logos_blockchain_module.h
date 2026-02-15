#ifndef I_LOGOS_BLOCKCHAIN_MODULE_API_H
#define I_LOGOS_BLOCKCHAIN_MODULE_API_H

#include <core/interface.h>
#include "known_addresses.h"

class ILogosBlockchainModule {
public:
    virtual ~ILogosBlockchainModule() = default;

    // Logos Core
    virtual void initLogos(LogosAPI* logosAPIInstance) = 0;

    // Node
    virtual int start(const QString& config_path, const QString& deployment) = 0;
    virtual int stop() = 0;
    virtual int wallet_get_balance(
        const uint8_t* wallet_address,
        const HeaderId* optional_tip,
        BalanceResult* output_balance
    ) = 0;
    virtual int wallet_transfer_funds(const TransferFundsArguments* transfer_funds_arguments, Hash* output_hash) = 0;
    virtual int wallet_get_known_addresses(lb::KnownAddresses* output) = 0;
};

#define ILogosBlockchainModule_iid "org.logos.ilogosblockchainmodule"
Q_DECLARE_INTERFACE(ILogosBlockchainModule, ILogosBlockchainModule_iid)

#endif
