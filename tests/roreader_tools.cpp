#include "interfaces/reader/tools/tools_panel.h"
#include "interfaces/reader/ui/style/scale.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTextDocument>
#include <QBuffer>

using namespace kobalt;
class ToolsTests final : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QWidget host;
    QWidget *tools = nullptr;
    void click(const char *id) {
        auto *button = tools->findChild<QPushButton *>(id);
        QVERIFY(button);
        button->click();
    }
    QString status() { return tools->findChild<QLabel *>("rrToolsStatus")->text(); }
private slots:
    void init() {
        QVERIFY(temporary.isValid());
        host.resize(800, 1136);
        host.show();
        tools = reader::openTools(&host, {}, temporary.path());
        QVERIFY(tools->isVisible());
    }
    void cleanup() {
        delete tools;
        tools = nullptr;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void shellRunsAndCancelsWithoutBlocking() {
        auto *command = tools->findChild<QLineEdit *>("rrTerminalCommand");
        auto *output = tools->findChild<QPlainTextEdit *>("rrTerminalOutput");
        auto *process = tools->findChild<QProcess *>();
        command->setText("printf roreader-shell; sleep 0.1");
        bool eventDelivered = false;
        QTimer::singleShot(0, tools, [&] { eventDelivered = true; });
        click("rrTerminalRun");
        QTRY_VERIFY(eventDelivered);
        QTRY_VERIFY(output->toPlainText().contains("roreader-shell"));
        QTRY_VERIFY(output->toPlainText().contains("Exit: 0"));
        command->setText("sleep 30");
        click("rrTerminalRun");
        QTRY_COMPARE(process->state(), QProcess::Running);
        click("rrTerminalStop");
        QTRY_COMPARE(process->state(), QProcess::NotRunning);
    }
    void editorRoundTripAndConflict() {
        click("rrToolsEditor");
        auto *text = tools->findChild<QPlainTextEdit *>("rrTextDocument");
        auto *name = tools->findChild<QLineEdit *>("rrTextFilename");
        name->setText("roundtrip.txt");
        text->setPlainText(QString::fromUtf8("ROREADER\nGrüße 🌍"));
        click("rrTextSave");
        QTRY_VERIFY(status().startsWith("Gespeichert:"));
        QFile saved(temporary.path() + "/roundtrip.txt");
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), text->toPlainText().toUtf8());
        saved.close();
        click("rrTextNew");
        click("rrTextOpen");
        QTRY_VERIFY(!text->toPlainText().isEmpty());
        QVERIFY(!text->document()->isModified());
        QVERIFY(saved.open(QIODevice::WriteOnly));
        saved.write("external update"); saved.close();
        text->insertPlainText("more");
        click("rrTextSave");
        QTRY_VERIFY(status().contains("geändert"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QByteArray("external update"));
        name->setText("../escape.txt");
        click("rrTextSave");
        QVERIFY(status().contains("ohne Ordner"));
        QVERIFY(!QFile::exists(temporary.path() + "/../escape.txt"));
    }
    void editorDoesNotOverwriteUnopenedFile() {
        QFile existing(temporary.path() + "/existing.txt");
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("keep me"); existing.close();
        click("rrToolsEditor");
        tools->findChild<QLineEdit *>("rrTextFilename")->setText("existing.txt");
        tools->findChild<QPlainTextEdit *>("rrTextDocument")->setPlainText("replacement");
        click("rrTextSave");
        QTRY_VERIFY(status().contains("existiert bereits"));
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("keep me"));
    }
    void sketchDrawUndoAndSave() {
        click("rrToolsSketch");
        auto *canvas = tools->findChild<QWidget *>("rrSketchCanvas");
        QVERIFY(canvas && canvas->isVisible());
        const auto blank = canvas->grab().toImage();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(60, 80));
        QTest::mouseMove(canvas, QPoint(180, 160));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(180, 160));
        QVERIFY(canvas->grab().toImage() != blank);
        click("rrSketchUndo");
        QCOMPARE(canvas->grab().toImage(), blank);
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(90, 90));
        click("rrSketchSave");
        QTRY_VERIFY(status().startsWith("Gespeichert:"));
        const auto files = QDir(temporary.path()).entryList({"sketch-*.png"}, QDir::Files);
        QVERIFY(!files.isEmpty());
        QImage image(temporary.path() + '/' + files.last());
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(800, 1136));
    }
    void keyboardAndDraftClose() {
        click("rrToolsEditor");
        click("rrTextKeyboard");
        QTRY_VERIFY(tools->findChild<QWidget *>("widgetKeyboard"));
        QVERIFY(tools->findChild<QWidget *>("widgetKeyboard")->isVisible());
        tools->findChild<QLineEdit *>("rrToolsInput")->setText("touch input");
        click("rrToolsInputApply");
        QTRY_COMPARE(tools->findChild<QLineEdit *>("rrTextInsert")->text(), QStringLiteral("touch input"));
        click("rrTextAdd");
        QCOMPARE(tools->findChild<QPlainTextEdit *>("rrTextDocument")->toPlainText(), QStringLiteral("touch input"));
        tools->findChild<QPlainTextEdit *>("rrTextDocument")->insertPlainText("draft");
        click("rrToolsClose");
        QTRY_VERIFY(tools->findChild<QPushButton *>("rrToolsKeep"));
        click("rrToolsKeep");
        QTRY_VERIFY(tools->isVisible());
    }
    void captures() {
        const auto destination = qEnvironmentVariable("ROREADER_CAPTURE_DIR");
        for (const auto &scene : {"terminal", "sketchpad", "editor"}) {
            click(qstrcmp(scene, "terminal") == 0 ? "rrToolsTerminal" : qstrcmp(scene, "sketchpad") == 0 ? "rrToolsSketch" : "rrToolsEditor");
            const auto image = tools->grab().toImage();
            QCOMPARE(image.size(), host.size());
            QByteArray bytes;
            QBuffer output(&bytes);
            QVERIFY(output.open(QIODevice::WriteOnly));
            QVERIFY(image.save(&output, "PNG"));
            QVERIFY(bytes.size() > 1000);
            if (!destination.isEmpty()) {
                QVERIFY(QDir().mkpath(destination));
                QVERIFY(image.save(destination + '/' + scene + ".png"));
            }
        }
    }
};
int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "minimal:enable_fonts:freetype");
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    ui::setDensity(300);
    ToolsTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "roreader_tools.moc"
