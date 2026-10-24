#include "matterdevice.h"

#include <QMap>
#include <QSet>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
QString path(int endpoint, quint32 cluster, int attribute) {
    return QStringLiteral("%1/%2/%3").arg(endpoint).arg(cluster).arg(attribute);
}
QSet<int> numbers(const QJsonValue &value) {
    QSet<int> result;
    for (const auto &entry : value.toArray()) {
        if (entry.isDouble()) result.insert(entry.toInt());
    }
    return result;
}
QSet<quint32> clusterIds(const QJsonValue &value) {
    QSet<quint32> result;
    for (const auto &entry : value.toArray()) {
        const double number = entry.toDouble(-1);
        if (entry.isDouble() && std::isfinite(number) && number >= 0 && number <= 4294967295.0
            && std::floor(number) == number) result.insert(quint32(number));
    }
    return result;
}
QString textAttribute(const QJsonObject &attributes, int endpoint, int cluster, int attribute) {
    return attributes.value(path(endpoint, cluster, attribute)).toString().trimmed();
}
bool validNumber(const QJsonValue &value, int minimum, int maximum) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= minimum && value.toDouble() <= maximum;
}
QSet<int> types(const QJsonObject &attributes, int endpoint) {
    QSet<int> result;
    for (const auto &entry : attributes.value(path(endpoint, 29, 0)).toArray()) {
        const auto item = entry.toObject();
        auto value = item.value(QStringLiteral("deviceType"));
        if (value.isUndefined()) value = item.value(QStringLiteral("device_type"));
        // matter-server serializes Matter structs with numeric TLV field tags;
        // DeviceTypeStruct tag 0 is DeviceType, tag 1 is Revision.
        if (value.isUndefined()) value = item.value(QStringLiteral("0"));
        const int type = value.toInt(-1);
        if (value.isDouble() && type >= 0 && value.toDouble() == type) result.insert(type);
    }
    return result;
}
int parentEndpoint(const QJsonObject &node, const QJsonObject &attributes, int endpoint) {
    const auto parents = node.value(QStringLiteral("endpoint_parents")).toObject();
    const auto explicitParent = parents.value(QString::number(endpoint));
    if (explicitParent.isDouble()) return explicitParent.toInt();
    // Older servers have no endpoint_parents. Prefer a bridged ancestor with a
    // PartsList containing this endpoint; root/aggregator ancestors carry no Reachable.
    for (auto it = attributes.begin(); it != attributes.end(); ++it) {
        if (!it.key().endsWith(QStringLiteral("/29/3"))) continue;
        const int candidate = it.key().section('/', 0, 0).toInt();
        if (candidate != endpoint && attributes.contains(path(candidate, 57, 17))
            && numbers(it.value()).contains(endpoint)) return candidate;
    }
    return 0;
}
bool reachable(const QJsonObject &node, const QJsonObject &attributes, int endpoint) {
    if (!node.value(QStringLiteral("available")).toBool(false)) return false;
    QSet<int> seen;
    while (endpoint > 0 && !seen.contains(endpoint)) {
        seen.insert(endpoint);
        const auto reach = attributes.value(path(endpoint, 57, 17));
        if (reach.isBool() && !reach.toBool()) return false;
        endpoint = parentEndpoint(node, attributes, endpoint);
    }
    return true;
}

bool integer(const QJsonValue &value, int minimum, int maximum) {
    return validNumber(value, minimum, maximum) && std::floor(value.toDouble()) == value.toDouble();
}
QJsonObject range(double minimum, double maximum, double step = 1) {
    return {{"min", minimum}, {"max", maximum}, {"step", step}};
}
// A supplied AcceptedCommandList is authoritative. Older servers omit global
// attributes, so mandatory commands still follow their cluster's conformance.
bool accepts(const QJsonObject &attrs, int ep, int cluster, int command) {
    const auto list = attrs.value(path(ep, cluster, 65529));
    return list.isUndefined() || (list.isArray() && numbers(list).contains(command));
}
void homeCapabilities(MatterDevice &d, const QJsonObject &attrs, const QSet<quint32> &servers) {
    const int ep = d.endpointId;
    auto at = [&](int cluster, int attribute) { return attrs.value(path(ep, cluster, attribute)); };
    auto add = [&](const QString &action, const QJsonValue &limits = QJsonValue(QJsonValue::Undefined)) {
        d.controls.append(action);
        if (!limits.isUndefined()) d.controlLimits.insert(action, limits);
    };
    if (d.hasOnOff && accepts(attrs, ep, 6, 0) && accepts(attrs, ep, 6, 1)) add("power");
    if (d.onKnown) d.controlState.insert("power", d.on);
    if (d.hasLevel && accepts(attrs, ep, 8, 4)) add("brightness", range(0, 100));
    if (d.levelKnown) d.controlState.insert("brightness", qRound(d.level * 100.0 / 254.0));
    if (d.hasColorTemperature && accepts(attrs, ep, 768, 10)) {
        const auto minimum = at(768, 16395), maximum = at(768, 16396);
        const int lo = integer(minimum, 1, 65534) ? minimum.toInt() : 1;
        const int hi = integer(maximum, 1, 65534) ? maximum.toInt() : 65534;
        if (lo <= hi) add("colorTemperature", range(lo, hi));
    }
    if (d.colorTemperatureKnown) d.controlState.insert("colorTemperature", d.colorTemperature);
    const auto feature = at(768, 65532), capabilities = at(768, 16394);
    const int colors = integer(feature, 0, 0xffff) ? feature.toInt()
        : integer(capabilities, 0, 0xffff) ? capabilities.toInt() : 0;
    if (servers.contains(768) && (colors & 1)
        && (accepts(attrs, ep, 768, 6) || ((colors & 2) && accepts(attrs, ep, 768, 67)))) {
        add("color", QJsonObject{{"hueMin", 0}, {"hueMax", 360}, {"saturationMin", 0}, {"saturationMax", 100}});
        const auto hue = at(768, 0), enhanced = at(768, 16384), saturation = at(768, 1);
        const bool eh = (colors & 2) && integer(enhanced, 0, 65535);
        if (integer(saturation, 0, 254) && (eh || integer(hue, 0, 254)))
            d.controlState.insert("color", QJsonObject{{"hue", eh ? enhanced.toDouble() * 360.0 / 65536.0 : hue.toDouble() * 360.0 / 254.0},
                {"saturation", saturation.toDouble() * 100.0 / 254.0}});
    }
    if (servers.contains(257)) {
        d.kind = "lock";
        // Never coerce null / not-fully-locked / future enum values into unlocked.
        const auto state = at(257, 0);
        QString label = "unknown";
        if (integer(state, 0, 3)) {
            static const QStringList labels{"notFullyLocked", "locked", "unlocked", "unlatched"};
            label = labels.at(state.toInt());
            if (state.toInt() == 1 || state.toInt() == 2) d.controlState.insert("lock", state.toInt() == 1);
        }
        d.controlState.insert("lockState", label);
        if (at(257, 2) != QJsonValue(false) && accepts(attrs, ep, 257, 0) && accepts(attrs, ep, 257, 1)) add("lock");
    }
    if (servers.contains(258)) {
        d.kind = "cover";
        const auto f = at(258, 65532);
        if (integer(f, 0, 0xffff) && (f.toInt() & 5) == 5 && accepts(attrs, ep, 258, 5)) add("position", range(0, 100));
        const auto position = at(258, 14), legacy = at(258, 8);
        if (integer(position, 0, 10000)) d.controlState.insert("position", (10000 - position.toInt()) / 100.0);
        else if (integer(legacy, 0, 100)) d.controlState.insert("position", 100 - legacy.toInt());
    }
    if (servers.contains(513)) {
        d.kind = "thermostat";
        const auto f = at(513, 65532), sequence = at(513, 27);
        const bool seqKnown = integer(sequence, 0, 5);
        const bool featureKnown = integer(f, 0, 0xffff);
        const bool heat = featureKnown ? (f.toInt() & 1) : seqKnown && sequence.toInt() >= 2;
        const bool cool = featureKnown ? (f.toInt() & 2) : seqKnown && (sequence.toInt() <= 1 || sequence.toInt() >= 4);
        // A mode needs both feature support and the current operating sequence.
        QJsonArray modes;
        if (seqKnown) {
            modes.append("off");
            if (heat && sequence.toInt() >= 2) modes.append("heat");
            if (cool && (sequence.toInt() <= 1 || sequence.toInt() >= 4)) modes.append("cool");
            if (heat && cool && (f.toInt() & 32) && sequence.toInt() >= 4) modes.append("auto");
            add("thermostatMode", modes);
        }
        const QMap<int, QString> modeNames{{0,"off"}, {1,"auto"}, {3,"cool"}, {4,"heat"}, {5,"emergencyHeat"}, {6,"precooling"}, {7,"fanOnly"}, {8,"dry"}, {9,"sleep"}};
        const auto mode = at(513, 28);
        if (integer(mode, 0, 255) && modeNames.contains(mode.toInt())) d.controlState.insert("thermostatMode", modeNames.value(mode.toInt()));
        auto setpoint = [&](QString action, bool supported, int attribute, int absMin, int absMax, int userMin, int userMax, int defaultMin, int defaultMax) {
            const auto current = at(513, attribute);
            if (integer(current, -27315, 32767)) d.controlState.insert(action, current.toDouble() / 100.0);
            if (!supported) return;
            int lo = defaultMin, hi = defaultMax;
            if (integer(at(513, absMin), -27315, 32767)) lo = at(513, absMin).toInt();
            if (integer(at(513, absMax), -27315, 32767)) hi = at(513, absMax).toInt();
            if (integer(at(513, userMin), -27315, 32767)) lo = qMax(lo, at(513, userMin).toInt());
            if (integer(at(513, userMax), -27315, 32767)) hi = qMin(hi, at(513, userMax).toInt());
            if (lo <= hi) add(action, range(lo / 100.0, hi / 100.0, 0.1));
        };
        setpoint("heatSetpoint", heat, 18, 3, 4, 21, 22, 700, 3000);
        setpoint("coolSetpoint", cool, 17, 5, 6, 23, 24, 1600, 3200);
        const auto deadband = at(513, 25);
        if (integer(deadband, 0, 127)) d.controlLimits.insert("setpointDeadband", deadband.toDouble() / 10.0);
        else if (featureKnown && (f.toInt() & 32)) d.controlLimits.insert("setpointDeadband", 2.0); // schema default
        const auto temperature = at(513, 0);
        d.readings << (integer(temperature, -27315, 32767)
            ? QStringLiteral("室温  %1 °C").arg(temperature.toDouble()/100.0, 0, 'f', 1) : QStringLiteral("室温  未知"));
    }
    if (servers.contains(514)) {
        if (d.kind != "thermostat") d.kind = "fan";
        add("fanPercent", range(0, 100));
        const auto setting = at(514, 2), current = at(514, 3), sequence = at(514, 1), mode = at(514, 0);
        if (integer(setting, 0, 100)) d.controlState.insert("fanPercent", setting);
        if (integer(current, 0, 100)) d.readings << QStringLiteral("当前风速  %1 %").arg(current.toInt());
        if (integer(sequence, 0, 5)) {
            QJsonArray modes{"off"};
            if (sequence.toInt() < 4) modes.append("low");
            if (sequence.toInt() == 0 || sequence.toInt() == 2) modes.append("medium");
            modes.append("high");
            const auto features = at(514, 65532);
            if (sequence.toInt() >= 2 && sequence.toInt() <= 4
                && (!integer(features, 0, 0xffff) || (features.toInt() & 2))) modes.append("auto");
            add("fanMode", modes);
        }
        const QMap<int, QString> names{{0,"off"},{1,"low"},{2,"medium"},{3,"high"},{4,"on"},{5,"auto"},{6,"smart"}};
        if (integer(mode, 0, 6)) d.controlState.insert("fanMode", names.value(mode.toInt()));
    }
    auto sensorNumber = [&](int cluster, int attribute, const QString &key, int lo, int hi, double divisor = 1.0) {
        const auto v = at(cluster, attribute);
        if (servers.contains(cluster) && integer(v, lo, hi)) d.controlState.insert(key, v.toDouble()/divisor);
    };
    sensorNumber(1026, 0, "temperature", -27315, 32767, 100);
    sensorNumber(513, 0, "temperature", -27315, 32767, 100);
    sensorNumber(1029, 0, "humidity", 0, 10000, 100);
    sensorNumber(47, 12, "battery", 0, 200, 2);
    if (servers.contains(69) && at(69, 0).isBool())
        d.controlState.insert(types(attrs, ep).contains(0x15) ? "contact" : "booleanState", at(69, 0));
    if (servers.contains(1030) && integer(at(1030, 0), 0, 1)) d.controlState.insert("occupancy", at(1030, 0).toInt() == 1);
    if (servers.contains(1024) && integer(at(1024, 0), 0, 65534))
        d.controlState.insert("illuminance", at(1024, 0).toInt() == 0 ? 0.0 : qPow(10.0, (at(1024, 0).toDouble()-1.0)/10000.0));
    if (servers.contains(91)) {
        if (d.controls.isEmpty()) d.kind = "sensor";
        static const QStringList labels{"未知", "优", "良", "中等", "差", "很差", "极差"};
        const auto quality = at(91, 0);
        d.readings << QStringLiteral("空气质量  %1").arg(integer(quality, 0, 6) ? labels.at(quality.toInt()) : labels.first());
        if (integer(quality, 1, 6)) d.controlState.insert("airQuality", quality);
    }
    const QList<QPair<int, QString>> concentrations{{0x40d,"CO₂"},{0x42a,"PM2.5"},{0x42b,"甲醛"},{0x42e,"TVOC"},{0x42c,"PM1"},{0x42d,"PM10"}};
    static const QStringList units{"ppm", "ppb", "ppt", "mg/m³", "µg/m³", "ng/m³", "p/m³", "Bq/m³"};
    for (const auto &measurement : concentrations) {
        if (!servers.contains(measurement.first)) continue;
        if (d.controls.isEmpty()) d.kind = "sensor";
        const auto value = at(measurement.first, 0), unit = at(measurement.first, 8);
        const bool known = value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0 && integer(unit, 0, 7);
        d.readings << (known ? QStringLiteral("%1  %2 %3").arg(measurement.second).arg(value.toDouble(), 0, 'f', 1).arg(units.at(unit.toInt()))
            : measurement.second + QStringLiteral("  未知"));
        if (known && measurement.first == 0x40d && unit.toInt() == 0) d.controlState.insert("co2", value);
        if (known && measurement.first == 0x42a && unit.toInt() == 4) d.controlState.insert("pm25", value);
    }
    if (servers.contains(144)) {
        const auto power = at(144, 8);
        if (power.isDouble() && std::isfinite(power.toDouble())) {
            d.readings << QStringLiteral("功率  %1 W").arg(power.toDouble()/1000.0, 0, 'f', 1);
            d.controlState.insert("powerWatts", power.toDouble()/1000.0);
        }
    }
    if (servers.contains(145)) {
        auto energy = at(145, 1).toObject().value("energy");
        if (energy.isUndefined()) energy = at(145, 1).toObject().value("0");
        if (energy.isDouble() && std::isfinite(energy.toDouble()) && energy.toDouble() >= 0) {
            d.readings << QStringLiteral("累计用电  %1 kWh").arg(energy.toDouble()/1000000.0, 0, 'f', 3);
            d.controlState.insert("energyKwh", energy.toDouble()/1000000.0);
        }
    }
}
}

QList<MatterDevice> devicesFromNodes(const QJsonArray &nodes) {
    QList<MatterDevice> devices;
    for (const auto &entry : nodes) {
        const auto node = entry.toObject();
        if (!node.value(QStringLiteral("node_id")).isDouble()) continue;
        const qint64 nodeId = node.value(QStringLiteral("node_id")).toInteger();
        const auto attributes = node.value(QStringLiteral("attributes")).toObject();
        QString nodeName = textAttribute(attributes, 0, 40, 5);
        if (nodeName.isEmpty()) nodeName = textAttribute(attributes, 0, 40, 3);
        if (nodeName.isEmpty()) nodeName = QStringLiteral("Matter 节点 %1").arg(nodeId);
        QSet<int> endpoints;
        for (auto it = attributes.begin(); it != attributes.end(); ++it) {
            const auto parts = it.key().split('/');
            bool ok = false;
            const int endpoint = parts.value(0).toInt(&ok);
            if (ok && parts.size() == 3 && endpoint > 0) endpoints.insert(endpoint);
        }
        auto orderedEndpoints = endpoints.values();
        std::sort(orderedEndpoints.begin(), orderedEndpoints.end());
        for (int endpoint : orderedEndpoints) {
            const auto servers = clusterIds(attributes.value(path(endpoint, 29, 1)));
            const auto deviceTypes = types(attributes, endpoint);
            // Aggregator infrastructure has no device tile. Unknown real device
            // endpoints remain inspectable, but never acquire inferred controls.
            if (deviceTypes.contains(0x000e)) continue;
            if (servers.isEmpty() && deviceTypes.isEmpty()) continue;
            MatterDevice device;
            device.nodeId = nodeId;
            device.endpointId = endpoint;
            device.key = QStringLiteral("%1/%2").arg(nodeId).arg(endpoint);
            device.nodeName = nodeName;
            device.name = textAttribute(attributes, endpoint, 57, 5);
            if (device.name.isEmpty()) device.name = textAttribute(attributes, endpoint, 40, 5);
            if (device.name.isEmpty()) device.name = textAttribute(attributes, endpoint, 57, 3);
            if (device.name.isEmpty()) device.name = QStringLiteral("%1 · 端点 %2").arg(nodeName).arg(endpoint);
            device.available = reachable(node, attributes, endpoint);
            // Descriptor.ServerList distinguishes server (controllable) clusters
            // from client clusters, e.g. a physical controller's OnOff binding.
            device.hasOnOff = servers.contains(6);
            device.hasLevel = servers.contains(8);
            const auto colorCapabilities = attributes.value(path(endpoint, 768, 16394));
            const auto colorFeatures = attributes.value(path(endpoint, 768, 65532));
            device.hasColorTemperature = servers.contains(768)
                && (integer(colorFeatures, 0, 0xffff) ? (colorFeatures.toInt() & 0x10)
                    : integer(colorCapabilities, 0, 0xffff) ? (colorCapabilities.toInt() & 0x10)
                    : attributes.contains(path(endpoint, 768, 7)));
            const auto on = attributes.value(path(endpoint, 6, 0));
            device.onKnown = on.isBool();
            device.on = on.toBool();
            const auto level = attributes.value(path(endpoint, 8, 0));
            device.levelKnown = validNumber(level, 0, 254);
            if (device.levelKnown) device.level = level.toInt();
            const auto colorTemp = attributes.value(path(endpoint, 768, 7));
            device.colorTemperatureKnown = validNumber(colorTemp, 1, 0xfffe);
            if (device.colorTemperatureKnown) device.colorTemperature = colorTemp.toInt();
            const bool isLight = deviceTypes.contains(0x100) || deviceTypes.contains(0x101)
                || deviceTypes.contains(0x10c) || deviceTypes.contains(0x10d)
                || deviceTypes.contains(0x10e) || device.hasLevel || device.hasColorTemperature;
            device.kind = isLight ? QStringLiteral("light")
                : deviceTypes.contains(0x10a) || deviceTypes.contains(0x10b) ? QStringLiteral("outlet")
                : device.hasOnOff ? QStringLiteral("switch") : QStringLiteral("device");
            const auto temperature = attributes.value(path(endpoint, 1026, 0));
            if (servers.contains(1026)) {
                device.kind = QStringLiteral("sensor");
                device.readings << (validNumber(temperature, -27315, 32767)
                    ? QStringLiteral("温度  %1 °C").arg(temperature.toDouble() / 100.0, 0, 'f', 1)
                    : QStringLiteral("温度  未知"));
            }
            const auto humidity = attributes.value(path(endpoint, 1029, 0));
            if (servers.contains(1029)) {
                device.kind = QStringLiteral("sensor");
                device.readings << (validNumber(humidity, 0, 10000)
                    ? QStringLiteral("湿度  %1 %").arg(humidity.toDouble() / 100.0, 0, 'f', 1)
                    : QStringLiteral("湿度  未知"));
            }
            const auto occupancy = attributes.value(path(endpoint, 1030, 0));
            if (servers.contains(1030)) {
                device.kind = QStringLiteral("sensor");
                device.readings << (!occupancy.isDouble() ? QStringLiteral("人体  未知")
                    : occupancy.toInt() & 1 ? QStringLiteral("人体  有人") : QStringLiteral("人体  无人"));
            }
            const auto booleanState = attributes.value(path(endpoint, 69, 0));
            if (servers.contains(69)) {
                device.kind = QStringLiteral("sensor");
                if (deviceTypes.contains(0x15)) {
                    device.readings << (!booleanState.isBool() ? QStringLiteral("门窗  未知")
                        : booleanState.toBool() ? QStringLiteral("门窗  关闭") : QStringLiteral("门窗  打开"));
                } else {
                    device.readings << (!booleanState.isBool() ? QStringLiteral("状态  未知")
                        : booleanState.toBool() ? QStringLiteral("状态  是") : QStringLiteral("状态  否"));
                }
            }
            const auto illuminance = attributes.value(path(endpoint, 1024, 0));
            if (servers.contains(1024)) {
                device.kind = QStringLiteral("sensor");
                device.readings << (validNumber(illuminance, 0, 65534)
                    ? QStringLiteral("照度  %1 lx").arg(illuminance.toInt() == 0 ? 0.0
                        : qPow(10.0, (illuminance.toDouble() - 1.0) / 10000.0), 0, 'f', 0)
                    : QStringLiteral("照度  未知"));
            }
            const auto battery = attributes.value(path(endpoint, 47, 12));
            if (validNumber(battery, 0, 200))
                device.readings << QStringLiteral("电池  %1 %").arg(battery.toDouble() / 2.0, 0, 'f', 0);
            // Camera capability requires the genuine Matter Camera device type and
            // both mandatory server clusters. Client bindings or a named switch
            // are never sufficient evidence of a camera.
            device.hasCamera = deviceTypes.contains(0x0142)
                && servers.contains(0x0551) && servers.contains(0x0553);
            constexpr quint32 PtzCluster = 0xFFF1FC01u;
            device.hasCameraPtz = device.hasCamera && endpoint == 1 && servers.contains(PtzCluster);
            if (device.hasCameraPtz) {
                const auto supported = attributes.value(path(endpoint, PtzCluster, 0));
                const auto available = attributes.value(path(endpoint, PtzCluster, 1));
                const auto mode = attributes.value(path(endpoint, PtzCluster, 2));
                const auto directions = attributes.value(path(endpoint, PtzCluster, 3));
                const auto commands = attributes.value(path(endpoint, PtzCluster, 65529));
                device.cameraPtzKnown = supported.isBool() && available.isBool()
                    && integer(mode, 0, 0) && integer(directions, 0, 15)
                    && commands.isArray() && commands.toArray().contains(QJsonValue(0));
                device.cameraPtzSupported = device.cameraPtzKnown && supported.toBool();
                device.cameraPtzAvailable = device.cameraPtzSupported && available.toBool();
                if (device.cameraPtzKnown) device.cameraPtzDirections = quint8(directions.toInt());
            }
            if (device.hasCamera) device.kind = QStringLiteral("camera");
            homeCapabilities(device, attributes, servers);
            if (device.hasCamera) device.kind = QStringLiteral("camera");
            const auto prefix = QString::number(endpoint) + '/';
            for (auto it = attributes.begin(); it != attributes.end(); ++it)
                if (it.key().startsWith(prefix)) device.attributes.insert(it.key(), it.value());
            devices.append(device);
        }
    }
    return devices;
}

QJsonArray demoNodes() {
    QJsonObject attrs{{"0/40/5", "Homebridge · 我的家"}, {"0/40/3", "Homebridge Matter Bridge"},
                      {"0/29/0", QJsonArray{QJsonObject{{"deviceType", 0x16}, {"revision", 1}}}},
                      {"1/29/0", QJsonArray{QJsonObject{{"deviceType", 0x0e}, {"revision", 1}}}},
                      {"1/29/1", QJsonArray{29}}, {"1/29/3", QJsonArray{2, 3, 4, 5, 6, 7}}};
    auto endpoint = [&](int ep, QString name, int type, QJsonArray servers, bool available = true) {
        attrs.insert(path(ep, 29, 0), QJsonArray{QJsonObject{{"deviceType", type}, {"revision", 1}}});
        servers.append(29); servers.append(57);
        attrs.insert(path(ep, 29, 1), servers);
        attrs.insert(path(ep, 57, 5), name);
        attrs.insert(path(ep, 57, 17), available);
    };
    endpoint(2, QStringLiteral("客厅落地灯"), 0x10c, {6, 8, 768});
    attrs.insert("2/6/0", true); attrs.insert("2/8/0", 183); attrs.insert("2/768/7", 333);
    attrs.insert("2/768/16394", 16); attrs.insert("2/768/16395", 153); attrs.insert("2/768/16396", 500);
    endpoint(3, QStringLiteral("咖啡机插座"), 0x10a, {6}); attrs.insert("3/6/0", false);
    endpoint(4, QStringLiteral("室内温湿度"), 0x302, {1026, 1029, 47});
    attrs.insert("4/1026/0", 2460); attrs.insert("4/1029/0", 5800); attrs.insert("4/47/12", 184);
    endpoint(5, QStringLiteral("书房台灯"), 0x101, {6, 8});
    attrs.insert("5/6/0", true); attrs.insert("5/8/0", 108);
    endpoint(6, QStringLiteral("入户门传感器"), 0x15, {69, 47});
    attrs.insert("6/69/0", true); attrs.insert("6/47/12", 156);
    endpoint(7, QStringLiteral("卧室床头灯"), 0x100, {6}, false); attrs.insert("7/6/0", false);
    return {QJsonObject{{"node_id", 1}, {"available", true}, {"is_bridge", true}, {"attributes", attrs},
                        {"endpoint_parents", QJsonObject{{"1", 0}, {"2", 1}, {"3", 1}, {"4", 1}, {"5", 1}, {"6", 1}, {"7", 1}}}}};
}


QJsonArray homeDemoNodes() {
    auto node = demoNodes().first().toObject();
    auto attrs = node.value("attributes").toObject();
    auto parents = node.value("endpoint_parents").toObject();
    auto parts = attrs.value("1/29/3").toArray();
    auto endpoint = [&](int ep, QString name, int type, QJsonArray servers) {
        attrs.insert(path(ep, 29, 0), QJsonArray{QJsonObject{{"deviceType", type}, {"revision", 1}}});
        servers.append(29); servers.append(57);
        attrs.insert(path(ep, 29, 1), servers);
        attrs.insert(path(ep, 57, 5), name); attrs.insert(path(ep, 57, 17), true);
        parents.insert(QString::number(ep), 1); parts.append(ep);
    };
    endpoint(8, QStringLiteral("客厅彩光灯"), 0x10d, {6,8,768});
    attrs.insert("8/6/0", true); attrs.insert("8/8/0", 178);
    attrs.insert("8/768/65532", 19); attrs.insert("8/768/16394", 19);
    attrs.insert("8/768/0", 148); attrs.insert("8/768/16384", 38229); attrs.insert("8/768/1", 191);
    attrs.insert("8/768/7", 333); attrs.insert("8/768/16395", 153); attrs.insert("8/768/16396", 500);
    endpoint(9, QStringLiteral("入户门锁"), 0x0a, {257,47});
    attrs.insert("9/257/0", 1); attrs.insert("9/257/2", true); attrs.insert("9/47/12", 180);
    endpoint(10, QStringLiteral("客厅窗帘"), 0x202, {258});
    attrs.insert("10/258/65532", 5); attrs.insert("10/258/14", 3500); attrs.insert("10/258/10", 0);
    endpoint(11, QStringLiteral("卧室恒温器"), 0x301, {513});
    attrs.insert("11/513/65532", 35); attrs.insert("11/513/0", 2360); attrs.insert("11/513/27", 4); attrs.insert("11/513/28", 1);
    attrs.insert("11/513/18", 2100); attrs.insert("11/513/17", 2500); attrs.insert("11/513/25", 20);
    attrs.insert("11/513/3", 700); attrs.insert("11/513/4", 3000); attrs.insert("11/513/5", 1600); attrs.insert("11/513/6", 3200);
    endpoint(12, QStringLiteral("卧室风扇"), 0x2b, {514});
    attrs.insert("12/514/65532", 2); attrs.insert("12/514/0", 1); attrs.insert("12/514/1", 2);
    attrs.insert("12/514/2", 35); attrs.insert("12/514/3", 35);
    endpoint(13, QStringLiteral("室内空气质量"), 0x2c, {91,0x40d,0x42a});
    attrs.insert("13/91/0", 1); attrs.insert("13/1037/0", 650.0); attrs.insert("13/1037/8", 0);
    attrs.insert("13/1066/0", 8.0); attrs.insert("13/1066/8", 4);
    auto outlet = attrs.value("3/29/1").toArray(); outlet.append(144); outlet.append(145); attrs.insert("3/29/1", outlet);
    attrs.insert("3/144/8", 0); attrs.insert("3/145/1", QJsonObject{{"0", 1245000}});
    attrs.insert("1/29/3", parts); node.insert("attributes", attrs); node.insert("endpoint_parents", parents);
    return {node};
}
