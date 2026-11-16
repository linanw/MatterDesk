#pragma once

#include <QString>
#include <optional>

class QUrl;
class QWidget;

struct ControllerLogin {
    QString username;
    QString password;
};

// The caller has already validated the URL. Plain WebSocket never asks for credentials.
std::optional<ControllerLogin> askControllerLogin(const QUrl &url, QWidget *parent = nullptr);
