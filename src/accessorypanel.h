#pragma once

#include "matterdevice.h"

#include <QDialog>
#include <QHash>
#include <QPointer>

class CameraMetadataPanel;
class HomeStore;
class MatterClient;
class QLabel;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QPushButton;
class QPlainTextEdit;
class QVBoxLayout;

// A live accessory sheet. Targets are kept separate from reported device state;
// changing a target never sends a command until its Apply button is pressed.
class AccessoryPanel : public QDialog {
    Q_OBJECT
public:
    explicit AccessoryPanel(MatterClient *client, HomeStore *store, MatterDevice device, QWidget *parent = nullptr);
    ~AccessoryPanel() override;
    MatterDevice device() const { return m_device; }

private:
    struct ControlRow;
    void refreshDevice();
    void refresh();
    void rebuildControls();
    void refreshMetadata();
    void saveMetadata();
    void apply(ControlRow *row);
    QJsonValue targetValue(const ControlRow *row) const;
    bool validTarget(const ControlRow *row) const;

    QPointer<MatterClient> m_client;
    QPointer<HomeStore> m_store;
    MatterDevice m_device;
    QString m_profile;
    QString m_controlSignature;
    bool m_present = true;
    bool m_metadataDirty = false;
    QLabel *m_title, *m_connection, *m_readings, *m_feedback;
    QLineEdit *m_name;
    QComboBox *m_room;
    QCheckBox *m_favorite, *m_hidden;
    QPushButton *m_save;
    QPlainTextEdit *m_raw;
    CameraMetadataPanel *m_cloudMetadata = nullptr;
    QWidget *m_controlsHost;
    QVBoxLayout *m_controlsLayout;
    QList<ControlRow *> m_rows;
    QHash<QString, ControlRow *> m_pending;
};
