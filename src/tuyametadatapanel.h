#pragma once

#include "tuyametadata.h"
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

class TuyaMetadataPanel : public QWidget {
    Q_OBJECT
public:
    explicit TuyaMetadataPanel(TuyaMetadataManager *manager, QWidget *parent = nullptr);
    TuyaMetadataPanel(TuyaMetadataManager *manager, const MatterDevice &device, QWidget *parent = nullptr);
    void setDevice(const MatterDevice &device);
private:
    void build();
    void updateState();
    void clearRoomDraft();
    QJsonObject selectedSourceRoom() const;
    bool roomDraftMatchesSnapshot() const;
    void rebaseRoomDraft();
    MatterDevice selectedDevice() const;
    QPointer<TuyaMetadataManager> m_manager;
    MatterDevice m_fixed;
    bool m_updating = false;
    QString m_draftDevice, m_draftRevision, m_panelProfile;
    QString m_roomDraftDevice, m_draftRoom, m_roomDraftRevision, m_roomDraftEpoch, m_roomDraftServer, m_roomDraftHome;
    bool m_roomDirty = false;
    QLineEdit *m_url = nullptr, *m_token = nullptr, *m_name = nullptr, *m_roomName = nullptr;
    QCheckBox *m_enabled = nullptr;
    QComboBox *m_mode = nullptr, *m_home = nullptr, *m_device = nullptr, *m_source = nullptr, *m_room = nullptr;
    QLabel *m_status = nullptr, *m_values = nullptr, *m_hint = nullptr;
    QPushButton *m_connect = nullptr, *m_refresh = nullptr, *m_bind = nullptr, *m_follow = nullptr;
    QPushButton *m_rename = nullptr, *m_move = nullptr, *m_ack = nullptr, *m_reset = nullptr, *m_rebase = nullptr;
    QPushButton *m_renameRoom = nullptr;
};
