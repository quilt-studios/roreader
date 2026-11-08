#include "screen_capture.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/style/styles.h"
#include "support/localization.h"
#include "support/main_thread.h"
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QImageWriter>
#include <QLabel>
#include <QMutex>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QWaitCondition>
#include <QWidget>
#include <atomic>

namespace kobalt::reader {
namespace {
namespace m = ui::metrics;
constexpr int fps = 2, maximumFrames = 60;
constexpr qint64 maximumBytes = 16 * 1024 * 1024;
QByteArray u32(quint32 n) {
    QByteArray bytes(4, '\0');
    for (int i = 0; i < 4; ++i)
        bytes[i] = char(n >> (8 * i));
    return bytes;
}
QByteArray u16(quint16 n) {
    return QByteArray(1, char(n)) + QByteArray(1, char(n >> 8));
}
QByteArray chunk(const char *tag, QByteArray data) {
    const auto length = data.size();
    if (length & 1)
        data += '\0';
    return QByteArray(tag, 4) + u32(length) + data;
}
// RIFF AVI 1.0, one independent JPEG keyframe per video sample. No codec SDK is needed on the reader.
bool writeAvi(QIODevice &output, const QVector<QByteArray> &frames, QSize size) {
    quint32 largest = 0, movieBytes = 0;
    for (const auto &frame : frames) {
        largest = qMax(largest, quint32(frame.size()));
        movieBytes += 8 + frame.size() + (frame.size() & 1);
    }
    QByteArray avih;
    for (quint32 value : {quint32(1000000 / fps), largest * fps, 0u, 0x10u, quint32(frames.size()), 0u, 1u, largest,
                          quint32(size.width()), quint32(size.height()), 0u, 0u, 0u, 0u})
        avih += u32(value);
    QByteArray strh("vidsMJPG", 8);
    strh += u32(0) + u16(0) + u16(0);
    for (quint32 value : {0u, 1u, quint32(fps), 0u, quint32(frames.size()), largest, 0xffffffffu, 0u})
        strh += u32(value);
    strh += u16(0) + u16(0) + u16(size.width()) + u16(size.height());
    QByteArray strf = u32(40) + u32(size.width()) + u32(size.height()) + u16(1) + u16(24) + QByteArray("MJPG", 4);
    for (quint32 value : {quint32(size.width() * size.height() * 3), 0u, 0u, 0u, 0u})
        strf += u32(value);
    const auto hdrl =
        chunk("LIST", QByteArray("hdrl", 4) + chunk("avih", avih) +
                          chunk("LIST", QByteArray("strl", 4) + chunk("strh", strh) + chunk("strf", strf)));
    const quint32 indexBytes = frames.size() * 16;
    auto write = [&](const QByteArray &bytes) { return output.write(bytes) == bytes.size(); };
    if (!write(QByteArray("RIFF", 4) + u32(4 + hdrl.size() + 12 + movieBytes + 8 + indexBytes) + "AVI " + hdrl +
               "LIST" + u32(4 + movieBytes) + "movi"))
        return false;
    QByteArray index;
    quint32 offset = 4;
    for (const auto &frame : frames) {
        if (!write(chunk("00dc", frame)))
            return false;
        index += QByteArray("00dc", 4) + u32(0x10) + u32(offset) + u32(frame.size());
        offset += 8 + frame.size() + (frame.size() & 1);
    }
    return write(chunk("idx1", index));
}
QString documentRoot(QWidget *host, QString directory) {
    if (directory.isEmpty())
        directory = host->property("roreader_tools_directory").toString();
    if (!directory.isEmpty())
        return directory;
    return QFileInfo::exists("/mnt/onboard/.kobo")
               ? QStringLiteral("/mnt/onboard/ROREADER")
               : QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/ROREADER";
}
class CaptureJob final : public QThread {
  public:
    QString root, path, error;
    QImage pending;
    int pendingSlot = 0;
    QSize dimensions;
    const bool video;
    QMutex mutex;
    QWaitCondition ready;
    std::atomic_bool stopping{false};
    std::atomic_int finalFrames{1};
    bool limited = false;
    explicit CaptureJob(QString root, bool video) : root(std::move(root)), video(video) {}
    bool submit(const QImage &image, int slot) {
        if (stopping.load() || !mutex.tryLock())
            return false;
        const bool accepted = !stopping.load() && pending.isNull();
        if (accepted) {
            pending = image;
            pendingSlot = slot;
            ready.wakeOne();
        }
        mutex.unlock();
        return accepted;
    }
    void stop(int count) {
        QMutexLocker lock(&mutex);
        finalFrames.store(qBound(1, count, maximumFrames));
        stopping.store(true);
        ready.wakeOne();
    }
    void run() override {
        mainthread::blocking("screen capture encoding and export");
        if (QFileInfo(root).isSymLink() || QFileInfo(root + "/recordings").isSymLink() ||
            !QDir().mkpath(root + "/recordings")) {
            error = i18n::text("Aufnahmeordner nicht verfügbar.");
            return;
        }
        path = root + "/recordings/" + (video ? "video-" : "screenshot-") +
               QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") + '-' +
               QUuid::createUuid().toString(QUuid::WithoutBraces) + (video ? ".avi" : ".png");
        if (QFileInfo::exists(path)) {
            error = i18n::text("Aufnahmedatei existiert bereits.");
            return;
        }
        QVector<QByteArray> frames;
        qint64 retained = 0;
        while (true) {
            QImage image;
            int slot;
            {
                QMutexLocker lock(&mutex);
                while (pending.isNull() && !stopping.load())
                    ready.wait(&mutex);
                if (pending.isNull())
                    break;
                image = std::move(pending);
                pending = {};
                slot = pendingSlot;
            }
            if (!video) {
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit())
                    error = i18n::text("Aufnahme konnte nicht gespeichert werden.");
                return;
            }
            if (dimensions.isEmpty())
                dimensions = image.size().scaled(QSize(800, 1080), Qt::KeepAspectRatio);
            QImage normalized(dimensions, QImage::Format_RGB32);
            normalized.fill(Qt::white);
            {
                QPainter painter(&normalized);
                const auto scaled = image.scaled(dimensions, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                painter.drawImage(
                    QPoint((dimensions.width() - scaled.width()) / 2, (dimensions.height() - scaled.height()) / 2),
                    scaled);
            }
            QByteArray jpeg;
            QBuffer buffer(&jpeg);
            buffer.open(QIODevice::WriteOnly);
            if (!normalized.save(&buffer, "JPEG", 75)) {
                error = i18n::text("JPEG-Encoder nicht verfügbar.");
                break;
            }
            while (frames.size() < slot && !frames.isEmpty() && frames.size() < maximumFrames &&
                   retained + frames.last().size() <= maximumBytes - 4096) {
                retained += frames.last().size();
                frames.append(frames.last());
            }
            if (frames.size() >= maximumFrames || retained + jpeg.size() > maximumBytes - 4096) {
                limited = true;
                break;
            }
            retained += jpeg.size();
            frames.append(jpeg);
            if (stopping.load()) {
                QMutexLocker lock(&mutex);
                if (pending.isNull())
                    break;
            }
        }
        if (frames.isEmpty()) {
            if (error.isEmpty())
                error = i18n::text("Keine Videobilder aufgenommen.");
            return;
        }
        while (frames.size() < finalFrames.load() && retained + frames.last().size() <= maximumBytes - 4096) {
            retained += frames.last().size();
            frames.append(frames.last());
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || !writeAvi(file, frames, dimensions) || !file.commit())
            error = i18n::text("Aufnahme konnte nicht gespeichert werden.");
    }
};
class Capture final : public QObject {
  public:
    QWidget *host;
    QTimer timer;
    QElapsedTimer clock;
    QPointer<CaptureJob> recording;
    QVector<QPointer<CaptureJob>> jobs;
    QPointer<ui::ModalOverlay> dialog;
    QPushButton *badge;
    explicit Capture(QWidget *host) : QObject(host), host(host) {
        setObjectName("rrScreenCaptureController");
        host->installEventFilter(this);
        badge = ui::makeButton(host, nullptr, "rrRecordingStop", {}, [this] { stop(); });
        ui::readerStyle(badge, QString("QPushButton{%1border:1px solid palette(mid);border-radius:%2px;"
                                       "background:palette(window);color:palette(window-text);}")
                                   .arg(ui::styles::font(m::chrome::labelText))
                                   .arg(ui::px(16)));
        badge->hide();
        timer.setInterval(500);
        connect(&timer, &QTimer::timeout, this, [this] {
            if (!recording || !recording->isRunning() || recording->stopping.load()) {
                timer.stop();
                return;
            }
            if (clock.elapsed() >= 30000) {
                stop();
                return;
            }
            badge->setText(i18n::text("Stoppen") + QString(" · %1 s").arg(clock.elapsed() / 1000));
            position();
            recording->submit(this->host->grab().toImage(), qMin(maximumFrames - 1, int(clock.elapsed() * fps / 1000)));
        });
    }
    ~Capture() override {
        if (recording)
            recording->stop(qMax(1, int((clock.elapsed() * fps + 999) / 1000)));
    }
    void position() {
        badge->setGeometry(qMax(0, host->width() - ui::px(336)), qMax(0, host->height() - ui::px(96)),
                           qMin(host->width(), ui::px(320)), ui::px(80));
        if (timer.isActive())
            badge->raise();
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == host && event->type() == QEvent::Resize)
            position();
        if (object == host && event->type() == QEvent::Hide)
            stop();
        return QObject::eventFilter(object, event);
    }
    void dismiss() {
        if (dialog) {
            dialog->hide();
            dialog->deleteLater();
            dialog.clear();
        }
    }
    void observe(CaptureJob *job) {
        jobs.append(job);
        connect(job, &QThread::finished, this, [this, job] {
            if (recording == job) {
                recording.clear();
                timer.stop();
                badge->hide();
            }
            const auto status = job->error.isEmpty() ? i18n::text("Gespeichert: ") + job->path : job->error;
            host->setProperty("roreader_capture_status", status);
            host->setProperty("roreader_capture_failed", !job->error.isEmpty());
            if (dialog) {
                if (auto *label = dialog->findChild<QLabel *>("rrCaptureStatus"))
                    label->setText(status);
            }
        });
        connect(job, &QThread::finished, job, &QObject::deleteLater);
    }
    bool screenshot(QString root) {
        if (!host->isVisible() || host->size().isEmpty() || busy())
            return false;
        dismiss();
        auto *job = new CaptureJob(documentRoot(host, std::move(root)), false);
        job->pending = host->grab().toImage();
        observe(job);
        job->start();
        return true;
    }
    bool start(QString root) {
        if (!host->isVisible() || host->size().isEmpty() || busy())
            return false;
        dismiss();
        auto *job = new CaptureJob(documentRoot(host, std::move(root)), true);
        job->pending = host->grab().toImage();
        observe(job);
        recording = job;
        clock.start();
        badge->setText(i18n::text("Stoppen") + " · 0 s");
        badge->setEnabled(true);
        badge->show();
        timer.start();
        position();
        job->start();
        return true;
    }
    void stop() {
        timer.stop();
        if (recording && recording->isRunning()) {
            recording->stop(qMax(1, int((clock.elapsed() * fps + 999) / 1000)));
            badge->setText(i18n::text("Speichern") + " …");
            badge->setEnabled(false);
        } else
            badge->hide();
    }
    bool busy() const {
        for (const auto &job : jobs)
            if (job && job->isRunning())
                return true;
        return false;
    }
    void wait() {
        stop();
        dismiss();
        for (const auto &job : jobs)
            if (job && job->isRunning()) {
                mainthread::blocking("finish screen capture before storage handoff");
                job->wait();
            }
    }
    void open(QString root) {
        if (dialog) {
            dialog->raise();
            return;
        }
        dialog = new ui::ModalOverlay(host);
        dialog->setObjectName("rrScreenCaptureDialog");
        dialog->setDimmed(true);
        dialog->setGeometry(host->rect());
        dialog->dismissed = [this] { dismiss(); };
        auto *card = dialog->card();
        card->title(i18n::text("Bildschirmaufnahme"));
        card->message(i18n::text("PNG-Screenshot oder AVI-Video der ROREADER-Ansicht. Video: 2 Bilder/s, bis zu 30 "
                                 "Sekunden und 16 MiB. Keine Tonaufnahme."));
        card->message(i18n::text("Speicherort: ROREADER/recordings. Das sichtbare Menü und der Stopp-Schalter können "
                                 "in der Aufnahme erscheinen."));
        auto *status =
            card->message(busy() && !timer.isActive() ? i18n::text("Datei wird verarbeitet …")
                                                      : host->property("roreader_capture_status").toString());
        status->setObjectName("rrCaptureStatus");
        auto *shot = ui::makeButton(card, nullptr, "rrCaptureScreenshot", i18n::text("Screenshot speichern"),
                                    [this, root] { screenshot(root); });
        shot->setEnabled(!busy());
        QPushButton *record = nullptr;
        if (timer.isActive())
            record = ui::makeButton(card, nullptr, "rrCaptureStop", i18n::text("Video stoppen"), [this] {
                stop();
                dismiss();
            });
        else {
            record = ui::makeButton(card, nullptr, "rrCaptureStart", i18n::text("Video starten"),
                                    [this, root] { start(root); });
            record->setEnabled(!busy());
        }
        card->adaptiveActions({shot, record});
        ui::makeButton(card, card->actions(), "rrCaptureClose", i18n::text("Zurück"), [this] { dismiss(); });
        dialog->show();
        dialog->raise();
        dialog->fitCard();
    }
};
Capture *controller(QWidget *host, bool create = false) {
    auto *object = host->findChild<QObject *>("rrScreenCaptureController", Qt::FindDirectChildrenOnly);
    return object ? static_cast<Capture *>(object) : create ? new Capture(host) : nullptr;
}
const Capture *controller(const QWidget *host) {
    return controller(const_cast<QWidget *>(host));
}
} // namespace
void openScreenCapture(QWidget *host, const QString &documents) {
    controller(host, true)->open(documents);
}
bool captureScreenshot(QWidget *host, const QString &documents) {
    return controller(host, true)->screenshot(documents);
}
bool startScreenRecording(QWidget *host, const QString &documents) {
    return controller(host, true)->start(documents);
}
void stopScreenRecording(QWidget *host) {
    if (auto *capture = controller(host))
        capture->stop();
}
bool screenCaptureActive(const QWidget *host) {
    const auto *capture = controller(host);
    return capture && capture->timer.isActive();
}
bool screenCaptureBusy(const QWidget *host) {
    const auto *capture = controller(host);
    return capture && capture->busy();
}
void waitForScreenCaptures(QWidget *host) {
    if (auto *capture = controller(host))
        capture->wait();
}
} // namespace kobalt::reader
