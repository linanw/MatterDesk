#pragma once

#include "matterclient.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>

// Controller-scoped local organization. No controller credentials or raw protocol
// responses are stored. Rules run only while this process and controller are ready.
class HomeStore : public QObject {
    Q_OBJECT
public:
    explicit HomeStore(MatterClient *client, QObject *parent = nullptr);
    HomeStore(MatterClient *client, const QString &storageDirectory, QObject *parent = nullptr);
    void setProfile(const QString &profile);
    QString profile() const { return m_profile; }
    QString lastError() const { return m_lastError; }
    QString homeName() const;
    bool setHomeName(const QString &name);
    QJsonArray rooms() const;
    QJsonArray zones() const;
    QJsonArray scenes() const;
    QJsonArray rules() const;
    QJsonArray activity() const;
    QString saveRoom(QJsonObject room);
    QString saveZone(QJsonObject zone);
    QString saveScene(QJsonObject scene);
    QString saveRule(QJsonObject rule);
    bool removeRoom(const QString &id);
    bool removeZone(const QString &id);
    bool removeScene(const QString &id);
    bool removeRule(const QString &id);
    QJsonObject accessory(const QString &deviceKey) const;
    bool setAccessory(const QString &deviceKey, QJsonObject metadata);
    QString displayName(const MatterDevice &device) const;
    bool runScene(const QString &id);
    bool runRule(const QString &id);
    bool isExecuting() const { return !m_executionId.isEmpty(); }
    void cancelExecution();
    // Public clock entry point makes schedule semantics testable. Time rules match
    // the supplied local minute, without replaying missed runs after downtime.
    void evaluateAutomations(const QDateTime &now = QDateTime::currentDateTime());
    void clearActivity();
signals:
    void changed();
    void activityChanged();
    void errorOccurred(QString message);
    void executionStarted(QString id);
    void executionFinished(QString id, bool success, QString detail);
private:
    bool fail(const QString &message);
    bool commit(QJsonObject document, bool notify = true);
    bool persist(const QJsonObject &document);
    bool validateDocument(const QJsonObject &document, QString *error) const;
    bool validateItem(const QString &collection, QJsonObject &item, const QJsonObject &document, QString *error) const;
    QString saveItem(const QString &collection, QJsonObject item);
    bool removeItem(const QString &collection, const QString &id);
    QJsonObject findItem(const QString &collection, const QString &id) const;
    bool startExecution(const QString &id, const QString &name, const QJsonArray &actions, bool automation);
    bool preflight(const QJsonArray &actions, QString *error) const;
    void sendNextAction();
    void finishExecution(bool success, const QString &detail);
    void recordActivity(const QString &type, const QString &name, bool success);
    void observeDevices();
    void resetObservation();
    QJsonValue observedValue(const QJsonObject &trigger) const;
    bool ruleCoolingDown(const QString &id, const QDateTime &now) const;
    bool markRuleRun(const QString &id, const QDateTime &now, bool daily);
    MatterClient *m_client;
    QString m_directory;
    QString m_profile;
    QString m_path;
    QString m_lastError;
    QJsonObject m_document;
    bool m_loadFailed = false;
    QTimer m_scheduleTimer;
    QTimer m_commandTimer;
    QHash<QString, QJsonValue> m_observed;
    bool m_observationReady = false;
    QDateTime m_edgeHoldUntil;
    QString m_executionId;
    QString m_executionName;
    QString m_pendingCommand;
    QSet<QString> m_abandonedCommands;
    QJsonArray m_executionActions;
    int m_actionIndex = 0;
    bool m_executionAutomation = false;
    quint64 m_executionGeneration = 0;
};
