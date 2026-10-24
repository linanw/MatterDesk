#pragma once

#include "camerametadata.h"
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;

// Cloud metadata and local HomeStore aliases are deliberately separate editors.
// Drafts keep their original revision until the user explicitly rebases them.
class CameraMetadataPanel : public QWidget {
    Q_OBJECT
public:
    explicit CameraMetadataPanel(CameraMetadataManager *manager, const MatterDevice &device,
                                 QWidget *parent = nullptr);
    void setDevice(const MatterDevice &device);
protected:
    void showEvent(QShowEvent *event) override;
private:
    void updateState();
    void markDirty(bool name);
    void resetDrafts();
    void rebaseDrafts();
    void submitName();
    void submitRoom();
    bool draftMatches(const CameraMetadataSnapshot &snapshot) const;
    QPointer<CameraMetadataManager> m_manager;
    MatterDevice m_device;
    CameraMetadataSnapshot m_baseline, m_lastSnapshot;
    QString m_profile, m_submittedOperation;
    bool m_hadSnapshot = false, m_nameDirty = false, m_roomDirty = false;
    bool m_revisionChanged = false;
    QLabel *m_nameValue, *m_homeValue, *m_roomValue, *m_observedValue, *m_status, *m_draftHint;
    QLineEdit *m_nameEdit;
    QComboBox *m_roomEdit;
    QCheckBox *m_sync;
    QPushButton *m_refresh, *m_rename, *m_move, *m_rebase, *m_reset;
};
