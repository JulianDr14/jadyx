// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// On-screen keyboard for short texts, driven by a gamepad, a mouse or a real keyboard: the user
// name in the library and every text a game asks for (uwp_software_keyboard.h). The frontend
// draws on its own swapchain without XAML, so there is no text box for the system keyboard to
// attach to; this keeps text entry identical on the Series and the PC. The model knows nothing of
// where it is drawn (text_entry_panel.h) or which button does what (Command).
//
// Five rows of ten columns. The first four hold characters; the last holds five wide action keys
// (Shift, symbols, Space, Delete, Done), two columns each, so moving up and down keeps the column.
//
// A numeric pad instead, for addresses and ports: four rows of four big keys,
//   1 2 3 Delete / 4 5 6 . / 7 8 9 : / 0 (two wide) Done (two wide),
// and a real keyboard types only digits, '.' and ':' into it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Jadyx {

class TextEntry {
public:
    static constexpr std::size_t Columns = 10;
    static constexpr std::size_t CharacterRows = 4;
    static constexpr std::size_t Rows = CharacterRows + 1;
    /// The numeric pad's grid.
    static constexpr std::size_t NumericColumns = 4;
    static constexpr std::size_t NumericRows = 4;
    enum class Action : std::uint8_t { Shift, Symbols, Space, Delete, Done };
    static constexpr std::size_t ActionCount = 5;
    enum class Result : std::uint8_t { None, Done, Cancel };
    /// What every input source maps to, so a pad, a real keyboard and a pointer act alike.
    enum class Command : std::uint8_t { Up, Down, Left, Right, Press, Delete, Space, Shift, Done, Cancel };

    TextEntry() = default;
    TextEntry(std::wstring initial, std::size_t max_length_, bool numeric_ = false)
        : value{std::move(initial)}, max_length{max_length_}, numeric{numeric_} {
        if (value.size() > max_length) value.resize(max_length);
        shift = !numeric && value.empty(); // A new name starts with a capital.
        if (numeric) row = 0;
    }

    void Move(int dx, int dy) {
        if (numeric) {
            if (dy != 0) {
                row = (row + NumericRows + (dy > 0 ? 1 : NumericRows - 1)) % NumericRows;
                if (row == NumericRows - 1) column = column / 2 * 2;
            }
            if (dx != 0) {
                const std::size_t step = row == NumericRows - 1 ? 2 : 1;
                column = (column + NumericColumns + (dx > 0 ? step : NumericColumns - step)) % NumericColumns;
            }
            return;
        }
        if (dy != 0) {
            row = (row + Rows + (dy > 0 ? 1 : Rows - 1)) % Rows;
            // Entering the action row lands on the key under the current column.
            if (row == CharacterRows) column = column / 2 * 2;
        }
        if (dx != 0) {
            const std::size_t step = row == CharacterRows ? 2 : 1;
            column = (column + Columns + (dx > 0 ? step : Columns - step)) % Columns;
        }
    }
    void Focus(std::size_t row_, std::size_t column_) {
        if (numeric) {
            row = row_ % NumericRows;
            column = row == NumericRows - 1 ? column_ % NumericColumns / 2 * 2 : column_ % NumericColumns;
            return;
        }
        row = row_ % Rows;
        column = row == CharacterRows ? column_ % Columns / 2 * 2 : column_ % Columns;
    }

    /// The highlighted key, as A on a pad or Enter would press it.
    Result Press() {
        if (numeric) {
            switch (NumericKey(row, column)) {
            case L'\b': Backspace(); return Result::None;
            case L'\n': return Result::Done;
            default: Type(NumericKey(row, column)); return Result::None;
            }
        }
        if (row < CharacterRows) {
            Type(Character(row, column));
            return Result::None;
        }
        switch (ActionAt(column)) {
        case Action::Shift:   shift = !shift; break;
        case Action::Symbols: symbols = !symbols; break;
        case Action::Space:   Type(L' '); break;
        case Action::Delete:  Backspace(); break;
        case Action::Done:    return Result::Done;
        }
        return Result::None;
    }

    Result Apply(Command command) {
        switch (command) {
        case Command::Up:     Move(0, -1); break;
        case Command::Down:   Move(0, 1); break;
        case Command::Left:   Move(-1, 0); break;
        case Command::Right:  Move(1, 0); break;
        case Command::Press:  return Press();
        case Command::Delete: Backspace(); break;
        case Command::Space:  Type(L' '); break;
        case Command::Shift:  ToggleShift(); break;
        case Command::Done:   return Result::Done;
        case Command::Cancel: return Result::Cancel;
        }
        return Result::None;
    }

    /// Replaces the text (a game changed it), keeping the length limit.
    void SetValue(std::wstring text) {
        value = std::move(text);
        if (value.size() <= max_length) return;
        value.resize(max_length);
        if (!value.empty() && value.back() >= 0xD800 && value.back() <= 0xDBFF) value.pop_back();
    }

    /// Typed on a real keyboard or produced by a key: printable characters only.
    void Type(wchar_t c) {
        if (numeric && !((c >= L'0' && c <= L'9') || c == L'.' || c == L':')) return;
        if (c < 0x20 || c == 0x7F || value.size() >= max_length) return;
        if (c == L' ' && (value.empty() || value.back() == L' ')) return;
        value += c;
        // Like phone keyboards: Shift applies to one letter, then turns itself off.
        if (shift && !symbols) shift = false;
    }
    void Backspace() {
        if (value.empty()) return;
        value.pop_back();
        if (!value.empty() && value.back() >= 0xD800 && value.back() <= 0xDBFF) value.pop_back();
    }
    void ToggleShift() {
        if (!numeric) shift = !shift;
    }

    /// The numeric pad's key: a character, '\b' for Delete or '\n' for Done.
    [[nodiscard]] static constexpr wchar_t NumericKey(std::size_t r, std::size_t c) {
        constexpr std::wstring_view keys[NumericRows]{L"123\b", L"456.", L"789:", L"00\n\n"};
        return r < NumericRows && c < NumericColumns ? keys[r][c] : L'\0';
    }

    [[nodiscard]] wchar_t Character(std::size_t r, std::size_t c) const {
        static constexpr std::array<std::wstring_view, CharacterRows> letters{
            L"1234567890", L"qwertyuiop", L"asdfghjklñ", L"zxcvbnm-_."};
        // Accents for Spanish and Portuguese (ã õ â ê ô) first, then the usual punctuation.
        static constexpr std::array<std::wstring_view, CharacterRows> symbol_rows{
            L"áàâãéêíóôõ", L"úüçèò!?¡¿@", L"#$%&*()/\\'", L"\":;,+=<>[]"};
        if (r >= CharacterRows || c >= Columns) return 0;
        const wchar_t ch = (symbols ? symbol_rows : letters)[r][c];
        if (!shift) return ch;
        if (ch >= L'a' && ch <= L'z') return static_cast<wchar_t>(ch - L'a' + L'A');
        switch (ch) {
        case L'ñ': return L'Ñ';
        case L'á': return L'Á';
        case L'à': return L'À';
        case L'â': return L'Â';
        case L'ã': return L'Ã';
        case L'é': return L'É';
        case L'ê': return L'Ê';
        case L'í': return L'Í';
        case L'ó': return L'Ó';
        case L'ô': return L'Ô';
        case L'õ': return L'Õ';
        case L'ú': return L'Ú';
        case L'ü': return L'Ü';
        case L'ç': return L'Ç';
        case L'è': return L'È';
        case L'ò': return L'Ò';
        default:   return ch;
        }
    }
    static constexpr Action ActionAt(std::size_t c) {
        return static_cast<Action>(c / 2 % ActionCount);
    }

    [[nodiscard]] const std::wstring& Value() const { return value; }
    [[nodiscard]] std::size_t MaxLength() const { return max_length; }
    [[nodiscard]] std::size_t Row() const { return row; }
    [[nodiscard]] std::size_t Column() const { return column; }
    [[nodiscard]] bool Shifted() const { return shift; }
    [[nodiscard]] bool ShowingSymbols() const { return symbols; }
    [[nodiscard]] bool Numeric() const { return numeric; }

private:
    std::wstring value;
    std::size_t max_length{10};
    std::size_t row{1}, column{0};
    bool shift{}, symbols{};
    bool numeric{};
};

} // namespace Jadyx
