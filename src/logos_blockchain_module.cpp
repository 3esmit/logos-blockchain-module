#include "logos_blockchain_module.h"
#include "logos_api_client.h"
#include <QByteArray>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMetaType>
#include <QUrl>
#include <QVariant>
#include <QVariantList>

// Define static member
LogosBlockchainModule* LogosBlockchainModule::s_instance = nullptr;

namespace {
    // Rust `File::open` / `deserialize_config_at_path` only accept real filesystem paths. QML often
    // passes `file:///...` URLs; strip to a local path when applicable.
    QString localPathFromFileUrl(const QString& s) {
        if (s.isEmpty()) {
            return s;
        }
        if (s.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
            const QUrl u(s);
            if (u.isLocalFile()) {
                return QDir::toNativeSeparators(u.toLocalFile());
            }
        }
        return s;
    }

    // Use the C API type Hash (from logos_blockchain.h) to define address/hash byte size.
    constexpr int ADDRESS_BYTES = sizeof(Hash);
    constexpr int ADDRESS_HEX_LEN = ADDRESS_BYTES * 2;

    // Parses ADDRESS_HEX_LEN hex chars (optional 0x prefix) to ADDRESS_BYTES. Returns empty QByteArray on error.
    QByteArray parse_address_hex(const QString& address_hex) {
        QString hex = address_hex.trimmed();
        if (hex.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            hex = hex.mid(2);
        if (hex.length() != ADDRESS_HEX_LEN)
            return {};
        QByteArray bytes = QByteArray::fromHex(hex.toUtf8());
        if (bytes.size() != ADDRESS_BYTES)
            return {};
        return bytes;
    }

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
        Deployment deployment_val{};
        QByteArray state_path_data;

        // The FFI struct with pointers into owned data
        GenerateConfigArgs ffi_args{};

        // Constructor that populates both owned data and FFI struct
        explicit OwnedGenerateConfigArgs(const QVariantMap& args) {
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
                const QVariantMap deployment = args["deployment"].toMap(); // NOLINT: Move definition to if-statement

                if (deployment.contains("well_known_deployment")) {
                    deployment_val.deployment_type = DeploymentType::WellKnown;
                    const QString wellknown =
                        deployment["well_known_deployment"].toString(); // NOLINT: Move definition to if-statement
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

namespace environment {
    constexpr auto LOGOS_BLOCKCHAIN_CIRCUITS = "LOGOS_BLOCKCHAIN_CIRCUITS";

    // Checks the directory exists and ensures it contains at least one file to avoid an empty directory false positive
    bool is_circuits_path_valid(const QString& path) {
        const QDir directory(path);
        return directory.exists() && !directory.entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty();
    }

    void setup_circuits_path(const LogosAPI& logos_api) {
        const QString module_path = logos_api.property("modulePath").toString();
        const QDir module_directory(module_path);

        const QString circuits_path = module_directory.filePath(QStringLiteral("circuits"));
        if (!is_circuits_path_valid(circuits_path)) {
            qFatal() << "The LOGOS_BLOCKCHAIN_CIRCUITS environment variable is not set or does not contain any files.";
            return;
        }

        qputenv("LOGOS_BLOCKCHAIN_CIRCUITS", circuits_path.toUtf8());
        qInfo() << "LOGOS_BLOCKCHAIN_CIRCUITS set to:" << circuits_path;
    }
} // namespace environment

void LogosBlockchainModule::on_new_block_callback(const char* block) {
    if (s_instance) {
        qInfo() << "Received new block: " << block;
        QVariantList data;
        data.append(QString::fromUtf8(block));
        s_instance->emit_event("newBlock", data);
        // SAFETY:
        // We are getting an owned pointer here which is freed after this callback is called, so there is not need to
        // free the resource here as we are copying the data!
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

// Logos Core

QString LogosBlockchainModule::name() const {
    return "liblogos_blockchain_module";
}

QString LogosBlockchainModule::version() const {
    return "1.0.0";
}

void LogosBlockchainModule::initLogos(LogosAPI* logos_api_instance) {
    logosAPI = logos_api_instance;
    if (logosAPI) {
        client = logosAPI->getClient("liblogos_blockchain_module");
        if (!client) {
            qWarning() << "LogosBlockchainModule: Failed to get liblogos_blockchain_module client";
        }
    }
}

// ---- Node ----

// Lifecycle

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

int LogosBlockchainModule::start(const QString& config_path, const QString& deployment) {
    if (node) {
        qWarning() << "Could not execute the operation: The node is already running.";
        return 1;
    }
    if (!logosAPI) {
        qCritical() << "LogosAPI instance is null, cannot start node.";
        return 2;
    }

    environment::setup_circuits_path(*logosAPI);
    QString effective_config_path = config_path;

    if (effective_config_path.isEmpty()) {
        const char* env = std::getenv("LB_CONFIG_PATH"); // NOLINT: Move definition to if-statement
        if (env && *env) {
            effective_config_path = QString::fromUtf8(env);
            qInfo() << "Using config from LB_CONFIG_PATH:" << effective_config_path;
        } else {
            qCritical() << "Config path was not specified and LB_CONFIG_PATH is not set.";
            return 3;
        }
    }

    effective_config_path = localPathFromFileUrl(effective_config_path);
    const QString deployment_path = localPathFromFileUrl(deployment);

    const QByteArray config_path_buffer = effective_config_path.toUtf8();
    const char* config_path_ptr = effective_config_path.isEmpty() ? nullptr : config_path_buffer.constData();

    const QByteArray deployment_buffer = deployment_path.toUtf8();
    const char* deployment_ptr = deployment_path.isEmpty() ? nullptr : deployment_buffer.constData();

    auto [value, error] = start_lb_node(config_path_ptr, deployment_ptr);
    qInfo() << "Start node returned with value and error.";
    if (!is_ok(&error)) {
        qCritical() << "Failed to start the node. Error:" << error;
        return 4;
    }

    node = value;
    qInfo() << "The node was started successfully.";

    // Subscribe to block events
    if (!node) {
        qWarning() << "Could not subscribe to block events: The node is not running.";
        return 4;
    }

    s_instance = this;
    const OperationStatus subscribe_status = subscribe_to_new_blocks(node, on_new_block_callback);
    if (!is_ok(&subscribe_status)) {
        qCritical() << "Failed to subscribe to new blocks. Error:" << subscribe_status;
        return 5;
    }

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

// Wallet

QString LogosBlockchainModule::wallet_get_balance(const QString& address_hex) {
    qDebug() << "wallet_get_balance: address_hex=" << address_hex;
    if (!node) {
        return QStringLiteral("Error: The node is not running.");
    }

    const QByteArray bytes = parse_address_hex(address_hex);
    if (bytes.isEmpty() || bytes.size() != ADDRESS_BYTES) {
        return QStringLiteral("Error: Address must be 64 hex characters (32 bytes).");
    }

    auto [value, error] = get_balance(node, reinterpret_cast<const uint8_t*>(bytes.constData()), nullptr);
    if (!is_ok(&error)) {
        return QStringLiteral("Error: Failed to get balance: ") + QString::number(error);
    }

    return QString::number(value);
}

QString LogosBlockchainModule::wallet_transfer_funds(
    const QString& change_public_key,
    const QStringList& sender_addresses,
    const QString& recipient_address,
    const QString& amount,
    const QString& optional_tip_hex
) {
    if (!node) {
        return QStringLiteral("Error: The node is not running.");
    }

    bool ok = false;
    const quint64 amount_val = amount.trimmed().toULongLong(&ok);
    if (!ok) {
        return QStringLiteral("Error: Invalid amount (positive integer required).");
    }

    const QByteArray change_bytes = parse_address_hex(change_public_key);
    if (change_bytes.isEmpty() || change_bytes.size() != ADDRESS_BYTES) {
        return QStringLiteral("Error: Invalid change_public_key (64 hex characters required).");
    }
    const QByteArray recipient_bytes = parse_address_hex(recipient_address);
    if (recipient_bytes.isEmpty() || recipient_bytes.size() != ADDRESS_BYTES) {
        return QStringLiteral("Error: Invalid recipient_address (64 hex characters required).");
    }
    if (sender_addresses.isEmpty()) {
        return QStringLiteral("Error: At least one sender address required.");
    }
    QVector<QByteArray> funding_bytes;
    for (const QString& hex : sender_addresses) {
        QByteArray b = parse_address_hex(hex);
        if (b.isEmpty() || b.size() != ADDRESS_BYTES) {
            return QStringLiteral("Error: Invalid sender address (64 hex characters required).");
        }
        funding_bytes.append(b);
    }
    QVector<const uint8_t*> funding_ptrs;
    for (const QByteArray& b : funding_bytes)
        funding_ptrs.append(reinterpret_cast<const uint8_t*>(b.constData()));

    QByteArray tip_bytes; // NOLINT: Needs to be outside of scope for lifetime
    const HeaderId* optional_tip = nullptr;
    if (!optional_tip_hex.isEmpty()) {
        tip_bytes = parse_address_hex(optional_tip_hex);
        if (tip_bytes.isEmpty() || tip_bytes.size() != ADDRESS_BYTES) {
            return QStringLiteral("Error: Invalid optional tip (64 hex characters or empty).");
        }
        optional_tip = reinterpret_cast<const HeaderId*>(tip_bytes.constData());
    }

    TransferFundsArguments args{};
    args.optional_tip = optional_tip;
    args.change_public_key = reinterpret_cast<const uint8_t*>(change_bytes.constData());
    args.funding_public_keys = funding_ptrs.constData();
    args.funding_public_keys_len = static_cast<size_t>(funding_ptrs.size());
    args.recipient_public_key = reinterpret_cast<const uint8_t*>(recipient_bytes.constData());
    args.amount = amount_val;

    auto [value, error] = transfer_funds(node, &args);
    if (!is_ok(&error)) {
        return QStringLiteral("Error: Failed to transfer funds: ") + QString::number(error);
    }
    // value is Hash (32 bytes); convert to hex string
    const QByteArray hash_bytes(reinterpret_cast<const char*>(&value), ADDRESS_BYTES);
    return QString::fromUtf8(hash_bytes.toHex());
}

QString LogosBlockchainModule::wallet_transfer_funds(
    const QString& change_public_key,
    const QString& sender_address,
    const QString& recipient_address,
    const QString& amount,
    const QString& optional_tip_hex
) {
    return wallet_transfer_funds(
        change_public_key, QStringList{sender_address}, recipient_address, amount, optional_tip_hex
    );
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
    // Each address is ADDRESS_BYTES (32) bytes
    for (size_t i = 0; i < value.len; ++i) {
        const uint8_t* ptr = value.addresses[i]; // NOLINT: Move definition to if-statement
        if (ptr) {
            QByteArray addr(reinterpret_cast<const char*>(ptr), ADDRESS_BYTES);
            out.append(QString::fromUtf8(addr.toHex()));
        }
    }
    const OperationStatus free_status = free_known_addresses(value);
    if (!is_ok(&free_status)) {
        qWarning() << "Failed to free known addresses. Error:" << free_status;
    }
    qDebug() << "blockchain lib: known addresses, count=" << out.size()
             << "sample:" << (out.isEmpty() ? QLatin1String("(none)") : out.constFirst());
    return out;
}

// Blend

QString LogosBlockchainModule::blend_join_as_core_node(
    const QString& provider_id_hex,
    const QString& zk_id_hex,
    const QString& locked_note_id_hex,
    const QStringList& locators
) {
    if (!node) {
        return QStringLiteral("Error: The node is not running.");
    }

    const QByteArray provider_id_bytes = parse_address_hex(provider_id_hex);
    if (provider_id_bytes.isEmpty() || provider_id_bytes.size() != ADDRESS_BYTES) {
        return QStringLiteral("Error: Invalid provider_id_hex (64 hex characters required).");
    }

    const QByteArray zk_id_bytes = parse_address_hex(zk_id_hex);
    if (zk_id_bytes.isEmpty() || zk_id_bytes.size() != ADDRESS_BYTES) {
        return QStringLiteral("Error: Invalid zk_id_hex (64 hex characters required).");
    }

    const QByteArray locked_note_id_bytes = parse_address_hex(locked_note_id_hex);
    if (locked_note_id_bytes.isEmpty() || locked_note_id_bytes.size() != ADDRESS_BYTES) {
        return QStringLiteral("Error: Invalid locked_note_id_hex (64 hex characters required).");
    }

    // QString is UTF-16, but the FFI requires UTF-8.
    // locators_data owns the converted buffers, while locators_ptrs holds raw pointers into them for the FFI call.
    // Using reserve() prevents reallocation, keeping the constData() pointers stable.
    std::vector<QByteArray> locators_data;
    std::vector<const char*> locators_ptrs;
    locators_data.reserve(locators.size());
    locators_ptrs.reserve(locators.size());
    for (const QString& locator : locators) {
        locators_data.push_back(locator.toUtf8());
        locators_ptrs.push_back(locators_data.back().constData());
    }

    auto [value, error] = ::blend_join_as_core_node(
        node,
        reinterpret_cast<const uint8_t*>(provider_id_bytes.constData()),
        reinterpret_cast<const uint8_t*>(zk_id_bytes.constData()),
        reinterpret_cast<const uint8_t*>(locked_note_id_bytes.constData()),
        locators_ptrs.data(),
        locators_ptrs.size()
    );
    if (!is_ok(&error)) {
        return QStringLiteral("Error: Failed to join as core node: ") + QString::number(error);
    }

    const QByteArray declaration_id_bytes(reinterpret_cast<const char*>(&value), sizeof(value));
    const QString declaration_id = QString::fromUtf8(declaration_id_bytes.toHex());
    qInfo() << "Successfully joined as core node. DeclarationId:" << declaration_id;
    return declaration_id;
}

// Storage

QString LogosBlockchainModule::get_block(const QString& header_id_hex) {
    if (!node) {
        return QStringLiteral("Error: The node is not running.");
    }

    const QByteArray bytes = parse_address_hex(header_id_hex);
    if (bytes.isEmpty() || bytes.size() != ADDRESS_BYTES) {
        return QStringLiteral("Error: Header ID must be 64 hex characters (32 bytes).");
    }

    auto [value, error] = ::get_block(node, reinterpret_cast<const HeaderId*>(bytes.constData()));
    if (!is_ok(&error)) {
        qWarning() << "Failed to get block. Error:" << error;
        return QStringLiteral("Error: Failed to get block: ") + QString::number(error);
    }

    const QString result = QString::fromUtf8(value);
    const OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        qWarning() << "Failed to free block string. Error:" << free_status;
    }
    return result;
}

QString LogosBlockchainModule::get_blocks(const quint64 from_slot, const quint64 to_slot) {
    if (!node) {
        return QStringLiteral("Error: The node is not running.");
    }

    auto [value, error] = ::get_blocks(node, from_slot, to_slot);
    if (!is_ok(&error)) {
        qWarning() << "Failed to get blocks. Error:" << error;
        return QStringLiteral("Error: Failed to get blocks: ") + QString::number(error);
    }

    const QString result = QString::fromUtf8(value);
    const OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        qWarning() << "Failed to free blocks string. Error:" << free_status;
    }
    return result;
}

QString LogosBlockchainModule::get_transaction(const QString& tx_hash_hex) {
    if (!node) {
        return QStringLiteral("Error: The node is not running.");
    }

    const QByteArray bytes = parse_address_hex(tx_hash_hex);
    if (bytes.isEmpty() || bytes.size() != ADDRESS_BYTES) {
        return QStringLiteral("Error: Transaction hash must be 64 hex characters (32 bytes).");
    }

    auto [value, error] = ::get_transaction(node, reinterpret_cast<const TxHash*>(bytes.constData()));
    if (!is_ok(&error)) {
        qWarning() << "Failed to get transaction. Error:" << error;
        return QStringLiteral("Error: Failed to get transaction: ") + QString::number(error);
    }

    const QString result = QString::fromUtf8(value);
    const OperationStatus free_status = free_cstring(value);
    if (!is_ok(&free_status)) {
        qWarning() << "Failed to free transaction string. Error:" << free_status;
    }
    return result;
}

// Cryptarchia

QString LogosBlockchainModule::get_cryptarchia_info() {
    if (!node) {
        return QStringLiteral("Error: The node is not running.");
    }

    auto [value, error] = ::get_cryptarchia_info(node);
    if (!is_ok(&error)) {
        qWarning() << "Failed to get cryptarchia info. Error:" << error;
        return QStringLiteral("Error: Failed to get cryptarchia info: ") + QString::number(error);
    }

    QJsonObject obj;
    obj[QStringLiteral("lib")] =
        QString::fromUtf8(QByteArray(reinterpret_cast<const char*>(value->lib), ADDRESS_BYTES).toHex());
    obj[QStringLiteral("tip")] =
        QString::fromUtf8(QByteArray(reinterpret_cast<const char*>(value->tip), ADDRESS_BYTES).toHex());
    obj[QStringLiteral("slot")] = static_cast<qint64>(value->slot);
    obj[QStringLiteral("height")] = static_cast<qint64>(value->height);
    obj[QStringLiteral("mode")] =
        (value->mode == State::Online) ? QStringLiteral("Online") : QStringLiteral("Bootstrapping");

    const OperationStatus free_status = free_cryptarchia_info(value);
    if (!is_ok(&free_status)) {
        qWarning() << "Failed to free cryptarchia info. Error:" << free_status;
    }
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

void LogosBlockchainModule::emit_event(const QString& event_name, const QVariantList& data) {
    if (!logosAPI) {
        qWarning() << "LogosBlockchainModule: LogosAPI not available, cannot emit" << event_name;
        return;
    }
    if (!client) {
        qWarning() << "LogosBlockchainModule: Failed to get liblogos_blockchain_module client for event" << event_name;
        return;
    }
    client->onEventResponse(this, event_name, data);
}
