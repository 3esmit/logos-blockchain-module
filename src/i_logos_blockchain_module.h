#ifndef I_LOGOS_BLOCKCHAIN_MODULE_API_H
#define I_LOGOS_BLOCKCHAIN_MODULE_API_H

#include <QString>
#include <core/interface.h>

class ILogosBlockchainModule {
public:
    virtual ~ILogosBlockchainModule() = default;

    // Logos Core
    virtual void initLogos(LogosAPI* logosAPIInstance) = 0;

    // Node
    virtual int generate_user_config(const QVariantMap& args) = 0;
    virtual int generate_user_config_from_str(const QString& args) = 0;
    virtual int start(const QString& config_path, const QString& deployment) = 0;
    virtual int stop() = 0;
    virtual QString wallet_get_balance(const QString& addressHex) = 0;
    virtual QString wallet_transfer_funds(
        const QString& changePublicKey,
        const QStringList& senderAddresses,
        const QString& recipientAddress,
        const QString& amount,
        const QString& optionalTipHex
    ) = 0;
    virtual QStringList wallet_get_known_addresses() = 0;
    virtual int blend_join_as_core_node(
        const QString& providerIdHex,
        const QString& zkIdHex,
        const QString& lockedNoteIdHex,
        const QStringList& locators
    ) = 0;

    // Explorer
    virtual QString get_block(const QString& headerIdHex) = 0;
    virtual QString get_blocks(quint64 fromSlot, quint64 toSlot) = 0;
    virtual QString get_transaction(const QString& txHashHex) = 0;
    virtual QString get_cryptarchia_info() = 0;
};

#define ILogosBlockchainModule_iid "org.logos.ilogosblockchainmodule"
Q_DECLARE_INTERFACE(ILogosBlockchainModule, ILogosBlockchainModule_iid)

#endif
