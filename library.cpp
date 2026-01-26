#include "library.h"

#include <QtCore/QDebug>
#include <iostream>
#include <QCoreApplication>


class LogosBlockchainModule : public LogosBlockchainModuleAPI {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID LogosBlockchainModuleInterface_iid FILE "metadata.json")

private:
    LogosBlockchainNode* node = nullptr;

public:
    LogosBlockchainModule() = default;

    ~LogosBlockchainModule() override {
        if (node) stop();
    }

    [[nodiscard]] QString name() const override { return "liblogos-blockchain-module"; }
    [[nodiscard]] QString version() const override { return "1.0.0"; }

    Q_INVOKABLE void initLogos(LogosAPI* logosAPIInstance) override {
        logosAPI = logosAPIInstance;
    }

    Q_INVOKABLE int start(const QString& config_path) override {
        if (node) {
            qWarning() << "Could not execute the operation: The node is already running.";
            return 1;
        }

        // TODO: Default configuration file

        // TODO: Ensure proper cleanup on SIGINT

        const QByteArray path = config_path.toUtf8();
        auto [value, error] = start_lb_node(path.constData());

        if (!is_ok(&error)) {
            qCritical() << "Failed to start the node. Error:" << error;
            return 2;
        }

        node = value;
        qInfo() << "The node was started successfully.";
        return 0;
    }

    Q_INVOKABLE int stop() override {
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

    Q_INVOKABLE int subscribe() override {
        if (!node) {
            qWarning() << "Could not execute the operation: The node is not running.";
            return 1;
        }

        subscribe_to_new_blocks(node, [](const char* block) {
            std::cout << "Received new block: " << block << std::endl;
        });

        return 0;
    }

    Q_INVOKABLE int wallet_get_balance(const uint8_t* wallet_address, const HeaderId* optional_tip, BalanceResult* output_balance) override {
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

    Q_INVOKABLE int wallet_transfer_funds(const TransferFundsArguments* transfer_funds_arguments, Hash* output_hash) override {
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
};

#include "library.moc"
