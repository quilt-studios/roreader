#include "tools_panel.h"
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/keyboard.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "interfaces/reader/ui/style/styles.h"
#include "support/main_thread.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <QTouchEvent>
#include <QVBoxLayout>
#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

namespace kobalt::reader {
namespace {
namespace m = ui::metrics;
constexpr qint64 textLimit = 1024 * 1024;

// Never wait for storage on the thread that turns pages. The thread owns its
// result until its queued finished callback has consumed it.
class FileJob final : public QThread {
public:
    std::function<void(FileJob &)> operation;
    QByteArray bytes;
    QString error;
    void run() override {
        mainthread::blocking("ROREADER utility file operation");
        operation(*this);
    }
};

QString toolsDirectory() {
    if (QFileInfo::exists(QStringLiteral("/mnt/onboard/.kobo")))
        return QStringLiteral("/mnt/onboard/ROREADER");
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/ROREADER");
}

class Sketch final : public QWidget {
public:
    QImage image{800, 1136, QImage::Format_RGB32};
    QVector<QImage> undo;
    bool dirty = false, drawing = false, eraser = false;
    QPoint previous;
    explicit Sketch(QWidget *parent) : QWidget(parent) {
        setObjectName("rrSketchCanvas");
        setAttribute(Qt::WA_AcceptTouchEvents);
        setMinimumSize(160, 160);
        image.fill(Qt::white);
    }
    QPoint paper(QPoint position) const {
        return {qBound(0, position.x() * image.width() / qMax(1, width()), image.width() - 1),
                qBound(0, position.y() * image.height() / qMax(1, height()), image.height() - 1)};
    }
    void begin(QPoint position) {
        if (undo.size() == 4) undo.removeFirst();
        undo.push_back(image);
        drawing = true;
        previous = paper(position);
        stroke(position);
    }
    void stroke(QPoint position) {
        if (!drawing) return;
        const auto next = paper(position);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(eraser ? Qt::white : Qt::black, eraser ? 32 : 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(previous, next);
        painter.drawPoint(next);
        const auto dirtyArea = QRect(previous, next).normalized();
        previous = next;
        dirty = true;
        const int radius = eraser ? 20 : 5;
        const QRect repaint(QPoint(dirtyArea.left() * width() / image.width(), dirtyArea.top() * height() / image.height()),
                            QPoint(dirtyArea.right() * width() / image.width(), dirtyArea.bottom() * height() / image.height()));
        update(repaint.adjusted(-radius, -radius, radius, radius));
    }
    void paintEvent(QPaintEvent *) override { QPainter(this).drawImage(rect(), image); }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) begin(event->pos());
    }
    void mouseMoveEvent(QMouseEvent *event) override { stroke(event->pos()); }
    void mouseReleaseEvent(QMouseEvent *) override { drawing = false; }
    bool event(QEvent *event) override {
        if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate || event->type() == QEvent::TouchEnd) {
            auto *touch = static_cast<QTouchEvent *>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const auto points = touch->points();
            if (!points.isEmpty()) {
                const auto position = points.first().position().toPoint();
#else
            const auto points = touch->touchPoints();
            if (!points.isEmpty()) {
                const auto position = points.first().pos().toPoint();
#endif
                if (event->type() == QEvent::TouchBegin) begin(position);
                else stroke(position);
            }
            if (event->type() == QEvent::TouchEnd) drawing = false;
            event->accept();
            return true;
        }
        if (event->type() == QEvent::TouchCancel) drawing = false;
        return QWidget::event(event);
    }
};

class Tools final : public QWidget {
    KeyboardFactory makeKeyboard;
    std::unique_ptr<KeyboardController> keyboard;
    QStackedWidget *pages;
    QLabel *status;
    QPlainTextEdit *terminal, *editor;
    QLineEdit *command, *filename, *insert;
    Sketch *sketch;
    QProcess *shell;
    QTimer *deadline;
    QPointer<ui::ChoiceModal> confirmation;
    QPointer<ui::ChoiceModal> inputDialog;
    QByteArray loadedHash;
    QString loadedName;
    bool pending = false;
    qint64 shellGroup = 0, outputBytes = 0;
    QString directory;

    QPushButton *button(QWidget *parent, QBoxLayout *layout, const char *id, const QString &text, std::function<void()> action) {
        auto *result = ui::makeButton(parent, layout, id, text, std::move(action));
        result->setMinimumHeight(m::chrome::actionHeight);
        return result;
    }
    void showKeyboard(QLineEdit *input) {
        if (inputDialog) return;
        keyboard.reset();
        auto *dialog = new ui::ChoiceModal(this, {{}, QStringLiteral("Text eingeben"), {}},
                                          {QStringLiteral("Abbrechen"), "rrToolsInputCancel"},
                                          {QStringLiteral("Übernehmen"), "rrToolsInputApply"});
        inputDialog = dialog;
        dialog->setObjectName("rrToolsInputDialog");
        dialog->setGeometry(rect());
        dialog->setDimmed(true);
        auto *copy = new QLineEdit(input->text(), dialog->card());
        copy->setObjectName("rrToolsInput");
        copy->setMaxLength(input->maxLength());
        copy->setMinimumHeight(m::chrome::actionHeight);
        copy->setInputMethodHints(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
        ui::readerStyle(copy, QStringLiteral("QLineEdit{background:palette(window);color:palette(window-text);border:1px solid palette(mid);") + ui::styles::font(m::chrome::labelText) + '}');
        dialog->card()->rows()->insertWidget(1, copy);
        dialog->setFocusProxy(copy);
        const auto dismiss = [this, dialog] {
            dialog->hide();
            if (keyboard) keyboard->hide();
            QTimer::singleShot(0, this, [this, dialog] {
                if (inputDialog == dialog) { keyboard.reset(); inputDialog.clear(); }
                dialog->deleteLater();
            });
        };
        const QPointer<QLineEdit> target(input);
        const auto submit = [dialog, target, copy, dismiss] {
            if (!dialog->isVisible()) return;
            if (target) target->setText(copy->text());
            dismiss();
        };
        connect(dialog->primaryButton(), &QPushButton::clicked, this, submit);
        connect(dialog->secondaryButton(), &QPushButton::clicked, this, dismiss);
        connect(copy, &QLineEdit::returnPressed, this, submit);
        dialog->dismissed = dismiss;
        keyboard = makeKeyboard ? makeKeyboard(dialog, copy) : ui::makeWidgetKeyboard(dialog, copy);
        if (!keyboard) { dismiss(); return; }
        keyboard->setSubmitText(QStringLiteral("OK"));
        keyboard->submitted = submit;
        dialog->setCardArea(QRect(0, 0, width(), height() * 2 / 3));
        dialog->show(); dialog->raise(); dialog->fitCard();
        copy->setFocus();
        keyboard->show();
        keyboard->place();
    }
    void job(std::function<void(FileJob &)> operation, std::function<void(const FileJob &)> done) {
        if (pending) return;
        pending = true;
        setProperty("utilityBusy", true);
        status->setText(QStringLiteral("Datei wird verarbeitet …"));
        auto *task = new FileJob;
        task->operation = std::move(operation);
        connect(task, &QThread::finished, this, [this, task, done] {
            pending = false;
            setProperty("utilityBusy", false);
            status->setText(task->error.isEmpty() ? directory : task->error);
            done(*task);
        });
        connect(task, &QThread::finished, task, &QObject::deleteLater);
        task->start();
    }
    QString name() {
        const auto value = filename->text().trimmed();
        if (value.isEmpty() || value.size() > 128 || value == "." || value == ".." ||
            value.contains('/') || value.contains('\\') || value.contains(QChar(0))) {
            status->setText(QStringLiteral("Bitte einen einfachen Dateinamen ohne Ordner angeben."));
            return {};
        }
        return value;
    }
    void loadText() {
        if (pending) return;
        if (editor->document()->isModified()) {
            status->setText(QStringLiteral("Bitte zuerst speichern oder den Text leeren."));
            return;
        }
        const auto file = name();
        if (file.isEmpty()) return;
        const auto root = directory;
        const int revision = editor->document()->revision();
        job([root, file](FileJob &task) {
            QFile input(root + '/' + file);
            if (QFileInfo(input).isSymLink() || !input.open(QIODevice::ReadOnly)) {
                task.error = QStringLiteral("Datei kann nicht geöffnet werden."); return;
            }
            if (input.size() > textLimit) { task.error = QStringLiteral("Maximale Textgröße: 1 MiB."); return; }
            task.bytes = input.readAll();
            if (input.error() != QFile::NoError || task.bytes.contains('\0') || QString::fromUtf8(task.bytes).toUtf8() != task.bytes)
                task.error = QStringLiteral("Nur gültige UTF-8-Textdateien werden unterstützt.");
        }, [this, file, revision](const FileJob &task) {
            if (!task.error.isEmpty()) return;
            if (editor->document()->revision() != revision) {
                status->setText(QStringLiteral("Der Text wurde inzwischen bearbeitet. Öffnen wurde abgebrochen."));
                return;
            }
            editor->setPlainText(QString::fromUtf8(task.bytes));
            editor->document()->setModified(false);
            loadedName = file;
            loadedHash = QCryptographicHash::hash(task.bytes, QCryptographicHash::Sha256);
        });
    }
    void saveText() {
        const auto file = name();
        const auto bytes = editor->toPlainText().toUtf8();
        if (file.isEmpty()) return;
        if (bytes.size() > textLimit) { status->setText(QStringLiteral("Maximale Textgröße: 1 MiB.")); return; }
        const auto root = directory;
        const auto expected = file == loadedName ? loadedHash : QByteArray();
        job([root, file, bytes, expected](FileJob &task) {
            if (QFileInfo(root).isSymLink() || !QDir().mkpath(root)) { task.error = QStringLiteral("Speicherordner nicht verfügbar."); return; }
            const auto path = root + '/' + file;
            if (QFileInfo(path).isSymLink()) { task.error = QStringLiteral("Symbolische Links werden nicht überschrieben."); return; }
            if (QFileInfo::exists(path)) {
                QFile old(path);
                if (expected.isEmpty() || !old.open(QIODevice::ReadOnly) || old.size() > textLimit ||
                    QCryptographicHash::hash(old.readAll(), QCryptographicHash::Sha256) != expected) {
                    task.error = QStringLiteral("Datei existiert bereits oder wurde geändert. Bitte einen neuen Namen wählen."); return;
                }
            }
            QSaveFile output(path);
            if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
                task.error = QStringLiteral("Speichern fehlgeschlagen. Originaldatei bleibt erhalten."); return;
            }
            task.bytes = bytes;
        }, [this, file, bytes](const FileJob &task) {
            if (!task.error.isEmpty()) return;
            loadedName = file;
            loadedHash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
            if (editor->toPlainText().toUtf8() == bytes) editor->document()->setModified(false);
            status->setText(QStringLiteral("Gespeichert: ") + directory + '/' + file);
        });
    }
    void saveSketch() {
        const auto image = sketch->image;
        const auto root = directory;
        const auto file = QStringLiteral("sketch-") + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + ".png";
        job([root, file, image](FileJob &task) {
            if (QFileInfo(root).isSymLink() || !QDir().mkpath(root)) { task.error = QStringLiteral("Speicherordner nicht verfügbar."); return; }
            const auto path = root + '/' + file;
            if (QFileInfo::exists(path)) { task.error = QStringLiteral("Datei existiert bereits. Bitte erneut speichern."); return; }
            QSaveFile output(path);
            if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") || !output.commit())
                task.error = QStringLiteral("Zeichnung konnte nicht gespeichert werden.");
        }, [this, image, file](const FileJob &task) {
            if (task.error.isEmpty()) {
                if (sketch->image == image) sketch->dirty = false;
                status->setText(QStringLiteral("Gespeichert: ") + directory + '/' + file);
            }
        });
    }
    void stopShell() {
#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        if (shellGroup > 0) ::kill(-shellGroup, SIGKILL);
        shellGroup = 0;
#endif
        if (shell->state() != QProcess::NotRunning) shell->kill();
    }
    void runCommand() {
        if (shell->state() != QProcess::NotRunning) return;
        const auto text = command->text();
        if (text.trimmed().isEmpty()) return;
        terminal->appendPlainText("$ " + text);
        outputBytes = 0;
        command->clear();
        shell->setWorkingDirectory(QFileInfo::exists(directory) ? directory : QDir::homePath());
        shell->start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), text});
        deadline->start(60000);
    }
    void discard(std::function<void()> action) {
        if (confirmation) return;
        auto *dialog = new ui::ChoiceModal(this, {{}, QStringLiteral("Ungespeicherte Änderungen verwerfen?"), {}},
                                          {QStringLiteral("Behalten"), "rrToolsKeep"},
                                          {QStringLiteral("Verwerfen"), "rrToolsDiscard"});
        confirmation = dialog;
        connect(dialog->findChild<QPushButton *>("rrToolsKeep"), &QPushButton::clicked, dialog, &QObject::deleteLater);
        connect(dialog->findChild<QPushButton *>("rrToolsDiscard"), &QPushButton::clicked, this, [dialog, action] { dialog->deleteLater(); action(); });
        dialog->setGeometry(rect());
        dialog->show(); dialog->raise(); dialog->fitCard();
    }
    void closeTools() {
        if (pending) { status->setText(QStringLiteral("Bitte das Speichern abwarten.")); return; }
        if (keyboard) keyboard->hide();
        auto close = [this] { stopShell(); hide(); deleteLater(); };
        if (sketch->dirty || editor->document()->isModified()) discard(close);
        else close();
    }
public:
    void dismiss() { closeTools(); }
    void quiesce() {
        stopShell();
        if (keyboard) keyboard->hide();
        if (inputDialog) { inputDialog->hide(); inputDialog->deleteLater(); inputDialog.clear(); }
    }
    explicit Tools(QWidget *host, KeyboardFactory factory, const QString &path) : QWidget(host), makeKeyboard(std::move(factory)), directory(path.isEmpty() ? toolsDirectory() : path) {
        setObjectName("rrTools");
        setAutoFillBackground(true);
        setGeometry(host->rect());
        host->installEventFilter(this);
        auto *rows = new QVBoxLayout(this);
        rows->setContentsMargins(m::chrome::inset, m::chrome::inset, m::chrome::inset, m::chrome::inset);
        rows->setSpacing(m::spacing::row);
        auto *header = new QHBoxLayout;
        rows->addLayout(header);
        auto *title = new ui::InterfaceLabel(QStringLiteral("ROREADER · Werkzeuge"), this);
        ui::readerStyle(title, ui::styles::font(m::text::body));
        header->addWidget(title, 1);
        button(this, header, "rrToolsClose", QStringLiteral("Zurück"), [this] { closeTools(); });
        auto *tabs = new QHBoxLayout;
        rows->addLayout(tabs);
        pages = new QStackedWidget(this);
        int index = 0;
        for (const auto &text : {QStringLiteral("Terminal"), QStringLiteral("Sketchpad"), QStringLiteral("Texteditor")}) {
            const int page = index++;
            button(this, tabs, page == 0 ? "rrToolsTerminal" : page == 1 ? "rrToolsSketch" : "rrToolsEditor", text, [this, page] {
                if (keyboard) keyboard->hide();
                pages->setCurrentIndex(page);
            });
        }
        rows->addWidget(pages, 1);
        auto *terminalPage = new QWidget(pages);
        pages->addWidget(terminalPage);
        auto *terminalRows = new QVBoxLayout(terminalPage);
        terminal = new QPlainTextEdit(terminalPage);
        terminal->setObjectName("rrTerminalOutput");
        terminal->setReadOnly(true);
        terminal->setMaximumBlockCount(1000);
        terminal->setPlainText(QStringLiteral("Shell-Kommandos · jeweils bis 60 Sekunden\nInteraktive Vollbildprogramme werden nicht unterstützt.\nBefehle laufen mit den Rechten des Readers."));
        terminalRows->addWidget(terminal, 1);
        command = new QLineEdit(terminalPage);
        command->setObjectName("rrTerminalCommand");
        command->setMaxLength(4096);
        command->setPlaceholderText(QStringLiteral("Shell-Befehl"));
        command->setMinimumHeight(m::chrome::actionHeight);
        terminalRows->addWidget(command);
        auto *terminalActions = new QHBoxLayout;
        terminalRows->addLayout(terminalActions);
        button(terminalPage, terminalActions, "rrTerminalKeyboard", QStringLiteral("Tastatur"), [this] { showKeyboard(command); });
        button(terminalPage, terminalActions, "rrTerminalRun", QStringLiteral("Ausführen"), [this] { runCommand(); });
        button(terminalPage, terminalActions, "rrTerminalStop", QStringLiteral("Stoppen"), [this] { stopShell(); });
        shell = new QProcess(this);
        connect(shell, &QProcess::started, this, [this] { shellGroup = shell->processId(); });
        shell->setProcessChannelMode(QProcess::MergedChannels);
#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        shell->setChildProcessModifier([] { ::setsid(); });
#endif
        connect(shell, &QProcess::readyReadStandardOutput, this, [this] {
            const auto output = shell->readAllStandardOutput();
            outputBytes += output.size();
            auto cursor = terminal->textCursor();
            cursor.movePosition(QTextCursor::End);
            cursor.insertText(QString::fromUtf8(output.left(65536)));
            terminal->setTextCursor(cursor);
            terminal->ensureCursorVisible();
            if (output.size() > 65536 || outputBytes > textLimit) { terminal->appendPlainText(QStringLiteral("Ausgabelimit erreicht.")); stopShell(); }
        });
        deadline = new QTimer(this);
        deadline->setSingleShot(true);
        connect(deadline, &QTimer::timeout, this, [this] { terminal->appendPlainText(QStringLiteral("Zeitlimit erreicht.")); stopShell(); });
        connect(shell, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus) {
            stopShell(); deadline->stop(); terminal->appendPlainText(QStringLiteral("\nExit: %1").arg(code));
        });
        connect(shell, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            status->setText(shell->errorString());
        });
        connect(command, &QLineEdit::returnPressed, this, [this] { runCommand(); });
        auto *sketchPage = new QWidget(pages);
        pages->addWidget(sketchPage);
        auto *sketchRows = new QVBoxLayout(sketchPage);
        auto *sketchActions = new QHBoxLayout;
        sketchRows->addLayout(sketchActions);
        sketch = new Sketch(sketchPage);
        sketchRows->addWidget(sketch, 1);
        auto *eraser = button(sketchPage, sketchActions, "rrSketchEraser", QStringLiteral("Radierer"), [] {});
        eraser->setCheckable(true);
        connect(eraser, &QPushButton::toggled, this, [this](bool active) { sketch->eraser = active; });
        button(sketchPage, sketchActions, "rrSketchUndo", QStringLiteral("Rückgängig"), [this] {
            if (sketch->undo.isEmpty()) return;
            sketch->image = sketch->undo.takeLast(); sketch->dirty = true; sketch->update();
        });
        button(sketchPage, sketchActions, "rrSketchClear", QStringLiteral("Neu"), [this] {
            auto clear = [this] { sketch->image.fill(Qt::white); sketch->undo.clear(); sketch->dirty = false; sketch->update(); };
            if (sketch->dirty) discard(clear); else clear();
        });
        button(sketchPage, sketchActions, "rrSketchSave", QStringLiteral("PNG speichern"), [this] { saveSketch(); });
        auto *editorPage = new QWidget(pages);
        pages->addWidget(editorPage);
        auto *editorRows = new QVBoxLayout(editorPage);
        filename = new QLineEdit(QStringLiteral("notes.txt"), editorPage);
        filename->setObjectName("rrTextFilename");
        filename->setMaxLength(128);
        filename->setMinimumHeight(m::chrome::actionHeight);
        editorRows->addWidget(filename);
        auto *fileActions = new QHBoxLayout;
        editorRows->addLayout(fileActions);
        button(editorPage, fileActions, "rrTextFilenameKeyboard", QStringLiteral("Dateiname"), [this] { showKeyboard(filename); });
        button(editorPage, fileActions, "rrTextOpen", QStringLiteral("Öffnen"), [this] { loadText(); });
        button(editorPage, fileActions, "rrTextSave", QStringLiteral("Speichern"), [this] { saveText(); });
        button(editorPage, fileActions, "rrTextNew", QStringLiteral("Neu"), [this] {
            auto clear = [this] { editor->clear(); editor->document()->setModified(false); loadedHash.clear(); loadedName.clear(); };
            if (editor->document()->isModified()) discard(clear); else clear();
        });
        editor = new QPlainTextEdit(editorPage);
        editor->setObjectName("rrTextDocument");
        editorRows->addWidget(editor, 1);
        insert = new QLineEdit(editorPage);
        insert->setObjectName("rrTextInsert");
        insert->setPlaceholderText(QStringLiteral("Text an der Auswahl einfügen"));
        insert->setMaxLength(4096);
        insert->setMinimumHeight(m::chrome::actionHeight);
        editorRows->addWidget(insert);
        auto *textActions = new QHBoxLayout;
        editorRows->addLayout(textActions);
        button(editorPage, textActions, "rrTextKeyboard", QStringLiteral("Tastatur"), [this] { showKeyboard(insert); });
        button(editorPage, textActions, "rrTextAdd", QStringLiteral("Einfügen"), [this] { editor->insertPlainText(insert->text()); insert->clear(); });
        button(editorPage, textActions, "rrTextNewline", QStringLiteral("Neue Zeile"), [this] { editor->insertPlainText("\n"); });
        button(editorPage, textActions, "rrTextUndo", QStringLiteral("Rückgängig"), [this] { editor->undo(); });
        status = new ui::InterfaceLabel(directory, this);
        status->setObjectName("rrToolsStatus");
        status->setWordWrap(true);
        status->setTextFormat(Qt::PlainText);
        ui::readerStyle(status, ui::styles::font(m::text::secondary));
        rows->addWidget(status);
        for (auto *input : {command, filename, insert}) {
            ui::readerStyle(input, QStringLiteral("QLineEdit{background:palette(base);color:palette(text);border:1px solid palette(mid);padding:12px;") + ui::styles::font(m::chrome::labelText) + '}');
            input->setInputMethodHints(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
        }
        for (auto *text : {terminal, editor})
            ui::readerStyle(text, QStringLiteral("QPlainTextEdit{background:palette(base);color:palette(text);border:1px solid palette(mid);padding:8px;") + ui::styles::font(m::chrome::labelText) + '}');
    }
    ~Tools() override {
        keyboard.reset();
        stopShell();
        // QProcess destruction may wait. Reap asynchronously after hiding or
        // destruction, without blocking the reader's UI thread.
        if (shell->state() != QProcess::NotRunning) {
            shell->setParent(nullptr);
            connect(shell, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), shell, &QObject::deleteLater);
        }
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == parentWidget()) {
            if (event->type() == QEvent::Resize) {
                setGeometry(parentWidget()->rect());
                if (inputDialog) {
                    inputDialog->setGeometry(rect());
                    inputDialog->setCardArea(QRect(0, 0, width(), height() * 2 / 3));
                    inputDialog->fitCard();
                }
                if (keyboard) keyboard->place();
            }
            if (event->type() == QEvent::Hide) { stopShell(); if (keyboard) keyboard->hide(); }
        }
        return QWidget::eventFilter(object, event);
    }
};
}
QWidget *openTools(QWidget *host, KeyboardFactory keyboard, const QString &directory) {
    if (auto *existing = host->findChild<QWidget *>("rrTools", Qt::FindDirectChildrenOnly)) {
        existing->show(); existing->raise(); return existing;
    }
    auto *tools = new Tools(host, std::move(keyboard), directory);
    tools->show(); tools->raise();
    return tools;
}
bool toolsBusy(const QWidget *host) {
    const auto *widget = host->findChild<QWidget *>("rrTools", Qt::FindDirectChildrenOnly);
    return widget && widget->property("utilityBusy").toBool();
}
void stopTools(QWidget *host) {
    if (auto *widget = host->findChild<QWidget *>("rrTools", Qt::FindDirectChildrenOnly))
        static_cast<Tools *>(widget)->quiesce();
}
bool dismissTools(QWidget *host) {
    auto *widget = host->findChild<QWidget *>("rrTools", Qt::FindDirectChildrenOnly);
    if (!widget || !widget->isVisible()) return false;
    static_cast<Tools *>(widget)->dismiss();
    return true;
}
}
