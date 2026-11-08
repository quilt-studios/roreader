#pragma once
#include "platform/api/keyboard_controller.h"
#include <QString>
class QWidget;
namespace kobalt::reader {
// Full-screen, touch-accessible utilities shared by desktop and device hosts.
QWidget *openTools(QWidget *host, KeyboardFactory keyboard, const QString &directory = {});
bool toolsBusy(const QWidget *host);
void stopTools(QWidget *host);
bool dismissTools(QWidget *host);
} // namespace kobalt::reader
