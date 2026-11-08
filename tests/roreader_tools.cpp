#include "interfaces/reader/tools/tools_panel.h"
#include "interfaces/reader/tools/sketch_canvas.h"
#include "interfaces/reader/tools/game_models.h"
#include "interfaces/reader/ui/style/scale.h"
#include <QApplication>
#include "support/strings.h"
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
#include <QComboBox>
#include <QTreeWidget>
#include <QJsonDocument>

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
    QString status() {
        return tools->findChild<QLabel *>("rrToolsStatus")->text();
    }
  private slots:
    void init() {
        i18n::setLanguage("de");
        QVERIFY(temporary.isValid());
        host.resize(800, 1136);
        host.show();
        tools = reader::openTools(&host, {}, temporary.path());
        QVERIFY(tools->isVisible());
    }
    void cleanup() {
        i18n::setLanguage("en");
        delete tools;
        tools = nullptr;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void localizedTools_data() {
        QTest::addColumn<QString>("code");
        const auto languages = i18n::languages();
        QVERIFY(!languages.isEmpty());
        for (auto it = languages.begin(); it != languages.end(); ++it)
            QTest::newRow(qPrintable(it.key())) << it.key();
    }
    void localizedTools() {
        QFETCH(QString, code);
        delete tools;
        i18n::setLanguage(code);
        tools = reader::openTools(&host, {}, temporary.path());
        const auto catalog = i18n::resource(code);
        QVERIFY(!catalog.isEmpty());
        QCOMPARE(tools->findChild<QPushButton *>("rrToolsEditor")->text(),
                 catalog.value("tools").toObject().value("Texteditor").toString());
        QCOMPARE(strings::get("reader.exit"),catalog.value("reader").toObject().value("reader.exit").toString());
        const auto formatted = i18n::text("%1 Dateien · %2").arg(7).arg("ROREADER");
        QVERIFY(formatted.contains("7") && formatted.contains("ROREADER"));
        const auto destination = qEnvironmentVariable("ROREADER_CAPTURE_DIR");
        if (!destination.isEmpty() && QStringList{"ku","uk","fr"}.contains(code)) {
            QVERIFY(QDir().mkpath(destination));
            QVERIFY(tools->grab().save(destination + "/tools-" + code + ".png"));
        }
        i18n::setLanguage("not-a-language");
        QCOMPARE(i18n::language(),QStringLiteral("en"));
        QCOMPARE(i18n::text("Unknown future text"),QStringLiteral("Unknown future text"));
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
        saved.write("external update");
        saved.close();
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
        existing.write("keep me");
        existing.close();
        click("rrToolsEditor");
        tools->findChild<QLineEdit *>("rrTextFilename")->setText("existing.txt");
        tools->findChild<QPlainTextEdit *>("rrTextDocument")->setPlainText("replacement");
        click("rrTextSave");
        QTRY_VERIFY(status().contains("existiert bereits"));
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("keep me"));
    }
    void editorFindAndPreservesUtf8Bom() {
        QFile input(temporary.path() + "/bom.txt");
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write(QByteArray("\xef\xbb\xbf") + "first needle second needle");
        input.close();
        click("rrToolsEditor");
        tools->findChild<QLineEdit *>("rrTextFilename")->setText("bom.txt");
        click("rrTextOpen");
        auto *document = tools->findChild<QPlainTextEdit *>("rrTextDocument");
        QTRY_COMPARE(document->toPlainText(), QStringLiteral("first needle second needle"));
        tools->findChild<QLineEdit *>("rrTextFind")->setText("needle");
        click("rrTextFindNext");
        QCOMPARE(document->textCursor().selectedText(), QStringLiteral("needle"));
        const int first = document->textCursor().position();
        click("rrTextFindNext");
        QVERIFY(document->textCursor().position() > first);
        document->insertPlainText("changed");
        click("rrTextSave");
        QTRY_VERIFY(status().startsWith("Gespeichert:"));
        QVERIFY(input.open(QIODevice::ReadOnly));
        const auto saved = input.readAll();
        QVERIFY(saved.startsWith("\xef\xbb\xbf"));
        QVERIFY(saved.contains("changed"));
        QVERIFY(!document->document()->isModified());
    }
    void sketchDrawUndoAndSave() {
        click("rrToolsSketch");
        auto *canvas = tools->findChild<QWidget *>("rrSketchCanvas");
        QVERIFY(canvas && canvas->isVisible());
        const auto blank = canvas->grab().toImage();
        const auto center = canvas->rect().center();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, center - QPoint(30, 30));
        QTest::mouseMove(canvas, center + QPoint(30, 30));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, center + QPoint(30, 30));
        QVERIFY(canvas->grab().toImage() != blank);
        click("rrSketchUndo");
        QCOMPARE(canvas->grab().toImage(), blank);
        click("rrSketchRedo");
        QVERIFY(canvas->grab().toImage() != blank);
        click("rrSketchUndo");
        QCOMPARE(canvas->grab().toImage(), blank);
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, center);
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
    void terminalHistoryDirectoryAndInput() {
        QVERIFY(QDir().mkpath(temporary.path() + "/shell-sub"));
        auto *command = tools->findChild<QLineEdit *>("rrTerminalCommand");
        auto *output = tools->findChild<QPlainTextEdit *>("rrTerminalOutput");
        auto *process = tools->findChild<QProcess *>();
        command->setText("cd shell-sub; printf first");
        click("rrTerminalRun");
        QTRY_VERIFY(output->toPlainText().contains("Exit: 0"));
        QCOMPARE(tools->findChild<QLineEdit *>("rrTerminalDirectory")->text(), temporary.path() + "/shell-sub");
        command->setText("pwd");
        click("rrTerminalRun");
        QTRY_VERIFY(output->toPlainText().contains(temporary.path() + "/shell-sub"));
        QTRY_COMPARE(process->state(), QProcess::NotRunning);
        QVERIFY(!output->toPlainText().contains("ROREADER_"));
        QTest::keyClick(command, Qt::Key_Up);
        QCOMPARE(command->text(), QStringLiteral("pwd"));
        QVERIFY(tools->findChild<QComboBox *>("rrTerminalHistory")->count() >= 3);
        command->setText("read answer; printf 'reply:%s' \"$answer\"");
        click("rrTerminalRun");
        QTRY_COMPARE(process->state(), QProcess::Running);
        command->setText("hello");
        click("rrTerminalRun");
        QTRY_VERIFY(output->toPlainText().contains("reply:hello"));
        QTRY_COMPARE(process->state(), QProcess::NotRunning);
        command->setText(QString::fromLatin1(R"(printf '\342'; sleep 0.05; printf '\202\254')"));
        click("rrTerminalRun");
        QTRY_COMPARE(process->state(), QProcess::NotRunning);
        QVERIFY(output->toPlainText().contains(QString::fromUtf8("€")));
        click("rrTerminalExport");
        QTRY_VERIFY(status().startsWith("Gespeichert:"));
        QVERIFY(!QDir(temporary.path()).entryList({"terminal-*.txt"}, QDir::Files).isEmpty());
    }
    void fileManagerRenameDeleteAndBackup() {
        QFile sample(temporary.path() + "/manage.txt");
        QVERIFY(sample.open(QIODevice::WriteOnly));
        sample.write("preserve me");
        sample.close();
        click("rrToolsFiles");
        auto *list = tools->findChild<QTreeWidget *>("rrFileList");
        QTRY_VERIFY(!list->findItems("manage.txt", Qt::MatchExactly, 0).isEmpty());
        list->setCurrentItem(list->findItems("manage.txt", Qt::MatchExactly, 0).first());
        tools->findChild<QLineEdit *>("rrFileRenameInput")->setText("renamed.txt");
        click("rrFilesRename");
        QTRY_VERIFY(!list->findItems("renamed.txt", Qt::MatchExactly, 0).isEmpty());
        QVERIFY(!QFile::exists(temporary.path() + "/manage.txt"));
        list->setCurrentItem(list->findItems("renamed.txt", Qt::MatchExactly, 0).first());
        click("rrFilesHash");
        auto *report = tools->findChild<QPlainTextEdit *>("rrDeviceReport");
        QTRY_VERIFY(report->toPlainText().contains("SHA-256:"));
        click("rrToolsMods");
        QTRY_VERIFY(!tools->property("utilityBusy").toBool());
        click("rrDocumentsBackup");
        QTRY_VERIFY(status().startsWith("Sicherung abgeschlossen:"));
        const auto backups = QDir(temporary.path()).entryList({"backup-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
        QVERIFY(!backups.isEmpty());
        QFile copy(temporary.path() + '/' + backups.last() + "/renamed.txt");
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), QByteArray("preserve me"));
        QVERIFY(QFile::exists(temporary.path() + '/' + backups.last() + "/backup-manifest.json"));
        click("rrToolsFiles");
        QTRY_VERIFY(!tools->property("utilityBusy").toBool());
        list->setCurrentItem(list->findItems("renamed.txt", Qt::MatchExactly, 0).first());
        click("rrFilesDelete");
        click("rrToolsCancel");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(QFile::exists(temporary.path() + "/renamed.txt"));
        click("rrFilesDelete");
        click("rrToolsConfirm");
        QTRY_VERIFY(!QFile::exists(temporary.path() + "/renamed.txt"));
        QTRY_VERIFY(!tools->property("utilityBusy").toBool());
    }
    void gamesHaveLegalMovesAndSolvableSudokus() {
        using namespace reader::games;
        TicTacToe game;
        QVERIFY(game.play(0));
        QVERIFY(!game.play(0));
        QVERIFY(game.play(3));
        QVERIFY(game.play(1));
        QVERIFY(game.play(4));
        QVERIFY(game.play(2));
        QCOMPARE(game.winner(), 1);
        QVERIFY(!game.play(8));
        game = {};
        game.cells = {2, 2, 0, 1, 1, 0, 0, 0, 0};
        game.turn = 2;
        QCOMPARE(game.suggested(), 2);
        QVERIFY(game.play(2));
        QCOMPARE(game.winner(), 2);
        for (unsigned variant = 0; variant < 18; ++variant) {
            Sudoku puzzle(variant);
            for (int cell = 0; cell < 81; ++cell) {
                QVERIFY(!puzzle.conflict(cell));
                if (puzzle.givens[cell])
                    QVERIFY(!puzzle.set(cell, 0));
            }
            int steps = 0;
            while (!puzzle.solved() && steps++ < 81)
                QVERIFY(puzzle.hint() >= 0);
            QVERIFY(puzzle.solved());
            for (int cell = 0; cell < 81; ++cell)
                QVERIFY(!puzzle.conflict(cell));
        }
        click("rrToolsGames");
        click("rrTicCell0");
        QCOMPARE(tools->findChild<QPushButton *>("rrTicCell0")->text(), QStringLiteral("X"));
        click("rrGameSudoku");
        click("rrSudokuCell2");
        click("rrSudokuDigit5");
        QVERIFY(tools->findChild<QLabel *>("rrSudokuStatus")->text().contains("Konflikt"));
        click("rrSudokuHint");
        QCOMPARE(tools->findChild<QPushButton *>("rrSudokuCell2")->text(), QStringLiteral("4"));
        click("rrGameMemory");
        click("rrMemoryCell0");
        auto *first = tools->findChild<QPushButton *>("rrMemoryCell0");
        QVERIFY(first->text() != "?");
        QVERIFY(!first->isEnabled());
        click("rrMemoryCell1");
        auto *second = tools->findChild<QPushButton *>("rrMemoryCell1");
        if (first->text() != second->text()) {
            QVERIFY(!tools->findChild<QPushButton *>("rrMemoryCell2")->isEnabled());
            QTRY_COMPARE(first->text(), QStringLiteral("?"));
        }
        click("rrMemoryNew");
        for (int cell = 0; cell < 16; ++cell)
            QCOMPARE(tools->findChild<QPushButton *>(QString("rrMemoryCell%1").arg(cell))->text(), QStringLiteral("?"));
    }
    void captures() {
        const auto destination = qEnvironmentVariable("ROREADER_CAPTURE_DIR");
        for (const auto &scene : {"terminal", "sketchpad", "editor"}) {
            click(qstrcmp(scene, "terminal") == 0    ? "rrToolsTerminal"
                  : qstrcmp(scene, "sketchpad") == 0 ? "rrToolsSketch"
                                                     : "rrToolsEditor");
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
