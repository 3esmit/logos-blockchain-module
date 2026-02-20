#include "logos_blockchain_module.h"
#include "logos_api_client.h"
#include <QByteArray>
#include <QVariant>

// Define static member
LogosBlockchainModule* LogosBlockchainModule::s_instance = nullptr;

namespace {
    // Use the C API type Hash (from logos_blockchain.h) to define address/hash byte size.
    constexpr int kAddressBytes = static_cast<int>(sizeof(Hash));
    constexpr int kAddressHexLen = kAddressBytes * 2;

    // Parses kAddressHexLen hex chars (optional 0x prefix) to kAddressBytes. Returns empty QByteArray on error.
    QByteArray parseAddressHex(const QString& addressHex) {
        QString hex = addressHex.trimmed();
        if (hex.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            hex = hex.mid(2);
        if (hex.length() != kAddressHexLen)
            return {};
        QByteArray bytes = QByteArray::fromHex(hex.toUtf8());
        if (bytes.size() != kAddressBytes)
            return {};
        return bytes;
    }
} // namespace

void LogosBlockchainModule::onNewBlockCallback(const char* block) {
    if (s_instance) {
        qInfo() << "Received new block: " << block;
        QVariantList data;
        data.append(QString::fromUtf8(block));
        s_instance->emitEvent("newBlock", data);
        // SAFETY:
        // We are getting an owned pointer here which is freed after this callback is called, so there is not need to
        // free the resrouce here as we are copying the data!
    }
}

LogosBlockchainModule::LogosBlockchainModule() {
    node = nullptr;
}

LogosBlockchainModule::~LogosBlockchainModule() {
    s_instance = nullptr;
    if (node) {
        stop();
    }
}

QString LogosBlockchainModule::name() const {
    return "liblogos_blockchain_module";
}

QString LogosBlockchainModule::version() const {
    return "1.0.0";
}

void LogosBlockchainModule::initLogos(LogosAPI* logosAPIInstance) {
    logosAPI = logosAPIInstance;
    if (logosAPI) {
        client = logosAPI->getClient("liblogos_blockchain_module");
        if (!client) {
            qWarning() << "LogosBlockchainModule: Failed to get liblogos_blockchain_module client";
        }
    }
}

int LogosBlockchainModule::start(const QString& config_path, const QString& deployment) {
    if (node) {
        qWarning() << "Could not execute the operation: The node is already running.";
        return 1;
    }

    // Set LOGOS_BLOCKCHAIN_CIRCUITS env variable
    QString circuits_path = logosAPI->property("modulePath").toString().append(QString::fromUtf8("\\circuits"));
    qputenv("LOGOS_BLOCKCHAIN_CIRCUITS", circuits_path.toUtf8());
    qInfo() << "LOGOS_BLOCKCHAIN_CIRCUITS set to:" << circuits_path;
    QString effective_config_path = config_path;

    if (effective_config_path.isEmpty()) {
        const char* env = std::getenv("LB_CONFIG_PATH");
        if (env && *env) {
            effective_config_path = QString::fromUtf8(env);
            qInfo() << "Using config from LB_CONFIG_PATH:" << effective_config_path;
        } else {
            qCritical() << "Config path was not specified and LB_CONFIG_PATH is not set.";
            return 2;
        }
    }

    qInfo() << "Starting the node with the configuration file:" << effective_config_path;
    qInfo() << "Using deployment:" << (deployment.isEmpty() ? "<default>" : deployment);

    const QByteArray config_path_buffer = effective_config_path.toUtf8();
    const char* config_path_ptr = effective_config_path.isEmpty() ? nullptr : config_path_buffer.constData();

    const QByteArray deployment_buffer = deployment.toUtf8();
    const char* deployment_ptr = deployment.isEmpty() ? nullptr : deployment_buffer.constData();

    auto [value, error] = start_lb_node(config_path_ptr, deployment_ptr);
    qInfo() << "Start node returned with value and error.";
    if (!is_ok(&error)) {
        qCritical() << "Failed to start the node. Error:" << error;
        return 3;
    }

    node = value;
    qInfo() << "The node was started successfully.";

    // Subscribe to block events
    if (!node) {
        qWarning() << "Could not subcribe to block events: The node is not running.";
        return 1;
    }

    s_instance = this;
    subscribe_to_new_blocks(node, onNewBlockCallback);

    return 0;
}

int LogosBlockchainModule::stop() {
    if (!node) {
        qWarning() << "Could not execute the operation: The node is not running.";
        return 1;
    }

    s_instance = nullptr; // Clear before stopping to prevent callbacks during shutdown

    const OperationStatus status = stop_node(node);
    if (is_ok(&status)) {
        qInfo() << "The node was stopped successfully.";
    } else {
        qCritical() << "Could not stop the node. Error:" << status;
    }

    node = nullptr;
    return 0;
}

QString LogosBlockchainModule::wallet_get_balance(const QString& addressHex) {
    qDebug() << "wallet_get_balance: addressHex=" << addressHex;
    if (!node) {
        return QStringLiteral("Error: The node is not running.");
    }

    QByteArray bytes = parseAddressHex(addressHex);
    if (bytes.isEmpty() || bytes.size() != kAddressBytes) {
        return QStringLiteral("Error: Address must be 64 hex characters (32 bytes).");
    }

    auto [value, error] = get_balance(node, reinterpret_cast<const uint8_t*>(bytes.constData()), nullptr);
    if (!is_ok(&error)) {
        return QStringLiteral("Error: Failed to get balance: ") + QString::number(error);
    }

    return QString::number(value);
}

QString LogosBlockchainModule::wallet_transfer_funds(
    const QString& changePublicKey,
    const QStringList& senderAddresses,
    const QString& recipientAddress,
    const QString& amount,
    const QString& optionalTipHex
) {
    if (!node) {
        return QStringLiteral("Error: The node is not running.");
    }

    bool ok = false;
    const quint64 amountVal = amount.trimmed().toULongLong(&ok);
    if (!ok) {
        return QStringLiteral("Error: Invalid amount (positive integer required).");
    }

    QByteArray changeBytes = parseAddressHex(changePublicKey);
    if (changeBytes.isEmpty() || changeBytes.size() != kAddressBytes) {
        return QStringLiteral("Error: Invalid changePublicKey (64 hex characters required).");
    }
    QByteArray recipientBytes = parseAddressHex(recipientAddress);
    if (recipientBytes.isEmpty() || recipientBytes.size() != kAddressBytes) {
        return QStringLiteral("Error: Invalid recipientAddress (64 hex characters required).");
    }
    if (senderAddresses.isEmpty()) {
        return QStringLiteral("Error: At least one sender address required.");
    }
    QVector<QByteArray> fundingBytes;
    for (const QString& hex : senderAddresses) {
        QByteArray b = parseAddressHex(hex);
        if (b.isEmpty() || b.size() != kAddressBytes) {
            return QStringLiteral("Error: Invalid sender address (64 hex characters required).");
        }
        fundingBytes.append(b);
    }
    QVector<const uint8_t*> fundingPtrs;
    for (const QByteArray& b : fundingBytes)
        fundingPtrs.append(reinterpret_cast<const uint8_t*>(b.constData()));

    QByteArray tipBytes;
    const HeaderId* optional_tip = nullptr;
    if (!optionalTipHex.isEmpty()) {
        tipBytes = parseAddressHex(optionalTipHex);
        if (tipBytes.isEmpty() || tipBytes.size() != kAddressBytes) {
            return QStringLiteral("Error: Invalid optional tip (64 hex characters or empty).");
        }
        optional_tip = reinterpret_cast<const HeaderId*>(tipBytes.constData());
    }

    TransferFundsArguments args{};
    args.optional_tip = optional_tip;
    args.change_public_key = reinterpret_cast<const uint8_t*>(changeBytes.constData());
    args.funding_public_keys = fundingPtrs.constData();
    args.funding_public_keys_len = static_cast<size_t>(fundingPtrs.size());
    args.recipient_public_key = reinterpret_cast<const uint8_t*>(recipientBytes.constData());
    args.amount = amountVal;

    auto [value, error] = transfer_funds(node, &args);
    if (!is_ok(&error)) {
        return QStringLiteral("Error: Failed to transfer funds: ") + QString::number(static_cast<int>(error));
    }
    // value is Hash (32 bytes); convert to hex string
    QByteArray hashBytes(reinterpret_cast<const char*>(&value), kAddressBytes);
    return QString::fromUtf8(hashBytes.toHex());
}

QString LogosBlockchainModule::wallet_transfer_funds(
    const QString& changePublicKey,
    const QString& senderAddress,
    const QString& recipientAddress,
    const QString& amount,
    const QString& optionalTipHex
) {
    return wallet_transfer_funds(changePublicKey, QStringList{senderAddress}, recipientAddress, amount, optionalTipHex);
}

QStringList LogosBlockchainModule::wallet_get_known_addresses() {
    QStringList out;
    if (!node) {
        qWarning() << "Could not execute the operation: The node is not running.";
        return out;
    }
    auto [value, error] = get_known_addresses(node);
    if (!is_ok(&error)) {
        qCritical() << "Failed to get known addresses. Error:" << error;
        return out;
    }
    // Each address is kAddressBytes (32) byte
    for (size_t i = 0; i < value.len; ++i) {
        const uint8_t* ptr = value.addresses[i];
        if (ptr) {
            QByteArray addr(reinterpret_cast<const char*>(ptr), kAddressBytes);
            out.append(QString::fromUtf8(addr.toHex()));
        }
    }
    free_known_addresses(value);
    qDebug() << "blockchain lib: known addresses, count=" << out.size()
             << "sample:" << (out.isEmpty() ? QLatin1String("(none)") : out.constFirst());
    return out;
}

namespace {
    // Wrapper that owns data and provides GenerateConfigArgs
    struct OwnedGenerateConfigArgs {
        std::vector<QByteArray> initial_peers_data;
        std::vector<const char*> initial_peers_ptrs;
        uint32_t initial_peers_count_val;
        QByteArray output_data;
        uint16_t net_port_val;
        uint16_t blend_port_val;
        QByteArray http_addr_data;
        QByteArray external_address_data;
        bool no_public_ip_check_val;
        QByteArray custom_deployment_config_path_data;
        Deployment deployment_val;
        QByteArray state_path_data;

        // The FFI struct with pointers into owned data
        GenerateConfigArgs ffi_args;

        // Constructor that populates both owned data and FFI struct
        explicit OwnedGenerateConfigArgs(const QVariantMap& args) {
            ffi_args = {};

            // initial_peers (QStringList -> const char**)
            if (args.contains("initial_peers")) {
                QStringList peers = args["initial_peers"].toStringList();
                initial_peers_count_val = static_cast<uint32_t>(peers.size());

                for (const QString& peer : peers) {
                    initial_peers_data.push_back(peer.toUtf8());
                }
                for (const QByteArray& data : initial_peers_data) {
                    initial_peers_ptrs.push_back(data.constData());
                }

                ffi_args.initial_peers = initial_peers_ptrs.data();
                ffi_args.initial_peers_count = &initial_peers_count_val;
            } else {
                ffi_args.initial_peers = nullptr;
                ffi_args.initial_peers_count = nullptr;
            }

            // output (QString -> const char*)
            if (args.contains("output")) {
                output_data = args["output"].toString().toUtf8();
                ffi_args.output = output_data.constData();
            } else {
                ffi_args.output = nullptr;
            }

            // net_port (int -> const uint16_t*)
            if (args.contains("net_port")) {
                net_port_val = static_cast<uint16_t>(args["net_port"].toInt());
                ffi_args.net_port = &net_port_val;
            } else {
                ffi_args.net_port = nullptr;
            }

            // blend_port (int -> const uint16_t*)
            if (args.contains("blend_port")) {
                blend_port_val = static_cast<uint16_t>(args["blend_port"].toInt());
                ffi_args.blend_port = &blend_port_val;
            } else {
                ffi_args.blend_port = nullptr;
            }

            // http_addr (QString -> const char*)
            if (args.contains("http_addr")) {
                http_addr_data = args["http_addr"].toString().toUtf8();
                ffi_args.http_addr = http_addr_data.constData();
            } else {
                ffi_args.http_addr = nullptr;
            }

            // external_address (QString -> const char*)
            if (args.contains("external_address")) {
                external_address_data = args["external_address"].toString().toUtf8();
                ffi_args.external_address = external_address_data.constData();
            } else {
                ffi_args.external_address = nullptr;
            }

            // no_public_ip_check (bool -> const bool*)
            if (args.contains("no_public_ip_check")) {
                no_public_ip_check_val = args["no_public_ip_check"].toBool();
                ffi_args.no_public_ip_check = &no_public_ip_check_val;
            } else {
                ffi_args.no_public_ip_check = nullptr;
            }

            // deployment (const struct Deployment*)
            // Expected format: { "deployment": { "well_known_deployment": "devnet" } }
            //              OR: { "deployment": { "config_path": "/path/to/config" } }
            if (args.contains("deployment")) {
                QVariantMap deployment = args["deployment"].toMap();

                if (deployment.contains("well_known_deployment")) {
                    deployment_val.deployment_type = DeploymentType::WellKnown;
                    QString wellknown = deployment["well_known_deployment"].toString();
                    if (wellknown == "devnet") {
                        deployment_val.well_known_deployment = WellKnownDeployment::Devnet;
                    }
                    deployment_val.custom_deployment_config_path = nullptr;
                } else if (deployment.contains("config_path")) {
                    deployment_val.deployment_type = DeploymentType::Custom;
                    deployment_val.well_known_deployment = static_cast<WellKnownDeployment>(0);
                    custom_deployment_config_path_data = deployment["config_path"].toString().toUtf8();
                    deployment_val.custom_deployment_config_path = custom_deployment_config_path_data.constData();
                }

                ffi_args.deployment = &deployment_val;
            } else {
                ffi_args.deployment = nullptr;
            }

            // state_path (QString -> const char*)
            if (args.contains("state_path")) {
                state_path_data = args["state_path"].toString().toUtf8();
                ffi_args.state_path = state_path_data.constData();
            } else {
                ffi_args.state_path = nullptr;
            }
        }
    };
} // namespace

int LogosBlockchainModule::generate_user_config(const QVariantMap& args) {
    const OwnedGenerateConfigArgs owned_args(args);

    const OperationStatus status = ::generate_user_config(owned_args.ffi_args);
    if (!is_ok(&status)) {
        qCritical() << "Failed to generate user config. Error:" << status;
        return 1;
    }

    return 0;
}

int LogosBlockchainModule::generate_user_config_from_str(const QString& args) {
    const QVariantMap parsed_args = QJsonDocument::fromJson(args.toUtf8()).object().toVariantMap();
    return generate_user_config(parsed_args);
}

void LogosBlockchainModule::emitEvent(const QString& eventName, const QVariantList& data) {
    if (!logosAPI) {
        qWarning() << "LogosBlockchainModule: LogosAPI not available, cannot emit" << eventName;
        return;
    }
    if (!client) {
        qWarning() << "LogosBlockchainModule: Failed to get liblogos_blockchain_module client for event" << eventName;
        return;
    }
    client->onEventResponse(this, eventName, data);
}
