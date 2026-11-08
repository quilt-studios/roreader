#pragma once
#include <QString>
class QWidget;
namespace kobalt::reader {
void openScreenCapture(QWidget *host, const QString &documents = {});
bool captureScreenshot(QWidget *host, const QString &documents = {});
bool startScreenRecording(QWidget *host, const QString &documents = {});
void stopScreenRecording(QWidget *host);
bool screenCaptureActive(const QWidget *host);
bool screenCaptureBusy(const QWidget *host);
// Only at an existing sleep/closing boundary where input cannot arrive.
void waitForScreenCaptures(QWidget *host);
} // namespace kobalt::reader
