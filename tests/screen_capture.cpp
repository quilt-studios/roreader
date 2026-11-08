#include "interfaces/reader/capture/screen_capture.h"
#include "support/localization.h"
#include "support/main_thread.h"
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWidget>
using namespace kobalt;
class CaptureTests final : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QWidget host;
    mainthread::Interaction interactive{[this] { return host.isVisible(); }};
    QStringList outputs(const QString &pattern) const {
        return QDir(temporary.path() + "/recordings").entryList({pattern}, QDir::Files, QDir::Name);
    }
  private slots:
    void init() {
        QVERIFY(temporary.isValid());
        i18n::setLanguage("en");
        host.resize(800, 1080);
        host.setAutoFillBackground(true);
        auto palette = host.palette();
        palette.setColor(QPalette::Window, Qt::red);
        host.setPalette(palette);
        host.show();
        host.setProperty("roreader_capture_status", QString());
    }
    void cleanup() {
        reader::stopScreenRecording(&host);
        QTRY_VERIFY(!reader::screenCaptureBusy(&host));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void screenshotIsLosslessAndUnique() {
        const auto before = host.grab().toImage();
        const int count = outputs("*.png").size();
        QVERIFY(reader::captureScreenshot(&host, temporary.path()));
        QTRY_VERIFY(!reader::screenCaptureBusy(&host));
        QTRY_COMPARE(outputs("*.png").size(), count + 1);
        const auto file = temporary.path() + "/recordings/" + outputs("*.png").last();
        const QImage image(file);
        QCOMPARE(image.size(), before.size());
        QCOMPARE(image.pixelColor(400, 400), QColor(Qt::red));
        QVERIFY(reader::captureScreenshot(&host, temporary.path()));
        QTRY_VERIFY(!reader::screenCaptureBusy(&host));
        QTRY_COMPARE(outputs("*.png").size(), count + 2);
    }
    void videoHasPlayableFramesAndStops() {
        const int count = outputs("*.avi").size();
        bool responsive = false;
        QVERIFY(reader::startScreenRecording(&host, temporary.path()));
        QVERIFY(reader::screenCaptureActive(&host));
        QVERIFY(!reader::startScreenRecording(&host, temporary.path()));
        QTimer::singleShot(0, &host, [&] { responsive = true; });
        QTRY_VERIFY(responsive);
        QTest::qWait(650);
        auto palette = host.palette();
        palette.setColor(QPalette::Window, Qt::blue);
        host.setPalette(palette);
        QTest::qWait(650);
        auto *stop = host.findChild<QPushButton *>("rrRecordingStop");
        QVERIFY(stop && stop->isVisible());
        QTest::mouseClick(stop, Qt::LeftButton);
        QVERIFY(!reader::screenCaptureActive(&host));
        QTRY_VERIFY(!reader::screenCaptureBusy(&host));
        QTRY_COMPARE(outputs("*.avi").size(), count + 1);
        const auto file = temporary.path() + "/recordings/" + outputs("*.avi").last();
        QFile avi(file);
        QVERIFY(avi.open(QIODevice::ReadOnly));
        const auto bytes = avi.readAll();
        QCOMPARE(bytes.left(4), QByteArray("RIFF"));
        QCOMPARE(bytes.mid(8, 4), QByteArray("AVI "));
        QVERIFY(bytes.contains("MJPG") && bytes.contains("idx1"));
        QVERIFY(bytes.size() < 16 * 1024 * 1024);
        const auto destination = qEnvironmentVariable("ROREADER_CAPTURE_TEST_DIR");
        if (!destination.isEmpty()) {
            QVERIFY(QDir().mkpath(destination));
            QVERIFY(avi.copy(destination + "/capture.avi"));
        }
        if (QFileInfo::exists("/usr/bin/ffprobe")) {
            mainthread::AllowBlocking verification("test-only video decoder verification");
            QProcess probe;
            probe.start("/usr/bin/ffprobe",
                        {"-v", "error", "-select_streams", "v:0", "-show_streams", "-of", "json", file});
            QVERIFY(probe.waitForFinished(10000));
            QCOMPARE(probe.exitCode(), 0);
            const auto stream = QJsonDocument::fromJson(probe.readAllStandardOutput())
                                    .object()
                                    .value("streams")
                                    .toArray()
                                    .first()
                                    .toObject();
            QCOMPARE(stream.value("codec_name").toString(), QStringLiteral("mjpeg"));
            QCOMPARE(stream.value("r_frame_rate").toString(), QStringLiteral("2/1"));
            QVERIFY(stream.value("nb_frames").toString().toInt() >= 2);
            QCOMPARE(stream.value("width").toInt(), 800);
            if (QFileInfo::exists("/usr/bin/ffmpeg")) {
                QProcess decode;
                decode.start("/usr/bin/ffmpeg", {"-v","error","-i",file,"-f","null","-"});
                QVERIFY(decode.waitForFinished(10000));
                QCOMPARE(decode.exitCode(),0);
            }
        }
    }
    void hiddenHostStopsAndStorageHandoffWaits() {
        QVERIFY(reader::startScreenRecording(&host, temporary.path()));
        host.hide();
        QVERIFY(!reader::screenCaptureActive(&host));
        {
            mainthread::AllowBlocking asleep("test sleep boundary with input disabled");
            reader::waitForScreenCaptures(&host);
        }
        QVERIFY(!reader::screenCaptureBusy(&host));
    }
    void rejectsSymlinkDestination() {
        QTemporaryDir outside;
        const auto directory = temporary.path() + "/linked";
        QVERIFY(QDir().mkpath(directory));
        QVERIFY(QFile::link(outside.path(), directory + "/recordings"));
        QVERIFY(reader::captureScreenshot(&host, directory));
        QTRY_VERIFY(!reader::screenCaptureBusy(&host));
        QTRY_VERIFY(host.property("roreader_capture_failed").toBool());
        QCOMPARE(QDir(outside.path()).entryList(QDir::Files).size(), 0);
    }
    void captureDialogUsesSharedControls() {
        reader::openScreenCapture(&host, temporary.path());
        auto *dialog = host.findChild<QWidget *>("rrScreenCaptureDialog");
        QVERIFY(dialog && dialog->isVisible());
        QVERIFY(dialog->findChild<QPushButton *>("rrCaptureScreenshot"));
        QVERIFY(dialog->findChild<QPushButton *>("rrCaptureStart"));
        const auto destination = qEnvironmentVariable("ROREADER_CAPTURE_TEST_DIR");
        if (!destination.isEmpty()) {
            QVERIFY(QDir().mkpath(destination));
            QVERIFY(dialog->grab().save(destination + "/capture-dialog.png"));
        }
        QTest::mouseClick(dialog->findChild<QPushButton *>("rrCaptureClose"), Qt::LeftButton);
        QTRY_VERIFY(!dialog->isVisible());
    }
};
int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "minimal:enable_fonts:freetype");
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    CaptureTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "screen_capture.moc"
