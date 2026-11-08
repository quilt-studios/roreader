#include "support/localization.h"
#include "games_panel.h"
#include "game_models.h"
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/text.h"
#include "interfaces/reader/ui/style/styles.h"
#include <QButtonGroup>
#include <QGridLayout>
#include <QLabel>
#include <QRandomGenerator>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace kobalt::reader {
namespace {
namespace m = ui::metrics;
QPushButton *action(QWidget *parent, QBoxLayout *row, const char *id, const QString &text, std::function<void()> run) {
    auto *button = ui::makeButton(parent, row, id, text, std::move(run));
    button->setMinimumHeight(ui::px(80));
    button->setProperty("segment_first", true);
    ui::readerStyle(
        button,
        QString("QPushButton{font-size:%1px;border:1px solid "
                "palette(mid);border-radius:16px;padding:8px;background:palette(window);color:palette(window-text);} "
                "QPushButton:checked,QPushButton:pressed{background:palette(window-text);color:palette(window);}")
            .arg(int(m::chrome::labelText)));
    return button;
}
class Games final : public QWidget {
    QStackedWidget *pages;
    games::TicTacToe tic;
    games::Sudoku sudoku;
    QVector<QPushButton *> ticButtons, sudokuButtons, memoryButtons;
    QLabel *ticStatus, *sudokuStatus, *memoryStatus;
    bool computer = true;
    int selected = -1, first = -1, second = -1, turns = 0;
    std::array<int, 16> cards{};
    std::array<bool, 16> matched{};
    QTimer flip;
    void updateTic() {
        for (int i = 0; i < 9; ++i) {
            ticButtons[i]->setText(tic.cells[i] == 1 ? "X" : tic.cells[i] == 2 ? "O" : "");
            ticButtons[i]->setEnabled(!tic.cells[i] && !tic.winner() && !tic.full());
        }
        ticStatus->setText(tic.winner() ? i18n::text(QStringLiteral("%1 gewinnt!")).arg(tic.winner() == 1 ? "X" : "O")
                           : tic.full() ? i18n::text(QStringLiteral("Unentschieden"))
                                        : i18n::text(QStringLiteral("%1 ist am Zug")).arg(tic.turn == 1 ? "X" : "O"));
    }
    void updateSudoku() {
        for (int i = 0; i < 81; ++i) {
            auto *cell = sudokuButtons[i];
            cell->setText(sudoku.cells[i] ? QString::number(sudoku.cells[i]) : QString());
            cell->setAccessibleName(i18n::text(QStringLiteral("Zeile %1, Spalte %2, %3%4"))
                                        .arg(i / 9 + 1)
                                        .arg(i % 9 + 1)
                                        .arg(sudoku.cells[i])
                                        .arg(sudoku.givens[i] ? i18n::text(", Vorgabe") : ""));
            const QString background = i == selected || sudoku.givens[i] ? "midlight" : "window";
            ui::readerStyle(cell,
                            QString("QPushButton{font-size:%1px;min-height:0;padding:0;border-radius:8px;border:%2px "
                                    "solid palette(%3);background:palette(%4);color:palette(%5);font-weight:%6;}")
                                .arg(qMin(ui::px(36), int(m::chrome::labelText)))
                                .arg(sudoku.conflict(i) || i == selected ? 3 : 1)
                                .arg(sudoku.conflict(i) || i == selected ? "window-text" : "mid")
                                .arg(background)
                                .arg("window-text")
                                .arg(sudoku.givens[i] ? "bold" : "normal"));
        }
        if (sudoku.solved())
            sudokuStatus->setText(i18n::text(QStringLiteral("Sudoku gelöst!")));
    }
    void memoryNew() {
        flip.stop();
        first = second = -1;
        turns = 0;
        matched.fill(false);
        for (int i = 0; i < 16; ++i)
            cards[i] = i / 2;
        for (int i = 15; i > 0; --i)
            std::swap(cards[i], cards[QRandomGenerator::global()->bounded(i + 1)]);
        updateMemory();
    }
    void updateMemory() {
        const auto found = std::count(matched.begin(), matched.end(), true) / 2;
        memoryStatus->setText(found == 8 ? i18n::text(QStringLiteral("Alle Paare gefunden · %1 Züge")).arg(turns)
                                         : i18n::text(QStringLiteral("%1 / 8 Paare · %2 Züge")).arg(found).arg(turns));
        for (int i = 0; i < 16; ++i) {
            memoryButtons[i]->setText(matched[i] || i == first || i == second ? QString(QChar('A' + cards[i])) : "?");
            memoryButtons[i]->setEnabled(!matched[i] && i != first && i != second && second < 0);
        }
    }

  public:
    explicit Games(QWidget *parent) : QWidget(parent) {
        setObjectName("rrGames");
        auto *rows = new QVBoxLayout(this);
        rows->setContentsMargins(0, 0, 0, 0);
        auto *tabs = new QHBoxLayout;
        rows->addLayout(tabs);
        pages = new QStackedWidget(this);
        auto *group = new QButtonGroup(this);
        const QStringList names{i18n::text("Tic-Tac-Toe"), i18n::text("Sudoku"), i18n::text("Memory")};
        const char *ids[] = {"rrGameTic", "rrGameSudoku", "rrGameMemory"};
        for (int i = 0; i < names.size(); ++i) {
            auto *tab = action(this, tabs, ids[i], names[i], [this, i] { pages->setCurrentIndex(i); });
            tab->setCheckable(true);
            group->addButton(tab);
            tab->setChecked(i == 0);
        }
        rows->addWidget(pages, 1);
        auto *ticPage = new QWidget(pages);
        pages->addWidget(ticPage);
        auto *ticRows = new QVBoxLayout(ticPage);
        ticStatus = new ui::InterfaceLabel(ticPage);
        ticStatus->setObjectName("rrTicStatus");
        ticRows->addWidget(ticStatus);
        auto *ticGrid = new QGridLayout;
        ticRows->addLayout(ticGrid, 1);
        for (int i = 0; i < 9; ++i) {
            auto *cell = ui::makeButton(ticPage, nullptr, qPrintable(QString("rrTicCell%1").arg(i)), {}, [this, i] {
                if (!tic.play(i))
                    return;
                if (computer && tic.turn == 2 && !tic.winner() && !tic.full())
                    tic.play(tic.suggested());
                updateTic();
            });
            cell->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            ui::readerStyle(cell, "QPushButton{font-size:64px;border-radius:20px;border:2px solid "
                                  "palette(mid);padding:0;background:palette(midlight);}");
            ticGrid->addWidget(cell, i / 3, i % 3);
            ticButtons.append(cell);
        }
        auto *ticActions = new QHBoxLayout;
        ticRows->addLayout(ticActions);
        action(ticPage, ticActions, "rrTicNew", i18n::text("Neu"), [this] {
            tic = {};
            updateTic();
        });
        auto *mode = action(ticPage, ticActions, "rrTicMode", i18n::text("Gegen Reader"), [] {});
        connect(mode, &QPushButton::clicked, this, [this, mode] {
            computer = !computer;
            mode->setText(computer ? i18n::text("Gegen Reader") : i18n::text("Zu zweit"));
            tic = {};
            updateTic();
        });
        updateTic();
        auto *sudokuPage = new QWidget(pages);
        pages->addWidget(sudokuPage);
        auto *sudokuRows = new QVBoxLayout(sudokuPage);
        sudokuStatus =
            new ui::InterfaceLabel(i18n::text(QStringLiteral("Feld wählen, dann Zahl antippen")), sudokuPage);
        sudokuStatus->setObjectName("rrSudokuStatus");
        sudokuRows->addWidget(sudokuStatus);
        auto *grid = new QGridLayout;
        grid->setSpacing(ui::px(3));
        sudokuRows->addLayout(grid, 1);
        for (int i = 0; i < 81; ++i) {
            auto *cell =
                ui::makeButton(sudokuPage, nullptr, qPrintable(QString("rrSudokuCell%1").arg(i)), {}, [this, i] {
                    selected = i;
                    updateSudoku();
                });
            cell->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            grid->addWidget(cell, i / 9 + i / 27, i % 9 + (i % 9) / 3);
            sudokuButtons.append(cell);
        }
        for (int index : {3, 7}) {
            grid->setRowMinimumHeight(index, ui::px(9));
            grid->setColumnMinimumWidth(index, ui::px(9));
        }
        auto *digits = new QHBoxLayout;
        sudokuRows->addLayout(digits);
        for (int value = 1; value <= 9; ++value) {
            auto *digit = action(sudokuPage, digits, qPrintable(QString("rrSudokuDigit%1").arg(value)),
                                 QString::number(value), [this, value] {
                                     if (sudoku.set(selected, value)) {
                                         sudokuStatus->setText(sudoku.conflict(selected)
                                                                   ? i18n::text("Konflikt in Zeile, Spalte oder Block")
                                                                   : i18n::text("Weiter so!"));
                                         updateSudoku();
                                     }
                                 });
            ui::readerStyle(digit, QString("QPushButton{min-width:0;min-height:%1px;padding:0;font-size:%2px;}")
                                       .arg(int(m::chrome::actionHeight))
                                       .arg(int(m::chrome::labelText)));
        }
        auto *sudokuActions = new QHBoxLayout;
        sudokuRows->addLayout(sudokuActions);
        action(sudokuPage, sudokuActions, "rrSudokuNew", i18n::text("Neu"), [this] {
            sudoku = games::Sudoku(QRandomGenerator::global()->bounded(18));
            selected = -1;
            sudokuStatus->setText(i18n::text("Neues Sudoku"));
            updateSudoku();
        });
        action(sudokuPage, sudokuActions, "rrSudokuErase", i18n::text("Löschen"), [this] {
            sudoku.set(selected, 0);
            updateSudoku();
        });
        action(sudokuPage, sudokuActions, "rrSudokuHint", i18n::text("Tipp"), [this] {
            selected = sudoku.hint(selected);
            sudokuStatus->setText(i18n::text("Ein Feld wurde ergänzt"));
            updateSudoku();
        });
        action(sudokuPage, sudokuActions, "rrSudokuCheck", i18n::text("Prüfen"), [this] {
            int conflicts = 0;
            for (int i = 0; i < 81; ++i)
                conflicts += sudoku.conflict(i);
            sudokuStatus->setText(conflicts ? QString(i18n::text("%1 Felder mit Konflikten")).arg(conflicts)
                                            : i18n::text("Keine Konflikte gefunden"));
            updateSudoku();
        });
        updateSudoku();
        auto *memoryPage = new QWidget(pages);
        pages->addWidget(memoryPage);
        auto *memoryRows = new QVBoxLayout(memoryPage);
        memoryStatus = new ui::InterfaceLabel(memoryPage);
        memoryStatus->setObjectName("rrMemoryStatus");
        memoryRows->addWidget(memoryStatus);
        auto *memoryGrid = new QGridLayout;
        memoryRows->addLayout(memoryGrid, 1);
        flip.setSingleShot(true);
        flip.setInterval(900);
        connect(&flip, &QTimer::timeout, this, [this] {
            first = second = -1;
            updateMemory();
        });
        for (int i = 0; i < 16; ++i) {
            auto *cell =
                ui::makeButton(memoryPage, nullptr, qPrintable(QString("rrMemoryCell%1").arg(i)), "?", [this, i] {
                    if (matched[i] || i == first || second >= 0)
                        return;
                    if (first < 0)
                        first = i;
                    else {
                        second = i;
                        ++turns;
                        if (cards[first] == cards[second]) {
                            matched[first] = matched[second] = true;
                            first = second = -1;
                        } else
                            flip.start();
                    }
                    updateMemory();
                });
            cell->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            ui::readerStyle(cell, "QPushButton{font-size:48px;border-radius:20px;border:2px solid "
                                  "palette(mid);padding:0;background:palette(midlight);}");
            memoryGrid->addWidget(cell, i / 4, i % 4);
            memoryButtons.append(cell);
        }
        action(memoryPage, memoryRows, "rrMemoryNew", i18n::text("Neues Spiel"), [this] { memoryNew(); });
        memoryNew();
    }
};
} // namespace
QWidget *createGamesPanel(QWidget *parent) {
    return new Games(parent);
}
} // namespace kobalt::reader
