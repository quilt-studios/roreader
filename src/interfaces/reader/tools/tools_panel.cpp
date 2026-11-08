#include "support/localization.h"
#include "tools_panel.h"
#include "sketch_canvas.h"
#include "games_panel.h"
#include <QButtonGroup>
#include <QComboBox>
#include <QGridLayout>
#include <QTreeWidget>
#include <QStorageInfo>
#include <QSysInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QUuid>
#include <QRegularExpression>
#include <QSlider>
#include <QKeyEvent>
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
    QString error, report;
    QJsonArray rows;
    QImage picture;
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

class Tools final : public QWidget {
    KeyboardFactory makeKeyboard;
    std::unique_ptr<KeyboardController> keyboard;
    QStackedWidget *pages;
    QLabel *status;
    QPlainTextEdit *terminal, *editor;
    QLineEdit *command, *filename, *insert;
    SketchCanvas *sketch;
    QTreeWidget *files;
    QPlainTextEdit *deviceReport;
    QLineEdit *renameInput, *workingDirectory, *findInput;
    QComboBox *history;
    QTimer terminalFrame;
    QByteArray terminalBuffer, utf8Tail, cwdMarker;
    QString shellDirectory;
    int historyIndex = -1;
    bool infoLoaded = false;
    QProcess *shell;
    QTimer *deadline;
    QPointer<ui::ChoiceModal> confirmation;
    QPointer<ui::ChoiceModal> inputDialog;
    QByteArray loadedHash;
    QString loadedName;
    bool loadedBom = false;
    bool pending = false;
    qint64 shellGroup = 0, outputBytes = 0;
    QString directory;

    QPushButton *button(QWidget *parent, QBoxLayout *layout, const char *id, const QString &text,
                        std::function<void()> action) {
        auto *result = ui::makeButton(parent, layout, id, text, std::move(action));
        result->setMinimumHeight(ui::px(80));
        result->setProperty("segment_first", true);
        result->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        ui::readerStyle(
            result,
            QString("QPushButton{border:1px solid "
                    "palette(mid);border-radius:%1px;padding:8px;background:palette(window);color:palette(window-text);"
                    "font-size:%2px;} "
                    "QPushButton:pressed,QPushButton:checked{background:palette(window-text);color:palette(window);}")
                .arg(ui::px(16))
                .arg(int(m::chrome::labelText)));
        return result;
    }
    void showKeyboard(QLineEdit *input) {
        if (inputDialog)
            return;
        keyboard.reset();
        auto *dialog = new ui::ChoiceModal(this, {{}, i18n::text(QStringLiteral("Text eingeben")), {}},
                                           {i18n::text(QStringLiteral("Abbrechen")), "rrToolsInputCancel"},
                                           {i18n::text(QStringLiteral("Übernehmen")), "rrToolsInputApply"});
        inputDialog = dialog;
        dialog->setObjectName("rrToolsInputDialog");
        dialog->setGeometry(rect());
        dialog->setDimmed(true);
        auto *copy = new QLineEdit(input->text(), dialog->card());
        copy->setObjectName("rrToolsInput");
        copy->setMaxLength(input->maxLength());
        copy->setMinimumHeight(m::chrome::actionHeight);
        copy->setInputMethodHints(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
        ui::readerStyle(
            copy,
            QStringLiteral(
                "QLineEdit{background:palette(window);color:palette(window-text);border:1px solid palette(mid);") +
                ui::styles::font(m::chrome::labelText) + '}');
        dialog->card()->rows()->insertWidget(1, copy);
        dialog->setFocusProxy(copy);
        const auto dismiss = [this, dialog] {
            dialog->hide();
            if (keyboard)
                keyboard->hide();
            QTimer::singleShot(0, this, [this, dialog] {
                if (inputDialog == dialog) {
                    keyboard.reset();
                    inputDialog.clear();
                }
                dialog->deleteLater();
            });
        };
        const QPointer<QLineEdit> target(input);
        const auto submit = [dialog, target, copy, dismiss] {
            if (!dialog->isVisible())
                return;
            if (target)
                target->setText(copy->text());
            dismiss();
        };
        connect(dialog->primaryButton(), &QPushButton::clicked, this, submit);
        connect(dialog->secondaryButton(), &QPushButton::clicked, this, dismiss);
        connect(copy, &QLineEdit::returnPressed, this, submit);
        dialog->dismissed = dismiss;
        keyboard = makeKeyboard ? makeKeyboard(dialog, copy) : ui::makeWidgetKeyboard(dialog, copy);
        if (!keyboard) {
            dismiss();
            return;
        }
        keyboard->setSubmitText(QStringLiteral("OK"));
        keyboard->submitted = submit;
        dialog->setCardArea(QRect(0, 0, width(), height() * 2 / 3));
        dialog->show();
        dialog->raise();
        dialog->fitCard();
        copy->setFocus();
        keyboard->show();
        keyboard->place();
    }
    void job(std::function<void(FileJob &)> operation, std::function<void(const FileJob &)> done) {
        if (pending)
            return;
        pending = true;
        setProperty("utilityBusy", true);
        status->setText(i18n::text(QStringLiteral("Datei wird verarbeitet …")));
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
        if (value.isEmpty() || value.size() > 128 || value == "." || value == ".." || value.contains('/') ||
            value.contains('\\') || value.contains(QChar(0))) {
            status->setText(i18n::text(QStringLiteral("Bitte einen einfachen Dateinamen ohne Ordner angeben.")));
            return {};
        }
        return value;
    }
    void loadText() {
        if (pending)
            return;
        if (editor->document()->isModified()) {
            status->setText(i18n::text(QStringLiteral("Bitte zuerst speichern oder den Text leeren.")));
            return;
        }
        const auto file = name();
        if (file.isEmpty())
            return;
        const auto root = directory;
        const int revision = editor->document()->revision();
        job(
            [root, file](FileJob &task) {
                QFile input(root + '/' + file);
                if (QFileInfo(input).isSymLink() || !input.open(QIODevice::ReadOnly)) {
                    task.error = i18n::text(QStringLiteral("Datei kann nicht geöffnet werden."));
                    return;
                }
                if (input.size() > textLimit) {
                    task.error = i18n::text(QStringLiteral("Maximale Textgröße: 1 MiB."));
                    return;
                }
                task.bytes = input.readAll();
                const auto text = task.bytes.startsWith("\xef\xbb\xbf") ? task.bytes.mid(3) : task.bytes;
                if (input.error() != QFile::NoError || text.contains('\0') || QString::fromUtf8(text).toUtf8() != text)
                    task.error = i18n::text(QStringLiteral("Nur gültige UTF-8-Textdateien werden unterstützt."));
            },
            [this, file, revision](const FileJob &task) {
                if (!task.error.isEmpty())
                    return;
                if (editor->document()->revision() != revision) {
                    status->setText(
                        i18n::text(QStringLiteral("Der Text wurde inzwischen bearbeitet. Öffnen wurde abgebrochen.")));
                    return;
                }
                loadedBom = task.bytes.startsWith("\xef\xbb\xbf");
                editor->setPlainText(QString::fromUtf8(loadedBom ? task.bytes.mid(3) : task.bytes));
                editor->document()->setModified(false);
                loadedName = file;
                loadedHash = QCryptographicHash::hash(task.bytes, QCryptographicHash::Sha256);
            });
    }
    void saveText() {
        const auto file = name();
        const auto body = editor->toPlainText().toUtf8();
        const bool withBom = file == loadedName && loadedBom;
        const auto bytes = withBom ? QByteArray("\xef\xbb\xbf") + body : body;
        if (file.isEmpty())
            return;
        if (bytes.size() > textLimit) {
            status->setText(i18n::text(QStringLiteral("Maximale Textgröße: 1 MiB.")));
            return;
        }
        const auto root = directory;
        const auto expected = file == loadedName ? loadedHash : QByteArray();
        job(
            [root, file, bytes, expected](FileJob &task) {
                if (QFileInfo(root).isSymLink() || !QDir().mkpath(root)) {
                    task.error = i18n::text(QStringLiteral("Speicherordner nicht verfügbar."));
                    return;
                }
                const auto path = root + '/' + file;
                if (QFileInfo(path).isSymLink()) {
                    task.error = i18n::text(QStringLiteral("Symbolische Links werden nicht überschrieben."));
                    return;
                }
                if (QFileInfo::exists(path)) {
                    QFile old(path);
                    if (expected.isEmpty() || !old.open(QIODevice::ReadOnly) || old.size() > textLimit ||
                        QCryptographicHash::hash(old.readAll(), QCryptographicHash::Sha256) != expected) {
                        task.error = i18n::text(QStringLiteral(
                            "Datei existiert bereits oder wurde geändert. Bitte einen neuen Namen wählen."));
                        return;
                    }
                }
                QSaveFile output(path);
                if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
                    task.error = i18n::text(QStringLiteral("Speichern fehlgeschlagen. Originaldatei bleibt erhalten."));
                    return;
                }
                task.bytes = bytes;
            },
            [this, file, bytes, body, withBom](const FileJob &task) {
                if (!task.error.isEmpty())
                    return;
                loadedName = file;
                loadedBom = withBom;
                loadedHash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
                if (editor->toPlainText().toUtf8() == body)
                    editor->document()->setModified(false);
                status->setText(i18n::text(QStringLiteral("Gespeichert: ")) + directory + '/' + file);
            });
    }
    void saveSketch() {
        const auto image = sketch->image;
        const auto root = directory;
        const auto file =
            QStringLiteral("sketch-") + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + ".png";
        job(
            [root, file, image](FileJob &task) {
                if (QFileInfo(root).isSymLink() || !QDir().mkpath(root)) {
                    task.error = i18n::text(QStringLiteral("Speicherordner nicht verfügbar."));
                    return;
                }
                const auto path = root + '/' + file;
                if (QFileInfo::exists(path)) {
                    task.error = i18n::text(QStringLiteral("Datei existiert bereits. Bitte erneut speichern."));
                    return;
                }
                QSaveFile output(path);
                if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") || !output.commit())
                    task.error = i18n::text(QStringLiteral("Zeichnung konnte nicht gespeichert werden."));
            },
            [this, image, file](const FileJob &task) {
                if (task.error.isEmpty()) {
                    if (sketch->image == image)
                        sketch->dirty = false;
                    status->setText(i18n::text(QStringLiteral("Gespeichert: ")) + directory + '/' + file);
                }
            });
    }
    void stopShell() {
#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        if (shellGroup > 0)
            ::kill(-shellGroup, SIGKILL);
        shellGroup = 0;
#endif
        if (shell->state() != QProcess::NotRunning)
            shell->kill();
    }
    bool simpleName(const QString &file) const {
        if (file.isEmpty() || file.size() > 128 || file == "." || file == ".." || file.contains('/') ||
            file.contains('\\'))
            return false;
        for (const auto character : file)
            if (character.unicode() < 32)
                return false;
        return true;
    }
    void confirm(const QString &title, std::function<void()> action) {
        if (confirmation)
            return;
        auto *dialog =
            new ui::ChoiceModal(this, {{}, title, {}}, {i18n::text(QStringLiteral("Abbrechen")), "rrToolsCancel"},
                                {i18n::text(QStringLiteral("Bestätigen")), "rrToolsConfirm"});
        confirmation = dialog;
        connect(dialog->secondaryButton(), &QPushButton::clicked, dialog, &QObject::deleteLater);
        connect(dialog->primaryButton(), &QPushButton::clicked, this, [dialog, action] {
            dialog->deleteLater();
            action();
        });
        dialog->setGeometry(rect());
        dialog->show();
        dialog->raise();
        dialog->fitCard();
    }
    void exportReport(const QByteArray &bytes, const QString &prefix) {
        if (pending)
            return;
        const auto root = directory;
        const auto file = prefix + '-' + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + ".txt";
        job(
            [root, file, bytes](FileJob &task) {
                if (QFileInfo(root).isSymLink() || !QDir().mkpath(root) || QFileInfo::exists(root + '/' + file)) {
                    task.error = i18n::text("Exportordner nicht verfügbar");
                    return;
                }
                QSaveFile output(root + '/' + file);
                if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
                    task.error = i18n::text("Export fehlgeschlagen");
            },
            [this, file](const FileJob &task) {
                if (task.error.isEmpty())
                    status->setText(i18n::text("Gespeichert: ") + directory + '/' + file);
            });
    }
    void refreshFiles() {
        if (pending)
            return;
        const auto root = directory;
        job(
            [root](FileJob &task) {
                if (QFileInfo(root).isSymLink()) {
                    task.error = i18n::text("Verlinkte Speicherordner werden nicht geöffnet");
                    return;
                }
                const auto entries = QDir(root).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
                if (entries.size() > 2000) {
                    task.error = i18n::text("Zu viele Dateien im Ordner");
                    return;
                }
                for (const auto &entry : entries)
                    task.rows.append(QJsonObject{{"name", entry.fileName()},
                                                 {"size", double(entry.size())},
                                                 {"modified", double(entry.lastModified().toMSecsSinceEpoch())}});
            },
            [this](const FileJob &task) {
                if (!task.error.isEmpty())
                    return;
                files->clear();
                for (const auto &value : task.rows) {
                    const auto row = value.toObject();
                    auto *item =
                        new QTreeWidgetItem(files, {row["name"].toString(),
                                                    QString::number(row["size"].toDouble() / 1024.0, 'f', 1) + " KiB"});
                    item->setData(0, Qt::UserRole, row);
                }
                status->setText(task.rows.isEmpty()
                                    ? i18n::text("Noch keine Dateien · ") + directory
                                    : QString(i18n::text("%1 Dateien · %2")).arg(task.rows.size()).arg(directory));
            });
    }
    QJsonObject selectedFile() const {
        return files->currentItem() ? files->currentItem()->data(0, Qt::UserRole).toJsonObject() : QJsonObject();
    }
    void openSelected() {
        const auto selected = selectedFile();
        const auto file = selected["name"].toString();
        if (!simpleName(file) || pending)
            return;
        if (file.endsWith(".png", Qt::CaseInsensitive)) {
            const auto root = directory;
            const auto load = [this, root, file] {
                job(
                    [root, file](FileJob &task) {
                        const auto path = root + '/' + file;
                        if (QFileInfo(path).isSymLink() || QFileInfo(path).size() > 8 * 1024 * 1024) {
                            task.error = i18n::text("Bild ist zu groß oder verlinkt");
                            return;
                        }
                        QImageReader reader(path, "PNG");
                        const auto size = reader.size();
                        if (!size.isValid() || size.width() > 2260 || size.height() > 2260) {
                            task.error = i18n::text("Bildabmessungen nicht unterstützt");
                            return;
                        }
                        task.picture = reader.read();
                        if (task.picture.isNull())
                            task.error = i18n::text("PNG kann nicht gelesen werden");
                    },
                    [this](const FileJob &task) {
                        if (task.error.isEmpty()) {
                            sketch->replaceImage(task.picture);
                            pages->setCurrentIndex(1);
                            selectTab(1);
                        }
                    });
            };
            if (sketch->dirty)
                discard(load);
            else
                load();
        } else {
            if (editor->document()->isModified()) {
                status->setText(i18n::text("Text zuerst speichern oder Neu wählen"));
                return;
            }
            filename->setText(file);
            pages->setCurrentIndex(2);
            selectTab(2);
            loadText();
        }
    }
    void changeSelected(bool remove) {
        if (pending)
            return;
        const auto selected = selectedFile();
        const auto file = selected["name"].toString();
        const auto target = renameInput->text().trimmed();
        if (!simpleName(file)) {
            status->setText(i18n::text("Bitte eine Datei auswählen"));
            return;
        }
        if (!remove && !simpleName(target)) {
            status->setText(i18n::text("Bitte einen einfachen neuen Dateinamen angeben"));
            return;
        }
        const auto root = directory;
        const auto change = [this, root, file, target, remove, selected] {
            job(
                [root, file, target, remove, selected](FileJob &task) {
                    const QFileInfo source(root + '/' + file);
                    if (QFileInfo(root).isSymLink() || source.isSymLink() || !source.isFile() ||
                        source.size() != qint64(selected["size"].toDouble()) ||
                        source.lastModified().toMSecsSinceEpoch() != qint64(selected["modified"].toDouble())) {
                        task.error = i18n::text("Datei wurde verändert. Liste neu laden.");
                        return;
                    }
                    if (remove) {
                        if (!QFile::remove(source.filePath()))
                            task.error = i18n::text("Löschen fehlgeschlagen");
                    } else if (QFileInfo::exists(root + '/' + target) ||
                               !QFile::rename(source.filePath(), root + '/' + target))
                        task.error = i18n::text("Zieldatei existiert oder Umbenennen fehlgeschlagen");
                },
                [this](const FileJob &task) {
                    if (task.error.isEmpty())
                        refreshFiles();
                });
        };
        if (remove)
            confirm(i18n::text("Datei wirklich löschen?\n") + file, change);
        else
            change();
    }
    void hashSelected() {
        const auto file = selectedFile()["name"].toString();
        if (!simpleName(file) || pending)
            return;
        const auto root = directory;
        job(
            [root, file](FileJob &task) {
                QFile input(root + '/' + file);
                if (QFileInfo(root).isSymLink() || QFileInfo(input).isSymLink() || !input.open(QIODevice::ReadOnly) ||
                    input.size() > 64 * 1024 * 1024) {
                    task.error = i18n::text("Datei kann nicht geprüft werden");
                    return;
                }
                QCryptographicHash hash(QCryptographicHash::Sha256);
                if (!hash.addData(&input)) {
                    task.error = "Lesefehler";
                    return;
                }
                task.report = file + "\nSHA-256: " + QString::fromLatin1(hash.result().toHex());
            },
            [this](const FileJob &task) {
                if (task.error.isEmpty()) {
                    deviceReport->setPlainText(task.report);
                    pages->setCurrentIndex(4);
                    selectTab(4);
                }
            });
    }
    void refreshDevice() {
        if (pending)
            return;
        const auto root = directory;
        job(
            [root](FileJob &task) {
                QString existing = root;
                while (!QFileInfo::exists(existing) && existing != "/")
                    existing = QFileInfo(existing).absolutePath();
                const QStorageInfo storage(existing);
                QString firmware = i18n::text("Nicht auf einem Kobo/tolino erkannt");
                QFile version("/mnt/onboard/.kobo/version");
                if (version.open(QIODevice::ReadOnly)) {
                    const auto text = QString::fromUtf8(version.read(16384));
                    const auto match = QRegularExpression("(?:^|[,\\s])([456]\\.\\d+\\.\\d+(?:\\.\\d+)?)").match(text);
                    if (match.hasMatch())
                        firmware = match.captured(1);
                }
                QString battery = i18n::text("Nicht verfügbar");
                for (const auto &supply :
                     QDir("/sys/class/power_supply").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    QFile capacity("/sys/class/power_supply/" + supply + "/capacity");
                    bool valid = false;
                    if (capacity.open(QIODevice::ReadOnly)) {
                        const int value = capacity.read(16).trimmed().toInt(&valid);
                        if (valid && value >= 0 && value <= 100) {
                            battery = QString::number(value) + " %";
                            break;
                        }
                    }
                }
                task.report =
                    QString(i18n::text("ROREADER · Geräteinformationen\n\nSystem: %1\nKernel: %2\nArchitektur: %3\nQt: "
                                       "%4\nFirmware: "
                                       "%5\nAkku: %6\nFreier Speicher: %7 MiB\nGesamtspeicher: %8 MiB\nDokumente: "
                                       "%9\n\nSicherungen "
                                       "enthalten nur Dateien aus dem ROREADER-Dokumentordner, keine Lesedatenbank "
                                       "oder Firmware."))
                        .arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion(),
                             QSysInfo::currentCpuArchitecture(), QString::fromLatin1(qVersion()), firmware, battery)
                        .arg(storage.bytesAvailable() / 1024 / 1024)
                        .arg(storage.bytesTotal() / 1024 / 1024)
                        .arg(root);
            },
            [this](const FileJob &task) {
                if (task.error.isEmpty()) {
                    deviceReport->setPlainText(task.report);
                    infoLoaded = true;
                }
            });
    }
    void backupDocuments() {
        if (pending)
            return;
        const auto root = directory;
        const auto folder = "backup-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz");
        job(
            [root, folder](FileJob &task) {
                if (QFileInfo(root).isSymLink()) {
                    task.error = i18n::text("Verlinkter Dokumentordner");
                    return;
                }
                const auto entries = QDir(root).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
                qint64 total = 0;
                for (const auto &entry : entries)
                    total += entry.size();
                if (entries.size() > 200 || total > 64 * 1024 * 1024) {
                    task.error = i18n::text("Sicherungslimit: 200 Dateien oder 64 MiB");
                    return;
                }
                const auto target = root + '/' + folder;
                if (QFileInfo::exists(target) || !QDir().mkpath(target)) {
                    task.error = i18n::text("Sicherungsordner kann nicht erstellt werden");
                    return;
                }
                QJsonArray manifest;
                qint64 copiedBytes = 0;
                for (const auto &entry : entries) {
                    QFile input(entry.filePath());
                    if (!input.open(QIODevice::ReadOnly)) {
                        task.error = i18n::text("Sicherung unvollständig: Lesefehler");
                        return;
                    }
                    const auto bytes = input.read(64 * 1024 * 1024 + 1);
                    copiedBytes += bytes.size();
                    if (copiedBytes > 64 * 1024 * 1024 || input.error() != QFile::NoError) {
                        task.error = i18n::text("Sicherung unvollständig: Datei zu groß oder Lesefehler");
                        return;
                    }
                    QSaveFile output(target + '/' + entry.fileName());
                    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
                        task.error = i18n::text("Sicherung unvollständig: Schreibfehler");
                        return;
                    }
                    manifest.append(QJsonObject{
                        {"name", entry.fileName()},
                        {"sha256",
                         QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}});
                }
                const auto bytes = QJsonDocument(manifest).toJson();
                QSaveFile summary(target + "/backup-manifest.json");
                if (!summary.open(QIODevice::WriteOnly) || summary.write(bytes) != bytes.size() || !summary.commit()) {
                    task.error = i18n::text("Sicherungsmanifest fehlt");
                    return;
                }
                task.report = i18n::text("Sicherung abgeschlossen: ") + target;
            },
            [this](const FileJob &task) {
                if (task.error.isEmpty())
                    status->setText(task.report);
            });
    }
    void selectTab(int page) {
        const char *ids[] = {"rrToolsTerminal", "rrToolsSketch", "rrToolsEditor",
                             "rrToolsFiles",    "rrToolsMods",   "rrToolsGames"};
        if (page >= 0 && page < 6)
            if (auto *tab = findChild<QPushButton *>(ids[page]))
                tab->setChecked(true);
    }
    void buildFilesPage() {
        auto *page = new QWidget(pages);
        pages->addWidget(page);
        auto *rows = new QVBoxLayout(page);
        auto *description = new ui::InterfaceLabel(i18n::text("Deine Texte, Zeichnungen und Exporte"), page);
        description->setWordWrap(true);
        rows->addWidget(description);
        files = new QTreeWidget(page);
        files->setObjectName("rrFileList");
        files->setColumnCount(2);
        files->setHeaderLabels({i18n::text("Datei"), i18n::text("Größe")});
        files->setRootIsDecorated(false);
        files->setColumnWidth(0, ui::px(440));
        rows->addWidget(files, 1);
        ui::readerStyle(files, QString("QTreeWidget{font-size:%1px;border:1px solid palette(mid);border-radius:16px;} "
                                       "QTreeWidget::item{min-height:%2px;padding:8px;}")
                                   .arg(int(m::chrome::labelText))
                                   .arg(ui::px(64)));
        connect(files, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *, int) { openSelected(); });
        auto *actions = new QHBoxLayout;
        rows->addLayout(actions);
        button(page, actions, "rrFilesRefresh", i18n::text("Neu laden"), [this] { refreshFiles(); });
        button(page, actions, "rrFilesOpen", i18n::text("Öffnen"), [this] { openSelected(); });
        button(page, actions, "rrFilesHash", "SHA-256", [this] { hashSelected(); });
        auto *rename = new QHBoxLayout;
        rows->addLayout(rename);
        renameInput = new QLineEdit(page);
        renameInput->setObjectName("rrFileRenameInput");
        renameInput->setPlaceholderText(i18n::text("Neuer Dateiname"));
        renameInput->setMaxLength(128);
        rename->addWidget(renameInput, 1);
        button(page, rename, "rrFilesName", i18n::text("Name"), [this] { showKeyboard(renameInput); });
        auto *changes = new QHBoxLayout;
        rows->addLayout(changes);
        button(page, changes, "rrFilesRename", i18n::text("Umbenennen"), [this] { changeSelected(false); });
        button(page, changes, "rrFilesDelete", i18n::text("Löschen"), [this] { changeSelected(true); });
    }
    void buildModsPage() {
        auto *page = new QWidget(pages);
        pages->addWidget(page);
        auto *rows = new QVBoxLayout(page);
        deviceReport = new QPlainTextEdit(page);
        deviceReport->setObjectName("rrDeviceReport");
        deviceReport->setReadOnly(true);
        deviceReport->setPlainText(i18n::text("Geräteinformationen laden oder eine Datei prüfen."));
        rows->addWidget(deviceReport, 1);
        ui::readerStyle(deviceReport, QString("QPlainTextEdit{font-size:%1px;background:palette(base);color:palette("
                                              "text);border:1px solid palette(mid);border-radius:16px;padding:12px;}")
                                          .arg(int(m::chrome::labelText)));
        auto *actions = new QHBoxLayout;
        rows->addLayout(actions);
        button(page, actions, "rrDeviceRefresh", i18n::text("Aktualisieren"), [this] { refreshDevice(); });
        button(page, actions, "rrDeviceExport", i18n::text("Exportieren"),
               [this] { exportReport(deviceReport->toPlainText().toUtf8(), "diagnose"); });
        button(page, rows, "rrDocumentsBackup", i18n::text("Dokumente sichern"), [this] { backupDocuments(); });
        auto *diagnostics = new QGridLayout;
        rows->addLayout(diagnostics);
        const QStringList names{i18n::text("Prozesse"), i18n::text("Speicher"), i18n::text("Netzwerk"),
                                i18n::text("System")};
        const QStringList commands{"ps", "df -h", "ip addr 2>/dev/null || ifconfig", "uname -a"};
        for (int i = 0; i < names.size(); ++i) {
            auto *action = button(page, nullptr, qPrintable(QString("rrDiagnostic%1").arg(i)), names[i],
                                  [this, script = commands[i]] {
                                      pages->setCurrentIndex(0);
                                      selectTab(0);
                                      command->setText(script);
                                      runCommand();
                                  });
            diagnostics->addWidget(action, i / 2, i % 2);
        }
    }
    void appendTerminal(QByteArray bytes, bool final = false) {
        bytes.prepend(utf8Tail);
        utf8Tail.clear();
        if (!final && !bytes.isEmpty()) {
            int lead = bytes.size() - 1;
            while (lead > 0 && (uchar(bytes[lead]) & 0xc0) == 0x80)
                --lead;
            const auto value = uchar(bytes[lead]);
            const int needed = value >= 0xf0 ? 4 : value >= 0xe0 ? 3 : value >= 0xc0 ? 2 : 1;
            if (bytes.size() - lead < needed) {
                utf8Tail = bytes.mid(lead);
                bytes.truncate(lead);
            }
        }
        auto cursor = terminal->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(QString::fromUtf8(bytes));
        terminal->setTextCursor(cursor);
        if (terminal->document()->characterCount() > textLimit) {
            auto trim = terminal->textCursor();
            trim.movePosition(QTextCursor::Start);
            trim.setPosition(int(terminal->document()->characterCount() - textLimit), QTextCursor::KeepAnchor);
            trim.removeSelectedText();
        }
        terminal->ensureCursorVisible();
    }
    void flushTerminal(bool final = false) {
        terminalFrame.stop();
        const int start = cwdMarker.isEmpty() ? -1 : terminalBuffer.indexOf(cwdMarker);
        if (start >= 0) {
            const int end = terminalBuffer.indexOf(char(31), start + cwdMarker.size());
            if (end >= 0) {
                appendTerminal(terminalBuffer.left(start));
                shellDirectory =
                    QString::fromUtf8(terminalBuffer.mid(start + cwdMarker.size(), end - start - cwdMarker.size()));
                workingDirectory->setText(shellDirectory);
                terminalBuffer.remove(0, end + 1);
            } else if (!final) {
                appendTerminal(terminalBuffer.left(start));
                terminalBuffer.remove(0, start);
                return;
            }
        }
        int keep = 0;
        if (!final)
            for (int size = 1; size < cwdMarker.size() && size <= terminalBuffer.size(); ++size)
                if (terminalBuffer.endsWith(cwdMarker.left(size)))
                    keep = size;
        appendTerminal(terminalBuffer.left(terminalBuffer.size() - keep), final);
        terminalBuffer = terminalBuffer.right(keep);
    }
    void runCommand() {
        const auto text = command->text();
        if (shell->state() != QProcess::NotRunning) {
            shell->write(text.toUtf8() + "\n");
            command->clear();
            return;
        }
        if (text.trimmed().isEmpty())
            return;
        const auto cwd = workingDirectory->text();
        if (cwd.contains(QChar(0)) || cwd.contains('\n')) {
            status->setText(i18n::text("Ungültiger Arbeitsordner"));
            return;
        }
        if (history->count() == 101)
            history->removeItem(1);
        history->addItem(text.left(80), text);
        history->setCurrentIndex(history->count() - 1);
        historyIndex = history->count();
        terminal->appendPlainText("$ " + text);
        outputBytes = 0;
        terminalBuffer.clear();
        utf8Tail.clear();
        command->clear();
        const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
        cwdMarker = QByteArray(1, char(30)) + "ROREADER_" + token.toUtf8() + ":";
        const auto script =
            text + "\n__rr_result=$?\nprintf '\\036ROREADER_" + token + ":%s\\037' \"$PWD\"\nexit \"$__rr_result\"\n";
        shell->setWorkingDirectory(cwd);
        shell->start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), script});
        deadline->start(60000);
    }
    void discard(std::function<void()> action) {
        if (confirmation)
            return;
        auto *dialog =
            new ui::ChoiceModal(this, {{}, i18n::text(QStringLiteral("Ungespeicherte Änderungen verwerfen?")), {}},
                                {i18n::text(QStringLiteral("Behalten")), "rrToolsKeep"},
                                {i18n::text(QStringLiteral("Verwerfen")), "rrToolsDiscard"});
        confirmation = dialog;
        connect(dialog->findChild<QPushButton *>("rrToolsKeep"), &QPushButton::clicked, dialog, &QObject::deleteLater);
        connect(dialog->findChild<QPushButton *>("rrToolsDiscard"), &QPushButton::clicked, this, [dialog, action] {
            dialog->deleteLater();
            action();
        });
        dialog->setGeometry(rect());
        dialog->show();
        dialog->raise();
        dialog->fitCard();
    }
    void closeTools() {
        if (pending) {
            status->setText(i18n::text(QStringLiteral("Bitte das Speichern abwarten.")));
            return;
        }
        if (keyboard)
            keyboard->hide();
        auto close = [this] {
            stopShell();
            hide();
            deleteLater();
        };
        if (sketch->dirty || editor->document()->isModified())
            discard(close);
        else
            close();
    }

  public:
    void dismiss() {
        closeTools();
    }
    void quiesce() {
        stopShell();
        if (keyboard)
            keyboard->hide();
        if (inputDialog) {
            inputDialog->hide();
            inputDialog->deleteLater();
            inputDialog.clear();
        }
    }
    explicit Tools(QWidget *host, KeyboardFactory factory, const QString &path)
        : QWidget(host), makeKeyboard(std::move(factory)), directory(path.isEmpty() ? toolsDirectory() : path) {
        setObjectName("rrTools");
        setAutoFillBackground(true);
        setGeometry(host->rect());
        host->installEventFilter(this);
        auto *rows = new QVBoxLayout(this);
        rows->setContentsMargins(m::chrome::inset, m::chrome::inset, m::chrome::inset, m::chrome::inset);
        rows->setSpacing(m::spacing::row);
        auto *header = new QHBoxLayout;
        rows->addLayout(header);
        auto *title = new ui::InterfaceLabel(i18n::text(QStringLiteral("ROREADER · Werkzeuge")), this);
        ui::readerStyle(title, ui::styles::font(m::text::body));
        header->addWidget(title, 1);
        button(this, header, "rrToolsClose", i18n::text(QStringLiteral("Zurück")), [this] { closeTools(); });
        auto *tabs = new QGridLayout;
        rows->addLayout(tabs);
        pages = new QStackedWidget(this);
        auto *tabGroup = new QButtonGroup(this);
        const QStringList pageNames{i18n::text("Terminal"), i18n::text("Sketchpad"),    i18n::text("Texteditor"),
                                    i18n::text("Dateien"),  i18n::text("Gerät / Mods"), i18n::text("Spiele")};
        const char *pageIds[] = {"rrToolsTerminal", "rrToolsSketch", "rrToolsEditor",
                                 "rrToolsFiles",    "rrToolsMods",   "rrToolsGames"};
        for (int page = 0; page < pageNames.size(); ++page) {
            auto *tab = button(this, nullptr, pageIds[page], pageNames[page], [this, page] {
                if (keyboard)
                    keyboard->hide();
                pages->setCurrentIndex(page);
                if (page == 3)
                    refreshFiles();
                if (page == 4 && !infoLoaded)
                    refreshDevice();
            });
            tab->setCheckable(true);
            tabGroup->addButton(tab);
            tab->setChecked(page == 0);
            tabs->addWidget(tab, page / 3, page % 3);
        }
        rows->addWidget(pages, 1);
        auto *terminalPage = new QWidget(pages);
        pages->addWidget(terminalPage);
        auto *terminalRows = new QVBoxLayout(terminalPage);
        terminal = new QPlainTextEdit(terminalPage);
        terminal->setObjectName("rrTerminalOutput");
        terminal->setReadOnly(true);
        terminal->setMaximumBlockCount(1000);
        terminal->setPlainText(
            i18n::text(QStringLiteral("ROREADER Shell · Verlauf, Arbeitsordner und Programmeingabe\nKein "
                                      "Vollbild-Terminal. Befehle laufen mit Reader-Rechten.")));
        auto *terminalOptions = new QHBoxLayout;
        terminalRows->addLayout(terminalOptions);
        workingDirectory = new QLineEdit(terminalPage);
        workingDirectory->setObjectName("rrTerminalDirectory");
        shellDirectory = QFileInfo::exists(directory) ? directory : QDir::homePath();
        workingDirectory->setText(shellDirectory);
        terminalOptions->addWidget(workingDirectory, 1);
        button(terminalPage, terminalOptions, "rrTerminalDirectoryKeyboard", i18n::text("Ordner"),
               [this] { showKeyboard(workingDirectory); });
        history = new QComboBox(terminalPage);
        ui::readerStyle(history,
                        QString("QComboBox{font-size:%1px;min-height:%2px;border:1px solid "
                                "palette(mid);border-radius:16px;padding:8px;} QComboBox "
                                "QAbstractItemView{font-size:%1px;selection-background-color:palette(highlight);}")
                            .arg(int(m::chrome::labelText))
                            .arg(ui::px(64)));
        history->setObjectName("rrTerminalHistory");
        history->addItem(i18n::text("Verlauf"));
        terminalRows->addWidget(history);
        connect(history, qOverload<int>(&QComboBox::activated), this, [this](int index) {
            if (index > 0)
                command->setText(history->itemData(index).toString());
        });
        auto *consoleActions = new QHBoxLayout;
        terminalRows->addLayout(consoleActions);
        button(terminalPage, consoleActions, "rrTerminalClear", i18n::text("Leeren"), [this] { terminal->clear(); });
        button(terminalPage, consoleActions, "rrTerminalExport", i18n::text("Exportieren"),
               [this] { exportReport(terminal->toPlainText().toUtf8(), "terminal"); });
        terminalRows->addWidget(terminal, 1);
        command = new QLineEdit(terminalPage);
        command->setObjectName("rrTerminalCommand");
        command->setMaxLength(4096);
        command->installEventFilter(this);
        command->setPlaceholderText(i18n::text(QStringLiteral("Shell-Befehl")));
        command->setMinimumHeight(m::chrome::actionHeight);
        terminalRows->addWidget(command);
        auto *terminalActions = new QHBoxLayout;
        terminalRows->addLayout(terminalActions);
        button(terminalPage, terminalActions, "rrTerminalKeyboard", i18n::text(QStringLiteral("Tastatur")),
               [this] { showKeyboard(command); });
        button(terminalPage, terminalActions, "rrTerminalRun", i18n::text(QStringLiteral("Ausführen")),
               [this] { runCommand(); });
        button(terminalPage, terminalActions, "rrTerminalStop", i18n::text(QStringLiteral("Stoppen")),
               [this] { stopShell(); });
        shell = new QProcess(this);
        connect(shell, &QProcess::started, this, [this] { shellGroup = shell->processId(); });
        connect(shell, &QProcess::started, this, [this] {
            findChild<QPushButton *>("rrTerminalRun")->setText(i18n::text("Senden"));
            command->setPlaceholderText(i18n::text("Programmeingabe · Enter sendet eine Zeile"));
        });
        shell->setProcessChannelMode(QProcess::MergedChannels);
#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        shell->setChildProcessModifier([] { ::setsid(); });
#endif
        terminalFrame.setSingleShot(true);
        terminalFrame.setInterval(16);
        connect(&terminalFrame, &QTimer::timeout, this, [this] { flushTerminal(); });
        connect(shell, &QProcess::readyReadStandardOutput, this, [this] {
            const auto output = shell->readAllStandardOutput();
            terminalBuffer += output.left(int(qMax<qint64>(0, textLimit - outputBytes)));
            outputBytes += output.size();
            if (!terminalFrame.isActive())
                terminalFrame.start();
            if (outputBytes > textLimit) {
                flushTerminal(true);
                terminal->appendPlainText(i18n::text("Ausgabelimit erreicht."));
                stopShell();
            }
        });
        deadline = new QTimer(this);
        deadline->setSingleShot(true);
        connect(deadline, &QTimer::timeout, this, [this] {
            terminal->appendPlainText(i18n::text(QStringLiteral("Zeitlimit erreicht.")));
            stopShell();
        });
        connect(shell, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus) {
                    stopShell();
                    deadline->stop();
                    flushTerminal(true);
                    terminal->appendPlainText(i18n::text(QStringLiteral("\nExit: %1")).arg(code));
                    findChild<QPushButton *>("rrTerminalRun")->setText(i18n::text("Ausführen"));
                    command->setPlaceholderText(i18n::text("Shell-Befehl"));
                });
        connect(shell, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            status->setText(shell->errorString());
            if (shell->state() == QProcess::NotRunning) {
                deadline->stop();
                flushTerminal(true);
                findChild<QPushButton *>("rrTerminalRun")->setText(i18n::text("Ausführen"));
            }
        });
        connect(command, &QLineEdit::returnPressed, this, [this] { runCommand(); });
        auto *sketchPage = new QWidget(pages);
        pages->addWidget(sketchPage);
        auto *sketchRows = new QVBoxLayout(sketchPage);
        auto *sketchActions = new QHBoxLayout;
        sketchRows->addLayout(sketchActions);
        sketch = new SketchCanvas(sketchPage);
        sketchRows->addWidget(sketch, 1);
        auto *eraser =
            button(sketchPage, sketchActions, "rrSketchEraser", i18n::text(QStringLiteral("Radierer")), [] {});
        eraser->setCheckable(true);
        connect(eraser, &QPushButton::toggled, this, [this, eraser](bool active) {
            sketch->eraser = active;
            eraser->setText(active ? i18n::text("Stift") : i18n::text("Radierer"));
        });
        button(sketchPage, sketchActions, "rrSketchUndo", i18n::text(QStringLiteral("Rückgängig")),
               [this] { sketch->undoStroke(); });
        button(sketchPage, sketchActions, "rrSketchClear", i18n::text(QStringLiteral("Neu")), [this] {
            auto clear = [this] { sketch->clearPaper(); };
            if (sketch->dirty)
                discard(clear);
            else
                clear();
        });
        button(sketchPage, sketchActions, "rrSketchSave", i18n::text(QStringLiteral("Speichern")),
               [this] { saveSketch(); });
        auto *brushRow = new QHBoxLayout;
        sketchRows->insertLayout(1, brushRow);
        auto *brush = new QSlider(Qt::Horizontal, sketchPage);
        brush->setObjectName("rrSketchWidth");
        brush->setRange(1, 16);
        brush->setValue(5);
        brushRow->addWidget(brush, 1);
        connect(brush, &QSlider::valueChanged, this, [this](int size) { sketch->penWidth = size; });
        button(sketchPage, brushRow, "rrSketchRedo", i18n::text("Wiederholen"), [this] { sketch->redoStroke(); });
        auto *shade = button(sketchPage, brushRow, "rrSketchShade", i18n::text("Grau"), [] {});
        connect(shade, &QPushButton::clicked, this, [this, shade] {
            sketch->ink = sketch->ink == Qt::black ? QColor(110, 110, 110) : QColor(Qt::black);
            shade->setText(sketch->ink == Qt::black ? i18n::text("Grau") : i18n::text("Schwarz"));
        });
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
        button(editorPage, fileActions, "rrTextFilenameKeyboard", i18n::text(QStringLiteral("Dateiname")),
               [this] { showKeyboard(filename); });
        button(editorPage, fileActions, "rrTextOpen", i18n::text(QStringLiteral("Öffnen")), [this] { loadText(); });
        button(editorPage, fileActions, "rrTextSave", i18n::text(QStringLiteral("Speichern")), [this] { saveText(); });
        button(editorPage, fileActions, "rrTextNew", i18n::text(QStringLiteral("Neu")), [this] {
            auto clear = [this] {
                editor->clear();
                editor->document()->setModified(false);
                loadedHash.clear();
                loadedName.clear();
            };
            if (editor->document()->isModified())
                discard(clear);
            else
                clear();
        });
        editor = new QPlainTextEdit(editorPage);
        editor->setObjectName("rrTextDocument");
        editorRows->addWidget(editor, 1);
        insert = new QLineEdit(editorPage);
        insert->setObjectName("rrTextInsert");
        insert->setPlaceholderText(i18n::text(QStringLiteral("Text an der Auswahl einfügen")));
        insert->setMaxLength(4096);
        insert->setMinimumHeight(m::chrome::actionHeight);
        editorRows->addWidget(insert);
        auto *searchRow = new QHBoxLayout;
        editorRows->addLayout(searchRow);
        findInput = new QLineEdit(editorPage);
        findInput->setObjectName("rrTextFind");
        findInput->setPlaceholderText(i18n::text("Im Text suchen"));
        searchRow->addWidget(findInput, 1);
        button(editorPage, searchRow, "rrTextFindKeyboard", i18n::text("Suche"), [this] { showKeyboard(findInput); });
        button(editorPage, searchRow, "rrTextFindNext", "Weiter", [this] {
            if (findInput->text().isEmpty())
                return;
            if (!editor->find(findInput->text())) {
                auto cursor = editor->textCursor();
                cursor.movePosition(QTextCursor::Start);
                editor->setTextCursor(cursor);
                if (!editor->find(findInput->text()))
                    status->setText(i18n::text("Kein Treffer"));
            }
        });
        auto *textActions = new QHBoxLayout;
        editorRows->addLayout(textActions);
        button(editorPage, textActions, "rrTextKeyboard", i18n::text(QStringLiteral("Tastatur")),
               [this] { showKeyboard(insert); });
        button(editorPage, textActions, "rrTextAdd", i18n::text(QStringLiteral("Einfügen")), [this] {
            editor->insertPlainText(insert->text());
            insert->clear();
        });
        button(editorPage, textActions, "rrTextNewline", i18n::text(QStringLiteral("Neue Zeile")),
               [this] { editor->insertPlainText("\n"); });
        button(editorPage, textActions, "rrTextUndo", i18n::text(QStringLiteral("Rückgängig")),
               [this] { editor->undo(); });
        buildFilesPage();
        buildModsPage();
        pages->addWidget(createGamesPanel(pages));
        status = new ui::InterfaceLabel(directory, this);
        status->setObjectName("rrToolsStatus");
        status->setWordWrap(true);
        status->setTextFormat(Qt::PlainText);
        ui::readerStyle(status, ui::styles::font(m::text::secondary));
        rows->addWidget(status);
        for (auto *input : {command, filename, insert, renameInput, workingDirectory, findInput}) {
            input->setMinimumHeight(ui::px(80));
            ui::readerStyle(input, QStringLiteral("QLineEdit{background:palette(base);color:palette(text);border:1px "
                                                  "solid palette(mid);border-radius:16px;padding:12px;") +
                                       ui::styles::font(m::chrome::labelText) + '}');
            input->setInputMethodHints(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
        }
        for (auto *text : {terminal, editor})
            ui::readerStyle(text, QStringLiteral("QPlainTextEdit{background:palette(base);color:palette(text);border:"
                                                 "1px solid palette(mid);border-radius:16px;padding:8px;") +
                                      ui::styles::font(m::chrome::labelText) + '}');
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
        if (object == command && event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Up || key == Qt::Key_Down) {
                historyIndex = qBound(1, historyIndex + (key == Qt::Key_Up ? -1 : 1), history->count());
                command->setText(historyIndex < history->count() ? history->itemData(historyIndex).toString()
                                                                 : QString());
                return true;
            }
        }
        if (object == parentWidget()) {
            if (event->type() == QEvent::Resize) {
                setGeometry(parentWidget()->rect());
                if (inputDialog) {
                    inputDialog->setGeometry(rect());
                    inputDialog->setCardArea(QRect(0, 0, width(), height() * 2 / 3));
                    inputDialog->fitCard();
                }
                if (keyboard)
                    keyboard->place();
            }
            if (event->type() == QEvent::Hide) {
                stopShell();
                if (keyboard)
                    keyboard->hide();
            }
        }
        return QWidget::eventFilter(object, event);
    }
};
} // namespace
QWidget *openTools(QWidget *host, KeyboardFactory keyboard, const QString &directory) {
    if (auto *existing = host->findChild<QWidget *>("rrTools", Qt::FindDirectChildrenOnly)) {
        existing->show();
        existing->raise();
        return existing;
    }
    const auto chosenDirectory =
        directory.isEmpty() ? host->property("roreader_tools_directory").toString() : directory;
    auto *tools = new Tools(host, std::move(keyboard), chosenDirectory);
    tools->show();
    tools->raise();
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
    if (!widget || !widget->isVisible())
        return false;
    static_cast<Tools *>(widget)->dismiss();
    return true;
}
} // namespace kobalt::reader
