#include "logos_blockchain_module.h"

#include <QtCore/QDebug>
#include <iostream>

LogosBlockchainModule::LogosBlockchainModule() = default;

LogosBlockchainModule::~LogosBlockchainModule() {
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
}

int LogosBlockchainModule::start(const QString& config_path, const QString& deployment) {
    if (node) {
        qWarning() << "Could not execute the operation: The node is already running.";
        return 1;
    }

    QString effective_config_path= config_path;

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
    return 0;
}

int LogosBlockchainModule::stop() {
    if (!node) {
        qWarning() << "Could not execute the operation: The node is not running.";
        return 1;
    }

    const OperationStatus status = stop_node(node);
    if (is_ok(&status)) {
        qInfo() << "The node was stopped successfully.";
    } else {
        qCritical() << "Could not stop the node. Error:" << status;
    }

    node = nullptr;
    return 0;
}

int LogosBlockchainModule::subscribe() {
    if (!node) {
        qWarning() << "Could not execute the operation: The node is not running.";
        return 1;
    }

    subscribe_to_new_blocks(node, [](const char* block) {
        std::cout << "Received new block: " << block << std::endl;
    });

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
