#ifndef I_LOGOS_BLOCKCHAIN_MODULE_API_H
#define I_LOGOS_BLOCKCHAIN_MODULE_API_H

#include <QString>
#include "interface.h"

class ILogosBlockchainModule {
public:
    virtual ~ILogosBlockchainModule() = default;

    // Logos Core
    virtual void initLogos(LogosAPI* logos_api_instance) = 0;

    // ---- Node ----

    // Lifecycle
    virtual int generate_user_config(const QVariantMap& args) = 0;
    virtual int generate_user_config_from_str(const QString& args) = 0;
    virtual int start(const QString& config_path, const QString& deployment) = 0;
    virtual int stop() = 0;

    // Wallet
    virtual QString wallet_get_balance(const QString& address_hex) = 0;
    virtual QString wallet_transfer_funds(
        const QString& change_public_key,
        const QStringList& sender_addresses,
        const QString& recipient_address,
        const QString& amount,
        const QString& optional_tip_hex
    ) = 0;
    virtual QStringList wallet_get_known_addresses() = 0;

    // Blend
    virtual QString blend_join_as_core_node(
        const QString& provider_id_hex,
        const QString& zk_id_hex,
        const QString& locked_note_id_hex,
        const QStringList& locators
    ) = 0;

    // Storage
    virtual QString get_block(const QString& header_id_hex) = 0;
    virtual QString get_blocks(quint64 from_slot, quint64 to_slot) = 0;
    virtual QString get_transaction(const QString& tx_hash_hex) = 0;

    // Cryptarchia
    virtual QString get_cryptarchia_info() = 0;
};

#define ILogosBlockchainModule_iid "org.logos.ilogosblockchainmodule"
Q_DECLARE_INTERFACE(ILogosBlockchainModule, ILogosBlockchainModule_iid)

#endif
