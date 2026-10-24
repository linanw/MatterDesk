#include "camerametadatapanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QLabel *valueLabel(const char *name, QWidget *parent) {
    auto *label = new QLabel(QStringLiteral("尚未读取"), parent);
    label->setObjectName(name); label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true); label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
bool sameSource(const CameraMetadataSnapshot &a, const CameraMetadataSnapshot &b) {
    return a.identity == b.identity && a.sourceEpoch == b.sourceEpoch && a.homeKey == b.homeKey;
}
QString observedText(const QString &value) {
    const auto date = QDateTime::fromString(value, Qt::ISODateWithMs);
    return date.isValid() ? date.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : value;
}
}

CameraMetadataPanel::CameraMetadataPanel(CameraMetadataManager *manager, const MatterDevice &device,
                                       QWidget *parent)
    : QWidget(parent), m_manager(manager), m_device(device) {
    setObjectName("cameraMetadataPanel");
    m_profile = manager ? manager->profile() : QString();
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(16, 15, 16, 15); layout->setSpacing(10);
    auto *title = new QLabel(QStringLiteral("米家设备信息"), this);
    title->setObjectName("sectionTitle"); layout->addWidget(title);
    auto *hint = new QLabel(QStringLiteral("以下名称和房间来自米家 App；本机别名与本机房间另行保存。"), this);
    hint->setObjectName("muted"); hint->setWordWrap(true); layout->addWidget(hint);
    auto *form = new QFormLayout; form->setSpacing(8);
    m_nameValue = valueLabel("cloudMetadataName", this);
    m_homeValue = valueLabel("cloudMetadataHome", this);
    m_roomValue = valueLabel("cloudMetadataRoom", this);
    m_observedValue = valueLabel("cloudMetadataObservedAt", this);
    form->addRow(QStringLiteral("米家名称"), m_nameValue);
    form->addRow(QStringLiteral("米家家庭"), m_homeValue);
    form->addRow(QStringLiteral("米家房间"), m_roomValue);
    form->addRow(QStringLiteral("上次读取"), m_observedValue); layout->addLayout(form);
    auto *syncLine = new QHBoxLayout;
    m_sync = new QCheckBox(QStringLiteral("自动同步米家信息"), this);
    m_sync->setObjectName("cloudMetadataSync");
    m_sync->setToolTip(QStringLiteral("由插件定期读取米家信息；不会将本机别名回写米家。")); syncLine->addWidget(m_sync, 1);
    m_refresh = new QPushButton(QStringLiteral("刷新米家信息"), this);
    m_refresh->setObjectName("cloudMetadataRefresh"); syncLine->addWidget(m_refresh); layout->addLayout(syncLine);
    m_status = valueLabel("cloudMetadataStatus", this); layout->addWidget(m_status);

    auto *editForm = new QFormLayout; editForm->setSpacing(8);
    auto *nameLine = new QHBoxLayout;
    m_nameEdit = new QLineEdit(this); m_nameEdit->setObjectName("cloudMetadataNameDraft");
    m_nameEdit->setAccessibleName(QStringLiteral("新的米家名称")); m_nameEdit->setMaxLength(160);
    nameLine->addWidget(m_nameEdit, 1);
    m_rename = new QPushButton(QStringLiteral("修改米家名称"), this);
    m_rename->setObjectName("cloudMetadataRename"); nameLine->addWidget(m_rename);
    editForm->addRow(QStringLiteral("名称目标"), nameLine);
    auto *roomLine = new QHBoxLayout;
    m_roomEdit = new QComboBox(this); m_roomEdit->setObjectName("cloudMetadataRoomDraft");
    m_roomEdit->setAccessibleName(QStringLiteral("新的米家所属房间")); roomLine->addWidget(m_roomEdit, 1);
    m_move = new QPushButton(QStringLiteral("修改所属房间"), this);
    m_move->setObjectName("cloudMetadataMove"); roomLine->addWidget(m_move);
    editForm->addRow(QStringLiteral("房间目标"), roomLine); layout->addLayout(editForm);
    auto *editHint = new QLabel(QStringLiteral("只有插件确认写入权限后才能修改；仅可移到米家已有房间，未分房只作显示。"), this);
    editHint->setObjectName("cloudMetadataWriteHint"); editHint->setWordWrap(true); layout->addWidget(editHint);
    m_draftHint = valueLabel("cloudMetadataDraftStatus", this); layout->addWidget(m_draftHint);
    auto *draftLine = new QHBoxLayout; draftLine->addStretch();
    m_rebase = new QPushButton(QStringLiteral("使用最新版本继续"), this); m_rebase->setObjectName("cloudMetadataRebase");
    m_reset = new QPushButton(QStringLiteral("还原草稿"), this); m_reset->setObjectName("cloudMetadataReset");
    draftLine->addWidget(m_rebase); draftLine->addWidget(m_reset); layout->addLayout(draftLine);

    connect(m_nameEdit, &QLineEdit::textEdited, this, [this] { markDirty(true); });
    connect(m_roomEdit, &QComboBox::activated, this, [this] { markDirty(false); });
    connect(m_rebase, &QPushButton::clicked, this, &CameraMetadataPanel::rebaseDrafts);
    connect(m_reset, &QPushButton::clicked, this, &CameraMetadataPanel::resetDrafts);
    connect(m_rename, &QPushButton::clicked, this, &CameraMetadataPanel::submitName);
    connect(m_move, &QPushButton::clicked, this, &CameraMetadataPanel::submitRoom);
    connect(m_refresh, &QPushButton::clicked, this, [this] {
        if (!m_manager) return;
        const auto state = m_manager->state(m_device);
        if (state.known && !state.stale && state.capabilities.read)
            m_manager->refresh(m_device, state.snapshot);
        else m_manager->read(m_device);
        updateState();
    });
    connect(m_sync, &QCheckBox::clicked, this, [this](bool enabled) {
        if (!m_manager) return;
        const auto state = m_manager->state(m_device);
        m_submittedOperation = QStringLiteral("sync");
        if (!m_manager->setSync(m_device, enabled, state.snapshot)) m_submittedOperation.clear();
        updateState();
    });
    if (manager) {
        connect(manager, &CameraMetadataManager::changed, this, [this](const QString &key) {
            if (key.isEmpty() || key == m_device.key) updateState();
        });
        connect(manager, &CameraMetadataManager::operationFinished, this,
            [this](const QString &key, const QString &operation, bool success, const QString &) {
                if (key != m_device.key || operation != m_submittedOperation) return;
                m_submittedOperation.clear();
                if (success) {
                    if (operation == QStringLiteral("rename")) m_nameDirty = false;
                    if (operation == QStringLiteral("move")) m_roomDirty = false;
                    if (!m_nameDirty && !m_roomDirty && m_manager) {
                        m_baseline = m_manager->state(m_device).snapshot; m_revisionChanged = false;
                    }
                }
                updateState();
            });
        connect(manager, &QObject::destroyed, this, [this] { updateState(); });
    }
    updateState();
}

void CameraMetadataPanel::setDevice(const MatterDevice &device) {
    const bool replaced = device.key != m_device.key || device.nodeId != m_device.nodeId
        || device.endpointId != m_device.endpointId || device.controllerGeneration != m_device.controllerGeneration;
    m_device = device;
    if (replaced) {
        m_hadSnapshot = false; m_nameDirty = m_roomDirty = m_revisionChanged = false;
        m_baseline = {}; m_lastSnapshot = {}; m_submittedOperation.clear();
    }
    updateState();
}

void CameraMetadataPanel::showEvent(QShowEvent *event) {
    QWidget::showEvent(event); updateState();
    QTimer::singleShot(0, this, [this] { if (m_manager && isVisible()) m_manager->read(m_device); });
}

bool CameraMetadataPanel::draftMatches(const CameraMetadataSnapshot &snapshot) const {
    return sameSource(m_baseline, snapshot) && m_baseline.revision == snapshot.revision;
}

void CameraMetadataPanel::markDirty(bool name) {
    if (!m_manager) return;
    if (!m_nameDirty && !m_roomDirty) m_baseline = m_manager->state(m_device).snapshot;
    if (name) m_nameDirty = true; else m_roomDirty = true;
    updateState();
}

void CameraMetadataPanel::resetDrafts() {
    if (!m_manager || m_manager->state(m_device).busy) return;
    m_nameDirty = m_roomDirty = m_revisionChanged = false;
    m_baseline = m_manager->state(m_device).snapshot; updateState();
}

void CameraMetadataPanel::rebaseDrafts() {
    if (!m_manager) return;
    const auto state = m_manager->state(m_device);
    if (!state.known || state.stale || state.busy || !sameSource(m_baseline, state.snapshot)) return;
    m_baseline = state.snapshot; m_revisionChanged = false; updateState();
}

void CameraMetadataPanel::submitName() {
    if (!m_manager || !m_rename->isEnabled()) return;
    m_submittedOperation = QStringLiteral("rename");
    if (!m_manager->rename(m_device, m_nameEdit->text().trimmed(), m_baseline)) m_submittedOperation.clear();
    updateState();
}
void CameraMetadataPanel::submitRoom() {
    if (!m_manager || !m_move->isEnabled()) return;
    m_submittedOperation = QStringLiteral("move");
    if (!m_manager->moveRoom(m_device, m_roomEdit->currentData().toString(), m_baseline)) m_submittedOperation.clear();
    updateState();
}

void CameraMetadataPanel::updateState() {
    const auto state = m_manager ? m_manager->state(m_device) : CameraMetadataState{};
    const bool profileChanged = m_manager && m_profile != m_manager->profile();
    if (profileChanged) m_profile = m_manager->profile();
    const bool replaced = profileChanged || (state.known && m_hadSnapshot && !sameSource(m_lastSnapshot, state.snapshot));
    if (replaced || (!state.known && m_hadSnapshot)) {
        m_nameDirty = m_roomDirty = m_revisionChanged = false; m_submittedOperation.clear(); m_baseline = {};
        m_hadSnapshot = false;
    }
    if (state.known) {
        if (!m_hadSnapshot || (!m_nameDirty && !m_roomDirty)) m_baseline = state.snapshot;
        else if (!draftMatches(state.snapshot)) m_revisionChanged = true;
        m_hadSnapshot = true; m_lastSnapshot = state.snapshot;
    }
    const bool incomplete = cameraMetadataUnavailable(state);
    m_nameValue->setText(incomplete ? QStringLiteral("暂不可用") : state.known ? state.snapshot.name : QStringLiteral("尚未读取"));
    m_homeValue->setText(incomplete ? QStringLiteral("暂不可用") : state.known ? state.snapshot.homeName : QStringLiteral("尚未读取"));
    m_roomValue->setText(incomplete ? QStringLiteral("暂不可用") : state.known ? state.snapshot.roomId.isEmpty() ? QStringLiteral("未分房") : state.snapshot.roomName : QStringLiteral("尚未读取"));
    m_observedValue->setText(incomplete && state.snapshot.observedAt.isEmpty() ? QStringLiteral("暂不可用")
        : state.known ? observedText(state.snapshot.observedAt) : QStringLiteral("尚未读取"));
    {
        const QSignalBlocker nameBlock(m_nameEdit), roomBlock(m_roomEdit), syncBlock(m_sync);
        if (!m_nameDirty) m_nameEdit->setText(state.known ? state.snapshot.name : QString());
        const QString wantedRoom = m_roomDirty ? m_roomEdit->currentData().toString() : state.snapshot.roomId;
        const QString wantedText = m_roomDirty ? m_roomEdit->currentText() : QString();
        m_roomEdit->clear(); m_roomEdit->addItem(QStringLiteral("未分房（仅显示）"), QString());
        if (auto *model = qobject_cast<QStandardItemModel *>(m_roomEdit->model()))
            if (auto *item = model->item(0)) item->setFlags(Qt::NoItemFlags);
        for (const auto &room : state.rooms) m_roomEdit->addItem(room.name, room.id);
        int index = m_roomEdit->findData(wantedRoom);
        if (index < 0 && m_roomDirty) { m_roomEdit->addItem(wantedText + QStringLiteral("（房间已不可用）"), wantedRoom); index = m_roomEdit->count() - 1; }
        m_roomEdit->setPlaceholderText(incomplete ? QStringLiteral("暂不可用") : QStringLiteral("选择米家已有房间"));
        m_roomEdit->setCurrentIndex(incomplete && !m_roomDirty ? -1 : index);
        m_nameEdit->setPlaceholderText(incomplete ? QStringLiteral("暂不可用") : QStringLiteral("新的米家名称"));
        m_sync->setChecked(state.syncEnabled);
    }
    const bool current = state.known && !state.stale && state.available && !state.busy && !state.loading;
    const bool baselineCurrent = current && !m_revisionChanged && draftMatches(state.snapshot);
    const bool canName = m_manager && m_manager->canRename(m_device);
    const bool canRoom = m_manager && m_manager->canMoveRoom(m_device);
    m_nameEdit->setEnabled(canName && !state.busy && !state.loading);
    m_roomEdit->setEnabled(canRoom && !state.busy && !state.loading);
    bool roomExists = false;
    for (const auto &room : state.rooms) if (room.id == m_roomEdit->currentData().toString()) roomExists = true;
    const auto nameTarget = m_nameEdit->text().trimmed();
    const bool nameTooLong = nameTarget.toUtf8().size() > 160;
    m_rename->setEnabled(canName && baselineCurrent && m_nameDirty && !nameTarget.isEmpty() && !nameTooLong);
    m_move->setEnabled(canRoom && baselineCurrent && m_roomDirty && m_roomEdit->currentIndex() >= 0
        && !m_roomEdit->currentData().toString().isEmpty() && roomExists);
    m_sync->setEnabled(m_manager && m_manager->canSetSync(m_device) && current);
    m_refresh->setEnabled(m_manager && m_manager->canRead(m_device) && !state.busy && !state.loading);
    m_reset->setVisible(m_nameDirty || m_roomDirty); m_reset->setEnabled(!state.busy);
    m_rebase->setVisible((m_nameDirty || m_roomDirty) && m_revisionChanged);
    m_rebase->setEnabled(current && sameSource(m_baseline, state.snapshot));
    QString status = state.status;
    if (status.isEmpty()) status = state.known ? state.stale ? QStringLiteral("信息已过期，请重新读取。")
        : state.available ? QStringLiteral("已读取米家信息。") : QStringLiteral("米家信息当前不可用。")
        : QStringLiteral("连接支持米家元数据的摄像头后可读取。此操作不会修改本机设置。");
    if (!state.lastError.isEmpty() && !status.contains(state.lastError)) status += QStringLiteral(" · ") + state.lastError;
    m_status->setText(status);
    m_status->setProperty("error", state.conflict || !state.lastError.isEmpty());
    m_status->setStyleSheet(state.conflict || !state.lastError.isEmpty() ? QStringLiteral("color:#a23e38") : QString());
    if (m_revisionChanged) m_draftHint->setText(QStringLiteral("米家信息已更新，草稿已保留。请核对当前信息，再点击“使用最新版本继续”。"));
    else if (state.busy && !m_submittedOperation.isEmpty()) m_draftHint->setText(QStringLiteral("正在等待米家权威信息回读；命令已接受不等于已修改。"));
    else if (state.conflict && (m_nameDirty || m_roomDirty)) m_draftHint->setText(QStringLiteral("版本冲突，草稿已保留。请刷新信息后核对再提交。"));
    else if (m_nameDirty || m_roomDirty) m_draftHint->setText(QStringLiteral("草稿仅在本窗口，点击对应修改按钮才会写入米家。"));
    else if (replaced) m_draftHint->setText(QStringLiteral("摄像头或数据来源已更换，旧草稿已清除。"));
    else m_draftHint->clear();
    if (m_nameDirty && nameTooLong) {
        const auto existing = m_draftHint->text();
        m_draftHint->setText(QStringLiteral("名称最多支持 160 字节，请缩短后再提交。")
            + (existing.isEmpty() ? QString() : QStringLiteral("\n") + existing));
    }
    m_draftHint->setVisible(!m_draftHint->text().isEmpty());
}
