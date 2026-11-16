#include "controllerlogin.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>

std::optional<ControllerLogin> askControllerLogin(const QUrl &url, QWidget *parent) {
    if (url.scheme() != "wss") return ControllerLogin{};

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("MatterServer 登录"));
    auto *form = new QFormLayout(&dialog);
    auto *target = new QLabel(url.toDisplayString(QUrl::RemovePath), &dialog);
    target->setTextFormat(Qt::PlainText);
    form->addRow(target);
    auto *username = new QLineEdit(&dialog);
    username->setObjectName(QStringLiteral("controllerUsername"));
    auto *password = new QLineEdit(&dialog);
    password->setObjectName(QStringLiteral("controllerPassword"));
    password->setEchoMode(QLineEdit::Password);
    password->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
    form->addRow(QStringLiteral("用户名"), username);
    form->addRow(QStringLiteral("密码"), password);
    form->addRow(new QLabel(QStringLiteral("凭据只用于本次连接。若服务器不需要登录，可匿名连接。"), &dialog));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *signIn = buttons->addButton(QStringLiteral("登录"), QDialogButtonBox::AcceptRole);
    auto *anonymous = buttons->addButton(QStringLiteral("匿名连接"), QDialogButtonBox::ActionRole);
    const auto update = [=] {
        signIn->setEnabled(!username->text().trimmed().isEmpty() && !username->text().contains(':')
                          && !password->text().isEmpty());
    };
    QObject::connect(username, &QLineEdit::textChanged, &dialog, update);
    QObject::connect(password, &QLineEdit::textChanged, &dialog, update);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(anonymous, &QPushButton::clicked, &dialog, [&dialog] { dialog.done(2); });
    form->addRow(buttons);
    update();
    username->setFocus();
    const int result = dialog.exec();
    if (result == QDialog::Accepted) return ControllerLogin{username->text().trimmed(), password->text()};
    if (result == 2) return ControllerLogin{};
    return std::nullopt;
}
