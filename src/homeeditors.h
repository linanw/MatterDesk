#pragma once

#include "matterdevice.h"
#include <QJsonArray>
#include <QJsonObject>

class QWidget;

namespace HomeEditors {
// A canceled editor returns {}. Editors only return a draft; HomeStore owns all
// persistence and execution. deviceKey is always MatterDevice::key.
QJsonObject scene(QWidget *parent, QJsonObject existing, QList<MatterDevice> devices);
QJsonObject automation(QWidget *parent, QJsonObject existing, QList<MatterDevice> devices, QJsonArray scenes);
}
