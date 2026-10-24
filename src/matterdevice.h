#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QStringList>

struct MatterDevice {
    QString key;
    QString nodeName;
    QString name;
    QString kind;
    qint64 nodeId = 0;
    quint64 controllerGeneration = 0;
    int endpointId = 0;
    bool available = false;
    bool on = false;
    bool onKnown = false;
    bool hasOnOff = false;
    bool hasLevel = false;
    bool hasColorTemperature = false;
    bool hasCamera = false;
    bool hasCameraPtz = false;
    bool cameraPtzKnown = false;
    bool cameraPtzSupported = false;
    bool cameraPtzAvailable = false;
    quint8 cameraPtzDirections = 0;
    bool levelKnown = false;
    bool colorTemperatureKnown = false;
    int level = 0;
    int colorTemperature = 0;
    QStringList readings;
    QJsonObject attributes;
    // Actions are capabilities; absent state means unknown, never a default.
    QStringList controls;
    QJsonObject controlState;
    QJsonObject controlLimits;
};

QList<MatterDevice> devicesFromNodes(const QJsonArray &nodes);
QJsonArray demoNodes();
QJsonArray homeDemoNodes();
