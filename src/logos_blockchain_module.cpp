#include "logos_blockchain_module.h"
#include "logos_api_client.h"
#include <QVariant>

// Define static member
LogosBlockchainModule* LogosBlockchainModule::s_instance = nullptr;

void LogosBlockchainModule::onNewBlockCallback(const char* block) {
    if (s_instance) {
        qInfo() << "Received new block: " << block;
        QVariantList data;
        data.append(QString::fromUtf8(block));
        s_instance->emitEvent("newBlock", data);
        free_cstring(const_cast<char*>(block)); // Free Rust-allocated memory
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
    return "liblogos-blockchain-module";
}

QString LogosBlockchainModule::version() const {
    return "1.0.0";
}

void LogosBlockchainModule::initLogos(LogosAPI* logosAPIInstance) {
    logosAPI = logosAPIInstance;
    if (logosAPI) {
        client = logosAPI->getClient("liblogos-blockchain-module");
        if (!client) {
            qWarning() << "LogosBlockchainModule: Failed to get liblogos-blockchain-module client";
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

int LogosBlockchainModule::wallet_get_balance(
    const uint8_t* wallet_address,
    const HeaderId* optional_tip,
    BalanceResult* output_balance
) {
    if (!node) {
        qWarning() << "Could not execute the operation: The node is not running.";
        return 1;
    }

    auto [value, error] = get_balance(node, wallet_address, optional_tip);
    if (!is_ok(&error)) {
        qCritical() << "Failed to get balance. Error:" << error;
        return 1;
    }

    output_balance->value = value;
    return 0;
}

int LogosBlockchainModule::wallet_transfer_funds(
    const TransferFundsArguments* transfer_funds_arguments,
    Hash* output_hash
) {
    if (!node) {
        qWarning() << "Could not execute the operation: The node is not running.";
        return 1;
    }

    auto [value, error] = transfer_funds(node, transfer_funds_arguments);
    if (!is_ok(&error)) {
        qCritical() << "Failed to transfer funds. Error:" << error;
        return 1;
    }

    std::ranges::copy(value, *output_hash);
    return 0;
}

int LogosBlockchainModule::wallet_get_known_addresses(lb::KnownAddresses* output) {
    if (!node) {
        qWarning() << "Could not execute the operation: The node is not running.";
        return 1;
    }

    auto [value, error] = get_known_addresses(node);
    if (!is_ok(&error)) {
        qCritical() << "Failed to get known addresses. Error:" << error;
        return 1;
    }

    *output = lb::KnownAddresses(value);
    return 0;
}

void LogosBlockchainModule::emitEvent(const QString& eventName, const QVariantList& data) {
    if (!logosAPI) {
        qWarning() << "LogosBlockchainModule: LogosAPI not available, cannot emit" << eventName;
        return;
    }
    if (!client) {
        qWarning() << "LogosBlockchainModule: Failed to get liblogos-blockchain-module client for event" << eventName;
        return;
    }
    client->onEventResponse(this, eventName, data);
}
