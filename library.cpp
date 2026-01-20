#include "library.h"

#include <QtCore/QDebug>

#ifdef __cplusplus
extern "C" {
#endif
#include <lib_logos_blockchain.h>
#ifdef __cplusplus
}
#endif

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

    QString name() const override { return "liblogos-blockchain-module"; }
    QString version() const override { return "1.0.0"; }

    void initLogos(LogosAPI* logosAPIInstance) override {
        logosAPI = logosAPIInstance;
    }

    Q_INVOKABLE int start(const QString& config_path) override {
        if (node) {
            qWarning() << "Node already started";
            return 1;
        }

        const QByteArray path = config_path.toUtf8();
        InitializedLogosBlockchainNodeResult result = start_lb_node(path.constData());

        if (!is_ok(&result.error)) {
            qCritical() << "Failed to start Nomos node. Error code:" << result.error;
            return 2;
        }

        node = result.value;
        qInfo() << "Nomos node started successfully";
        return 0;
    }

    Q_INVOKABLE void stop() override {
        if (!node) {
            qWarning() << "Node not running";
            return;
        }

        const OperationStatus status = stop_node(node);
        if (is_ok(&status)) {
            qInfo() << "Nomos node stopped successfully";
        } else {
            qCritical() << "Failed to stop Nomos node. Error code:" << status;
        }

        node = nullptr;
    }
};

#include "library.moc"
