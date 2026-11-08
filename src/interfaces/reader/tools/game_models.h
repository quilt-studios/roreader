#pragma once
#include <array>
#include <algorithm>

namespace kobalt::reader::games {
struct TicTacToe {
    std::array<int, 9> cells{};
    int turn = 1;
    int winner() const {
        constexpr int lines[8][3] = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}, {0, 3, 6},
                                     {1, 4, 7}, {2, 5, 8}, {0, 4, 8}, {2, 4, 6}};
        for (const auto &line : lines)
            if (cells[line[0]] && cells[line[0]] == cells[line[1]] && cells[line[1]] == cells[line[2]])
                return cells[line[0]];
        return 0;
    }
    bool full() const {
        return std::none_of(cells.begin(), cells.end(), [](int cell) { return !cell; });
    }
    bool play(int cell) {
        if (cell < 0 || cell >= 9 || cells[cell] || winner() || full())
            return false;
        cells[cell] = turn;
        turn = 3 - turn;
        return true;
    }
    int suggested() const {
        for (int player : {turn, 3 - turn})
            for (int cell = 0; cell < 9; ++cell) {
                if (cells[cell])
                    continue;
                auto next = *this;
                next.cells[cell] = player;
                if (next.winner() == player)
                    return cell;
            }
        for (int cell : {4, 0, 2, 6, 8, 1, 3, 5, 7})
            if (!cells[cell])
                return cell;
        return -1;
    }
};
struct Sudoku {
    std::array<int, 81> cells{}, givens{}, solution{};
    explicit Sudoku(unsigned variation = 0) {
        const char *puzzle = "530070000600195000098000060800060003400803001700020006060000280000419005000080079";
        const char *answer = "534678912672195348198342567859761423426853791713924856961537284287419635345286179";
        for (int i = 0; i < 81; ++i) {
            const int at = variation & 1 ? 80 - i : i;
            const auto map = [variation](int value) {
                return value ? ((value - 1 + int((variation / 2) % 9)) % 9) + 1 : 0;
            };
            givens[i] = cells[i] = map(puzzle[at] - '0');
            solution[i] = map(answer[at] - '0');
        }
    }
    bool set(int cell, int value) {
        if (cell < 0 || cell >= 81 || value < 0 || value > 9 || givens[cell])
            return false;
        cells[cell] = value;
        return true;
    }
    bool conflict(int cell) const {
        if (cell < 0 || cell >= 81 || !cells[cell])
            return false;
        const int row = cell / 9, column = cell % 9;
        for (int i = 0; i < 81; ++i)
            if (i != cell && cells[i] == cells[cell] &&
                (i / 9 == row || i % 9 == column || (i / 27 == row / 3 && (i % 9) / 3 == column / 3)))
                return true;
        return false;
    }
    bool solved() const {
        return cells == solution;
    }
    int hint(int preferred = -1) {
        if (preferred < 0 || preferred >= 81 || givens[preferred] || cells[preferred] == solution[preferred]) {
            preferred = -1;
            for (int i = 0; i < 81; ++i)
                if (cells[i] != solution[i]) {
                    preferred = i;
                    break;
                }
        }
        if (preferred >= 0)
            cells[preferred] = solution[preferred];
        return preferred;
    }
};
} // namespace kobalt::reader::games
