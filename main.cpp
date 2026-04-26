#include <ncurses.h>
#include <locale.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <locale>
#include <cwctype>
#include <cwchar>
#include <tuple>
#include <deque>
#include <cstdlib>
#include <array>
#include <limits>

namespace fs = std::filesystem;

constexpr int lineDigitsWidth = 4;
constexpr int editorLeft = 9;
constexpr int statusTextLeft = 16;
constexpr int liveInputDelayMs = 120;
constexpr int altKeyDelayMs = 30;
constexpr size_t maxUndo = 100;

std::vector<std::wstring> lines{L""};
std::wstring clipboard;
std::wstring searchQuery;
std::string filename;
std::wstring statusMessage = L"ready";

int cx = 0, cy = 0, rowoff = 0, coloff = 0;
int tabWidth = 4;
bool dirty = false;
bool showRat = true;
bool mouseEnabled = true;
bool selecting = false;
int selAnchorX = 0, selAnchorY = 0;
bool cursesStarted = false;

fs::file_time_type lastWrite;

struct EditorState {
    std::vector<std::wstring> savedLines;
    int savedCx = 0;
    int savedCy = 0;
    int savedRowoff = 0;
    int savedColoff = 0;
    bool savedDirty = false;
};

std::deque<EditorState> undoStack;
std::deque<EditorState> redoStack;

enum Colors {
    C_KEYWORD = 1,
    C_STRING,
    C_COMMENT,
    C_NUMBER,
    C_STATUS,
    C_LINE
};

const std::unordered_set<std::wstring> cppKeywords = {
    L"int", L"float", L"double", L"char", L"bool", L"void",
    L"auto", L"const", L"static", L"class", L"struct", L"public",
    L"private", L"protected", L"namespace", L"using", L"return",
    L"if", L"else", L"for", L"while", L"do", L"switch", L"case",
    L"break", L"continue", L"include", L"define", L"std", L"string",
    L"vector", L"true", L"false", L"nullptr", L"new", L"delete"
};

const std::unordered_set<std::wstring> pyKeywords = {
    L"def", L"class", L"import", L"from", L"as", L"return",
    L"if", L"elif", L"else", L"for", L"while", L"break",
    L"continue", L"in", L"is", L"not", L"and", L"or",
    L"True", L"False", L"None", L"try", L"except", L"finally",
    L"with", L"lambda", L"pass", L"yield", L"global", L"nonlocal"
};

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isPython() {
    return endsWith(filename, ".py");
}

bool isCpp() {
    return endsWith(filename, ".cpp") ||
           endsWith(filename, ".hpp") ||
           endsWith(filename, ".cc") ||
           endsWith(filename, ".cxx") ||
           endsWith(filename, ".h");
}

std::wstring toWide(const std::string& s) {
    std::mbstate_t state{};
    const char* source = s.c_str();
    size_t length = std::mbsrtowcs(nullptr, &source, 0, &state);

    if (length == (size_t)-1)
        return L"";

    std::wstring result(length, L'\0');
    state = std::mbstate_t{};
    source = s.c_str();
    std::mbsrtowcs(result.data(), &source, result.size(), &state);

    return result;
}

std::string toUtf8(const std::wstring& s) {
    std::mbstate_t state{};
    const wchar_t* source = s.c_str();
    size_t length = std::wcsrtombs(nullptr, &source, 0, &state);

    if (length == (size_t)-1)
        return "";

    std::string result(length, '\0');
    state = std::mbstate_t{};
    source = s.c_str();
    std::wcsrtombs(result.data(), &source, result.size(), &state);

    return result;
}

void setStatus(const std::wstring& message) {
    statusMessage = message;
}

int textSize(const std::wstring& text) {
    return (int)std::min(text.size(), (size_t)std::numeric_limits<int>::max());
}

void liveInput() {
    nodelay(stdscr, true);
    timeout(liveInputDelayMs);
}

void blockingInput() {
    nodelay(stdscr, false);
    timeout(-1);
}

void applyMouseMode() {
    mmask_t mask = mouseEnabled ? (ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION) : 0;
    mousemask(mask, nullptr);
}

struct InputModeGuard {
    int cursor;
    bool liveOnExit;

    InputModeGuard(int cursorMode, bool restoreLive = true)
        : cursor(cursorMode), liveOnExit(restoreLive) {
        curs_set(cursor);
        blockingInput();
    }

    ~InputModeGuard() {
        curs_set(1);
        if (liveOnExit)
            liveInput();
    }
};

EditorState captureState() {
    return {lines, cx, cy, rowoff, coloff, dirty};
}

void restoreState(const EditorState& state) {
    lines = state.savedLines;
    cx = state.savedCx;
    cy = state.savedCy;
    rowoff = state.savedRowoff;
    coloff = state.savedColoff;
    dirty = state.savedDirty;
    selecting = false;
}

void rememberUndo() {
    undoStack.push_back(captureState());
    if (undoStack.size() > maxUndo)
        undoStack.pop_front();
    redoStack.clear();
}

void undoEdit() {
    if (undoStack.empty()) {
        setStatus(L"nothing to undo");
        return;
    }

    redoStack.push_back(captureState());
    restoreState(undoStack.back());
    undoStack.pop_back();
    setStatus(L"undo");
}

void redoEdit() {
    if (redoStack.empty()) {
        setStatus(L"nothing to redo");
        return;
    }

    undoStack.push_back(captureState());
    restoreState(redoStack.back());
    redoStack.pop_back();
    setStatus(L"redo");
}

void toggleMouse() {
    mouseEnabled = !mouseEnabled;
    applyMouseMode();
    setStatus(mouseEnabled ? L"mouse on" : L"mouse off");
}

void loadSettings() {
    const char* home = std::getenv("HOME");
    if (!home) return;

    std::ifstream in(std::string(home) + "/.ratitrc");
    std::string line;

    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        if (key == "tab_width") {
            try { tabWidth = std::clamp(std::stoi(value), 1, 8); }
            catch (...) {}
        } else if (key == "show_rat") {
            showRat = value != "0" && value != "false";
        } else if (key == "mouse") {
            mouseEnabled = value != "0" && value != "false";
        }
    }
}

void initCurses() {
    if (cursesStarted) return;

    initscr();
    set_escdelay(25);
    raw();
    noecho();
    keypad(stdscr, true);
    timeout(liveInputDelayMs);
    curs_set(1);
    mouseinterval(0);
    applyMouseMode();

    start_color();
    use_default_colors();

    init_pair(C_KEYWORD, COLOR_CYAN, -1);
    init_pair(C_STRING, COLOR_GREEN, -1);
    init_pair(C_COMMENT, COLOR_BLUE, -1);
    init_pair(C_NUMBER, COLOR_YELLOW, -1);
    init_pair(C_STATUS, COLOR_MAGENTA, -1);
    init_pair(C_LINE, COLOR_BLUE, -1);

    cursesStarted = true;
}

void loadFile() {
    lines.clear();
    std::ifstream in(filename);
    std::string line;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(toWide(line));
    }

    if (lines.empty()) lines.push_back(L"");

    cy = std::min(cy, (int)lines.size() - 1);
    cx = std::min(cx, textSize(lines[cy]));

    if (fs::exists(filename))
        lastWrite = fs::last_write_time(filename);

    dirty = false;
    rowoff = 0;
    coloff = 0;
    undoStack.clear();
    redoStack.clear();
    setStatus(L"opened " + toWide(filename));
}

void saveFile() {
    std::ofstream out(filename);

    for (size_t i = 0; i < lines.size(); ++i) {
        out << toUtf8(lines[i]);
        if (i + 1 < lines.size()) out << '\n';
    }

    out.close();

    if (fs::exists(filename))
        lastWrite = fs::last_write_time(filename);

    dirty = false;
    undoStack.clear();
    redoStack.clear();
    setStatus(L"saved " + toWide(filename));
}

void drawBox(int y, int x, int h, int w) {
    if (h < 2 || w < 2) return;

    mvaddwstr(y, x, L"╭");
    for (int i = 1; i < w - 1; ++i) mvaddwstr(y, x + i, L"─");
    mvaddwstr(y, x + w - 1, L"╮");

    for (int i = 1; i < h - 1; ++i) {
        mvaddwstr(y + i, x, L"│");
        mvaddwstr(y + i, x + w - 1, L"│");
    }

    mvaddwstr(y + h - 1, x, L"╰");
    for (int i = 1; i < w - 1; ++i) mvaddwstr(y + h - 1, x + i, L"─");
    mvaddwstr(y + h - 1, x + w - 1, L"╯");
}

std::wstring prompt(const std::wstring& label) {
    int h, w;
    getmaxyx(stdscr, h, w);

    std::wstring input;

    InputModeGuard inputGuard(1);

    while (true) {
        attron(COLOR_PAIR(C_STATUS));
        mvprintw(h - 1, 0, "%*s", w, "");
        mvaddwstr(h - 1, 1, label.c_str());
        mvaddwstr(h - 1, (int)label.size() + 2, input.c_str());
        attroff(COLOR_PAIR(C_STATUS));

        move(h - 1, (int)label.size() + 2 + (int)input.size());
        refresh();

        wint_t ch;
        int result = get_wch(&ch);

        if (result == ERR) continue;

        if (result == KEY_CODE_YES) {
            if (ch == KEY_BACKSPACE && !input.empty()) {
                input.pop_back();
            } else if (ch == KEY_DC && !input.empty()) {
                input.pop_back();
            }
            continue;
        }

        if (ch == 27 || ch == 3) {
            return L"";
        }

        if (ch == L'\n' || ch == L'\r') {
            return input;
        }

        if ((ch == 127 || ch == 8) && !input.empty()) {
            input.pop_back();
        } else if (ch >= 32) {
            input.push_back((wchar_t)ch);
        }
    }
}

void drawCenteredText(int y, const std::wstring& text) {
    int h, w;
    getmaxyx(stdscr, h, w);
    (void)h;

    int x = std::max(0, (w - (int)text.size()) / 2);
    mvaddwstr(y, x, text.c_str());
}

std::vector<std::string> listFiles(const fs::path& dir) {
    std::vector<std::string> items;

    items.push_back("../");

    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            std::string name = entry.path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            if (entry.is_directory()) name += "/";
            items.push_back(name);
        }
    } catch (...) {}

    std::sort(items.begin() + 1, items.end(), [](const std::string& a, const std::string& b) {
        bool ad = !a.empty() && a.back() == '/';
        bool bd = !b.empty() && b.back() == '/';
        if (ad != bd) return ad > bd;
        return a < b;
    });

    return items;
}

std::string filePicker() {
    fs::path current = fs::current_path();
    int selected = 0;
    int offset = 0;

    InputModeGuard inputGuard(0);

    while (true) {
        auto items = listFiles(current);

        if (selected >= (int)items.size()) selected = (int)items.size() - 1;
        if (selected < 0) selected = 0;

        erase();

        int h, w;
        getmaxyx(stdscr, h, w);
        drawBox(0, 0, h, w);

        attron(A_BOLD);
        mvaddwstr(1, 3, L" Open file ");
        attroff(A_BOLD);

        std::wstring pathText = toWide(current.string());
        mvaddnwstr(3, 3, pathText.c_str(), w - 6);

        int listY = 5;
        int listH = std::max(1, h - 8);

        if (selected < offset) offset = selected;
        if (selected >= offset + listH) offset = selected - listH + 1;

        for (int i = 0; i < listH && offset + i < (int)items.size(); ++i) {
            int itemIndex = offset + i;
            std::wstring label = toWide(items[itemIndex]);

            if (itemIndex == selected) attron(A_REVERSE);
            mvaddnwstr(listY + i, 3, label.c_str(), w - 6);
            if (itemIndex == selected) attroff(A_REVERSE);
        }

        attron(COLOR_PAIR(C_STATUS));
        mvaddwstr(h - 2, 3, L"Enter open | n new path | Esc back");
        attroff(COLOR_PAIR(C_STATUS));

        refresh();

        wint_t ch;
        int result = get_wch(&ch);
        if (result == ERR) continue;

        if (result == KEY_CODE_YES) {
            if (ch == KEY_MOUSE) {
                MEVENT event;
                if (getmouse(&event) == OK &&
                    (event.bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON1_PRESSED))) {
                    int clicked = offset + event.y - listY;
                    if (clicked >= 0 && clicked < (int)items.size())
                        selected = clicked;

                    if (event.bstate & BUTTON1_DOUBLE_CLICKED) {
                        fs::path picked = current / items[selected];
                        if (items[selected] == "../") {
                            current = current.parent_path();
                            selected = 0;
                            offset = 0;
                        } else if (!items[selected].empty() && items[selected].back() == '/') {
                            current = picked;
                            selected = 0;
                            offset = 0;
                        } else {
                            return picked.string();
                        }
                    } else {
                        continue;
                    }
                }
            }

            if (ch == KEY_UP) selected--;
            else if (ch == KEY_DOWN) selected++;
            else if (ch == KEY_NPAGE) selected += listH;
            else if (ch == KEY_PPAGE) selected -= listH;
            selected = std::clamp(selected, 0, (int)items.size() - 1);
            continue;
        }

        if (ch == 27) {
            return "";
        }

        if (ch == L'n' || ch == L'N') {
            std::wstring input = prompt(L"New/open path: ");
            if (!input.empty()) {
                return toUtf8(input);
            }

            curs_set(0);
            blockingInput();
            continue;
        }

        if (ch == L'\n' || ch == L'\r') {
            fs::path picked = current / items[selected];
            if (items[selected] == "../") {
                current = current.parent_path();
                selected = 0;
                offset = 0;
            } else if (!items[selected].empty() && items[selected].back() == '/') {
                current = picked;
                selected = 0;
                offset = 0;
            } else {
                return picked.string();
            }
        }
    }
}

std::string startMenu() {
    int selected = 0;
    std::vector<std::wstring> options = {L"Open file", L"Exit"};

    InputModeGuard inputGuard(0);

    while (true) {
        erase();

        int h, w;
        getmaxyx(stdscr, h, w);
        drawBox(0, 0, h, w);

        int logoY = std::max(2, h / 2 - 7);

        attron(A_BOLD);
        drawCenteredText(logoY, L"rat.it");
        attroff(A_BOLD);

        drawCenteredText(logoY + 2, L"  (\\_/)");
        drawCenteredText(logoY + 3, L"  (o.o)");
        drawCenteredText(logoY + 4, L"  / > edit");

        for (int i = 0; i < (int)options.size(); ++i) {
            if (i == selected) attron(A_REVERSE);
            drawCenteredText(logoY + 7 + i * 2, options[i]);
            if (i == selected) attroff(A_REVERSE);
        }

        attron(COLOR_PAIR(C_STATUS));
        drawCenteredText(h - 2, L"Enter select | Up/Down move | q quit");
        attroff(COLOR_PAIR(C_STATUS));

        refresh();

        wint_t ch;
        int result = get_wch(&ch);
        if (result == ERR) continue;

        if (result == KEY_CODE_YES) {
            if (ch == KEY_MOUSE) {
                MEVENT event;
                if (getmouse(&event) == OK &&
                    (event.bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON1_PRESSED))) {
                    for (int i = 0; i < (int)options.size(); ++i) {
                        if (event.y == logoY + 7 + i * 2) {
                            selected = i;
                            if (event.bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED))
                                ch = L'\n';
                            break;
                        }
                    }
                }
            }

            if (ch == KEY_UP)
                selected = (selected + (int)options.size() - 1) % (int)options.size();
            else if (ch == KEY_DOWN)
                selected = (selected + 1) % (int)options.size();

            if (ch != L'\n' && ch != L'\r')
                continue;
        }

        if (ch == L'q' || ch == L'Q' || ch == 27) {
            return "";
        }

        if (ch == L'\n' || ch == L'\r') {
            if (selected == 1) {
                return "";
            }

            std::string picked = filePicker();
            if (!picked.empty()) {
                return picked;
            }

            curs_set(0);
            blockingInput();
        }
    }
}

bool findNext() {
    if (searchQuery.empty()) {
        setStatus(L"empty search");
        return false;
    }

    int startLine = cy;
    int startCol = cx + 1;

    for (int pass = 0; pass < 2; ++pass) {
        for (int y = pass == 0 ? startLine : 0; y < (int)lines.size(); ++y) {
            size_t from = 0;
            if (pass == 0 && y == startLine) from = startCol;

            size_t pos = lines[y].find(searchQuery, from);
            if (pos != std::wstring::npos) {
                cy = y;
                cx = (int)pos;
                setStatus(L"found");
                return true;
            }
        }
    }

    setStatus(L"not found");
    return false;
}

void gotoLine() {
    std::wstring input = prompt(L"Go to line: ");
    if (input.empty()) return;

    try {
        int line = std::stoi(input);
        line = std::clamp(line, 1, (int)lines.size());
        cy = line - 1;
        cx = std::min(cx, textSize(lines[cy]));
        setStatus(L"jumped");
    } catch (...) {
        setStatus(L"bad line number");
    }
}

void searchPrompt() {
    searchQuery = prompt(L"Search: ");
    if (!searchQuery.empty())
        findNext();
    else
        setStatus(L"search cancelled");
}

bool isWordChar(wchar_t c) {
    return iswalnum(c) || c == L'_';
}

void drawChars(int y, int x, const std::wstring& text, int from, int count) {
    if (count <= 0) return;
    mvaddnwstr(y, x, text.c_str() + from, count);
}

void clearSelection() {
    selecting = false;
}

void toggleSelection() {
    if (selecting) {
        clearSelection();
        return;
    }

    selecting = true;
    selAnchorX = cx;
    selAnchorY = cy;
}

void normalizedSelection(int& startY, int& startX, int& endY, int& endX) {
    startY = selAnchorY;
    startX = selAnchorX;
    endY = cy;
    endX = cx;

    if (std::tie(startY, startX) > std::tie(endY, endX)) {
        std::swap(startY, endY);
        std::swap(startX, endX);
    }
}

bool hasSelection() {
    return selecting && (selAnchorY != cy || selAnchorX != cx);
}

void selectionBounds(int& startY, int& startX, int& endY, int& endX) {
    normalizedSelection(startY, startX, endY, endX);

    startY = std::clamp(startY, 0, (int)lines.size() - 1);
    endY = std::clamp(endY, 0, (int)lines.size() - 1);
    startX = std::clamp(startX, 0, textSize(lines[startY]));
    endX = std::clamp(endX, 0, textSize(lines[endY]));
}

bool selectedColumnsForLine(int lineIndex, int& startCol, int& endCol) {
    if (!hasSelection()) return false;

    int startY, startX, endY, endX;
    selectionBounds(startY, startX, endY, endX);

    if (lineIndex < startY || lineIndex > endY) return false;

    int lineLen = textSize(lines[lineIndex]);
    startCol = lineIndex == startY ? startX : 0;
    endCol = lineIndex == endY ? endX : lineLen;

    startCol = std::clamp(startCol, 0, lineLen);
    endCol = std::clamp(endCol, 0, lineLen);

    return startCol < endCol;
}

std::wstring selectedText() {
    if (!hasSelection()) return L"";

    int startY, startX, endY, endX;
    selectionBounds(startY, startX, endY, endX);

    std::wstring selected;
    for (int y = startY; y <= endY; ++y) {
        int lineLen = textSize(lines[y]);
        int from = y == startY ? std::clamp(startX, 0, lineLen) : 0;
        int to = y == endY ? std::clamp(endX, 0, lineLen) : lineLen;

        if (to > from)
            selected += lines[y].substr(from, to - from);

        if (y != endY)
            selected.push_back(L'\n');
    }

    return selected;
}

void deleteSelection(bool remember = true) {
    if (!hasSelection()) return;

    if (remember) rememberUndo();

    int startY, startX, endY, endX;
    selectionBounds(startY, startX, endY, endX);

    if (startY == endY) {
        lines[startY].erase(startX, endX - startX);
    } else {
        std::wstring tail = lines[endY].substr(endX);
        lines[startY].erase(startX);
        lines[startY] += tail;
        lines.erase(lines.begin() + startY + 1, lines.begin() + endY + 1);
    }

    cy = startY;
    cx = startX;
    clearSelection();
    dirty = true;
}

std::vector<std::wstring> splitLines(const std::wstring& text) {
    std::vector<std::wstring> parts{L""};

    for (wchar_t ch : text) {
        if (ch == L'\r') continue;
        if (ch == L'\n') {
            parts.push_back(L"");
        } else {
            parts.back().push_back(ch);
        }
    }

    return parts;
}

void insertText(const std::wstring& text, bool remember = true) {
    if (text.empty()) return;

    if (remember) rememberUndo();
    if (hasSelection()) deleteSelection(false);

    auto parts = splitLines(text);
    std::wstring tail = lines[cy].substr(cx);
    lines[cy].erase(cx);
    lines[cy] += parts[0];

    if (parts.size() == 1) {
        cx += textSize(parts[0]);
    } else {
        int insertAt = cy + 1;
        for (size_t i = 1; i < parts.size(); ++i)
            lines.insert(lines.begin() + insertAt++, parts[i]);

        cy += (int)parts.size() - 1;
        cx = textSize(parts.back());
        lines[cy] += tail;
    }

    dirty = true;
}

void drawSyntaxLine(int y, int x, const std::wstring& line, int horizontalOffset, int maxWidth) {
    int i = horizontalOffset;
    int visibleEnd = std::min(textSize(line), horizontalOffset + maxWidth);
    bool py = isPython();
    bool cpp = isCpp();

    auto screenX = [&](int sourceColumn) {
        return x + sourceColumn - horizontalOffset;
    };

    auto drawToken = [&](int start, int end, int colorPair = 0) {
        start = std::max(start, horizontalOffset);
        end = std::min(end, visibleEnd);
        if (end <= start) return;

        if (colorPair) attron(COLOR_PAIR(colorPair));
        drawChars(y, screenX(start), line, start, end - start);
        if (colorPair) attroff(COLOR_PAIR(colorPair));
    };

    while (i < visibleEnd) {
        if (cpp && i + 1 < textSize(line) && line[i] == L'/' && line[i + 1] == L'/') {
            drawToken(i, visibleEnd, C_COMMENT);
            return;
        }

        if (py && line[i] == L'#') {
            drawToken(i, visibleEnd, C_COMMENT);
            return;
        }

        if (line[i] == L'"' || line[i] == L'\'') {
            wchar_t quote = line[i];
            int start = i++;

            while (i < textSize(line)) {
                if (line[i] == L'\\') i += 2;
                else if (line[i] == quote) {
                    i++;
                    break;
                } else i++;
            }

            drawToken(start, i, C_STRING);
            continue;
        }

        if (iswdigit(line[i])) {
            int start = i;
            while (i < textSize(line) && (iswalnum(line[i]) || line[i] == L'.')) i++;

            drawToken(start, i, C_NUMBER);
            continue;
        }

        if (isWordChar(line[i])) {
            int start = i;
            while (i < textSize(line) && isWordChar(line[i])) i++;

            std::wstring word = line.substr(start, i - start);
            bool keyword = cppKeywords.count(word) || pyKeywords.count(word);

            drawToken(start, i, keyword ? C_KEYWORD : 0);

            continue;
        }

        drawChars(y, screenX(i), line, i, 1);
        i++;
    }
}

void drawSelectionForLine(int screenY, int screenX, int lineIndex, int maxWidth, int horizontalOffset) {
    int startCol, endCol;
    if (!selectedColumnsForLine(lineIndex, startCol, endCol)) return;

    startCol -= horizontalOffset;
    endCol -= horizontalOffset;

    if (endCol <= 0 || startCol >= maxWidth) return;
    startCol = std::max(0, startCol);
    endCol = std::min(endCol, maxWidth);

    attron(A_REVERSE);
    drawChars(screenY, screenX + startCol, lines[lineIndex], horizontalOffset + startCol, endCol - startCol);
    attroff(A_REVERSE);
}

std::vector<std::wstring> ratMood() {
    if (selecting)
        return {L"  (\\_/)", L"  (^.^)", L"  / > select"};

    if (dirty)
        return {L"  (\\_/)", L"  (o_O)", L"  / > unsaved"};

    if (!searchQuery.empty())
        return {L"  (\\_/)", L"  (?.?)", L"  / > search"};

    if (!mouseEnabled)
        return {L"  (\\_/)", L"  (-.-)", L"  / > keys"};

    return {L"  (\\_/)", L"  (o.o)", L"  / > editing"};
}

void drawRat(int h, int w) {
    if (!showRat || h < 10 || w < 35) return;

    int boxW = 20;
    int boxH = 6;
    int y = h - boxH - 1;
    int x = w - boxW - 2;

    drawBox(y, x, boxH, boxW);

    auto mood = ratMood();

    for (int i = 0; i < (int)mood.size(); ++i)
        mvaddwstr(y + 1 + i, x + 3, mood[i].c_str());

    mvaddwstr(y + boxH - 2, x + 3, L"Ctrl+R hide");
}

void draw() {
    erase();

    int h, w;
    getmaxyx(stdscr, h, w);

    int textHeight = h - 4;

    if (cy < rowoff) rowoff = cy;
    if (cy >= rowoff + textHeight)
        rowoff = cy - textHeight + 1;

    int textWidth = std::max(1, w - editorLeft - 2);
    if (cx < coloff) coloff = cx;
    if (cx >= coloff + textWidth)
        coloff = cx - textWidth + 1;

    drawBox(0, 0, h, w);

    attron(A_BOLD);
    mvaddwstr(0, 3, L" rat.it ");
    attroff(A_BOLD);

    for (int i = 0; i < textHeight; ++i) {
        int lineIndex = rowoff + i;
        if (lineIndex >= (int)lines.size()) break;

        int screenY = i + 1;

        attron(COLOR_PAIR(C_LINE));
        mvprintw(screenY, 2, "%*d │ ", lineDigitsWidth, lineIndex + 1);
        attroff(COLOR_PAIR(C_LINE));

        drawSyntaxLine(screenY, editorLeft, lines[lineIndex], coloff, textWidth);
        drawSelectionForLine(screenY, editorLeft, lineIndex, textWidth, coloff);
    }

    mvaddwstr(h - 3, 0, L"├");
    for (int x = 1; x < w - 1; ++x) mvaddwstr(h - 3, x, L"─");
    mvaddwstr(h - 3, w - 1, L"┤");

    attron(COLOR_PAIR(C_STATUS));
    mvprintw(h - 2, 2, "%s", dirty ? "● modified" : "✓ saved");
    std::wstring statusLine =
        L"Ln " + std::to_wstring(cy + 1) + L"/" + std::to_wstring(lines.size()) +
        L" Col " + std::to_wstring(cx + 1) +
        L" | " + statusMessage +
        L" | F1 help";
    mvaddnwstr(h - 2, statusTextLeft, statusLine.c_str(), std::max(0, w - statusTextLeft - 2));
    attroff(COLOR_PAIR(C_STATUS));

    drawRat(h, w);

    move(cy - rowoff + 1, cx - coloff + editorLeft);
    refresh();
}

void checkExternalUpdate() {
    if (!fs::exists(filename)) return;

    auto currentWrite = fs::last_write_time(filename);

    if (currentWrite != lastWrite && !dirty) {
        loadFile();
        setStatus(L"reloaded from disk");
    }
}

void insertChar(wchar_t ch) {
    insertText(std::wstring(1, ch));
    setStatus(L"inserted");
}

void backspace() {
    if (hasSelection()) {
        deleteSelection();
        setStatus(L"deleted selection");
        return;
    }

    if (cx == 0 && cy == 0)
        return;

    rememberUndo();

    if (cx > 0) {
        lines[cy].erase(lines[cy].begin() + cx - 1);
        cx--;
        dirty = true;
    } else if (cy > 0) {
        cx = textSize(lines[cy - 1]);
        lines[cy - 1] += lines[cy];
        lines.erase(lines.begin() + cy);
        cy--;
        dirty = true;
    }
}

void deleteForward() {
    if (hasSelection()) {
        deleteSelection();
        setStatus(L"deleted selection");
        return;
    }

    if (cx >= textSize(lines[cy]) && cy + 1 >= (int)lines.size())
        return;

    rememberUndo();

    if (cx < textSize(lines[cy])) {
        lines[cy].erase(lines[cy].begin() + cx);
    } else {
        lines[cy] += lines[cy + 1];
        lines.erase(lines.begin() + cy + 1);
    }

    dirty = true;
    setStatus(L"deleted");
}

void newLine() {
    rememberUndo();
    if (hasSelection()) deleteSelection(false);

    std::wstring indent;
    for (wchar_t ch : lines[cy]) {
        if (ch == L' ' || ch == L'\t') indent.push_back(ch);
        else break;
    }

    std::wstring right = lines[cy].substr(cx);
    lines[cy].erase(cx);
    lines.insert(lines.begin() + cy + 1, indent + right);
    cy++;
    cx = textSize(indent);
    dirty = true;
    setStatus(L"new line");
}

void copyLine() {
    if (hasSelection()) {
        clipboard = selectedText();
        clearSelection();
        setStatus(L"copied selection");
        return;
    }

    clipboard = lines[cy];
    setStatus(L"copied line");
}

void cutLine() {
    if (hasSelection()) {
        clipboard = selectedText();
        deleteSelection();
        setStatus(L"cut selection");
        return;
    }

    rememberUndo();
    clipboard = lines[cy];
    lines.erase(lines.begin() + cy);

    if (lines.empty()) lines.push_back(L"");
    if (cy >= (int)lines.size()) cy = (int)lines.size() - 1;

    cx = std::min(cx, textSize(lines[cy]));
    dirty = true;
    setStatus(L"cut line");
}

void pasteText() {
    insertText(clipboard);
    setStatus(L"pasted");
}

void insertTab() {
    insertText(std::wstring(tabWidth, L' '));
    setStatus(L"indent");
}

void saveAs() {
    std::wstring input = prompt(L"Save as: ");
    if (input.empty()) {
        setStatus(L"save-as cancelled");
        return;
    }

    filename = toUtf8(input);
    saveFile();
}

void moveLeft() {
    if (cx > 0) cx--;
    else if (cy > 0) {
        cy--;
        cx = textSize(lines[cy]);
    }
}

void moveRight() {
    if (cx < textSize(lines[cy])) cx++;
    else if (cy + 1 < (int)lines.size()) {
        cy++;
        cx = 0;
    }
}

void moveUp() {
    if (cy > 0) cy--;
    cx = std::min(cx, textSize(lines[cy]));
}

void moveDown() {
    if (cy + 1 < (int)lines.size()) cy++;
    cx = std::min(cx, textSize(lines[cy]));
}

void moveHome() {
    int firstText = 0;
    while (firstText < textSize(lines[cy]) &&
           (lines[cy][firstText] == L' ' || lines[cy][firstText] == L'\t')) {
        firstText++;
    }

    cx = cx == firstText ? 0 : firstText;
}

void moveEnd() {
    cx = textSize(lines[cy]);
}

void pageUp() {
    int h, w;
    getmaxyx(stdscr, h, w);
    (void)w;

    int step = std::max(1, h - 5);
    cy = std::max(0, cy - step);
    cx = std::min(cx, textSize(lines[cy]));
}

void pageDown() {
    int h, w;
    getmaxyx(stdscr, h, w);
    (void)w;

    int step = std::max(1, h - 5);
    cy = std::min((int)lines.size() - 1, cy + step);
    cx = std::min(cx, textSize(lines[cy]));
}

void moveToScreenPoint(int screenY, int screenX) {
    int h, w;
    getmaxyx(stdscr, h, w);
    (void)w;

    int textHeight = std::max(1, h - 4);
    if (screenY < 1 || screenY > textHeight || screenX < editorLeft)
        return;

    int targetLine = rowoff + screenY - 1;
    if (targetLine < 0 || targetLine >= (int)lines.size())
        return;

    cy = targetLine;
    cx = std::clamp(coloff + screenX - editorLeft, 0, textSize(lines[cy]));
    clearSelection();
}

void handleMouse() {
    MEVENT event;
    if (getmouse(&event) != OK) return;

    if (event.bstate & BUTTON4_PRESSED) {
        if (cy > 0) moveUp();
        return;
    }

    if (event.bstate & BUTTON5_PRESSED) {
        if (cy + 1 < (int)lines.size()) moveDown();
        return;
    }

    if (event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_DOUBLE_CLICKED)) {
        moveToScreenPoint(event.y, event.x);
        setStatus(L"cursor moved");
    }
}

void showHelp() {
    std::vector<std::wstring> help = {
        L"Navigation",
        L"  Arrows move cursor",
        L"  Home smart line start",
        L"  End line end",
        L"  PageUp/PageDown scroll",
        L"",
        L"Editing",
        L"  Ctrl+S save",
        L"  Ctrl+O save as",
        L"  Ctrl+Q quit",
        L"  Ctrl+Z undo",
        L"  Ctrl+Y redo",
        L"  Tab indent",
        L"",
        L"Selection",
        L"  Alt+S start/stop selection",
        L"  Ctrl+C copy",
        L"  Ctrl+X cut",
        L"  Ctrl+V paste",
        L"  Backspace/Delete remove selection",
        L"",
        L"Search",
        L"  Ctrl+F find",
        L"  n next match",
        L"  Ctrl+G go to line",
        L"",
        L"Other",
        L"  Ctrl+R show/hide rat",
        L"  Alt+M toggle mouse",
        L"  Esc close this help"
    };

    InputModeGuard inputGuard(0);

    int scroll = 0;

    while (true) {
        draw();

        int h, w;
        getmaxyx(stdscr, h, w);
        int boxW = std::min(62, w - 4);
        int boxH = std::min((int)help.size() + 4, h - 2);
        int y = std::max(1, (h - boxH) / 2);
        int x = std::max(1, (w - boxW) / 2);
        int bodyH = std::max(1, boxH - 4);

        scroll = std::clamp(scroll, 0, std::max(0, (int)help.size() - bodyH));

        drawBox(y, x, boxH, boxW);
        attron(A_BOLD);
        mvaddwstr(y + 1, x + 3, L"Help");
        attroff(A_BOLD);

        for (int i = 0; i < bodyH && scroll + i < (int)help.size(); ++i) {
            std::wstring line = help[scroll + i];
            if (!line.empty() && line[0] != L' ') attron(A_BOLD);
            mvaddnwstr(y + 2 + i, x + 3, line.c_str(), boxW - 6);
            if (!line.empty() && line[0] != L' ') attroff(A_BOLD);
        }

        attron(COLOR_PAIR(C_STATUS));
        mvaddwstr(y + boxH - 2, x + 3, L"Esc close | Up/Down scroll");
        attroff(COLOR_PAIR(C_STATUS));

        refresh();

        wint_t ch;
        int result = get_wch(&ch);
        if (result == ERR) continue;

        if (result == KEY_CODE_YES) {
            if (ch == KEY_MOUSE) {
                MEVENT event;
                if (getmouse(&event) == OK) {
                    if (event.bstate & BUTTON4_PRESSED) scroll--;
                    else if (event.bstate & BUTTON5_PRESSED) scroll++;
                    else if (event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)) {
                        if (event.y <= y || event.y >= y + boxH - 1 ||
                            event.x <= x || event.x >= x + boxW - 1) {
                            setStatus(L"help closed");
                            return;
                        }
                    }
                }
            } else if (ch == KEY_UP) scroll--;
            else if (ch == KEY_DOWN) scroll++;
            else if (ch == KEY_PPAGE) scroll -= bodyH;
            else if (ch == KEY_NPAGE) scroll += bodyH;
            continue;
        }

        if (ch == 27 || ch == L'q' || ch == L'Q' || ch == L'\n' || ch == L'\r') {
            setStatus(L"help closed");
            return;
        }
    }
}

bool confirmQuit() {
    if (!dirty) return true;

    int h, w;
    getmaxyx(stdscr, h, w);
    int boxW = std::min(54, w - 4);
    int boxH = 7;
    int y = std::max(1, (h - boxH) / 2);
    int x = std::max(1, (w - boxW) / 2);

    InputModeGuard inputGuard(1);

    while (true) {
        draw();
        drawBox(y, x, boxH, boxW);
        attron(A_BOLD);
        mvaddwstr(y + 1, x + 3, L"Unsaved changes");
        attroff(A_BOLD);
        mvaddwstr(y + 3, x + 3, L"s save and quit | q quit | Esc cancel");
        refresh();

        wint_t ch;
        int result = get_wch(&ch);
        if (result == ERR) continue;

        if (result == KEY_CODE_YES && ch == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK &&
                (event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED))) {
                if (event.y == y + 3) {
                    if (event.x >= x + 3 && event.x <= x + 18) {
                        saveFile();
                        return true;
                    }

                    if (event.x >= x + 21 && event.x <= x + 27)
                        return true;

                    if (event.x >= x + 30) {
                        setStatus(L"quit cancelled");
                        return false;
                    }
                } else if (event.y <= y || event.y >= y + boxH - 1 ||
                           event.x <= x || event.x >= x + boxW - 1) {
                    setStatus(L"quit cancelled");
                    return false;
                }
            }

            continue;
        }

        if (ch == L's' || ch == L'S') {
            saveFile();
            return true;
        }

        if (ch == L'q' || ch == L'Q') {
            return true;
        }

        if (ch == 27) {
            setStatus(L"quit cancelled");
            return false;
        }
    }
}

bool handleSpecialKey(wint_t key) {
    if (key == KEY_LEFT) moveLeft();
    else if (key == KEY_RIGHT) moveRight();
    else if (key == KEY_UP) moveUp();
    else if (key == KEY_DOWN) moveDown();
    else if (key == KEY_BACKSPACE) backspace();
    else if (key == KEY_DC) deleteForward();
    else if (key == KEY_HOME) moveHome();
    else if (key == KEY_END) moveEnd();
    else if (key == KEY_PPAGE) pageUp();
    else if (key == KEY_NPAGE) pageDown();
    else if (key == KEY_F(1)) showHelp();
    else if (key == KEY_MOUSE) handleMouse();
    else return false;

    return true;
}

void handleAltKey() {
    timeout(altKeyDelayMs);

    wint_t next;
    int nextResult = get_wch(&next);

    timeout(liveInputDelayMs);

    if (nextResult == ERR) return;

    if (next == L's' || next == L'S')
        toggleSelection();
    else if (next == L'm' || next == L'M')
        toggleMouse();
}

bool handleTextKey(wint_t key) {
    if (key == 17) return confirmQuit();

    if (key == 19) saveFile();
    else if (key == 15) saveAs();
    else if (key == 3) copyLine();
    else if (key == 24) cutLine();
    else if (key == 22) pasteText();
    else if (key == 26) undoEdit();
    else if (key == 25) redoEdit();
    else if (key == 6) searchPrompt();
    else if (key == 7) gotoLine();
    else if (key == 18) showRat = !showRat;
    else if (key == 14) findNext();
    else if (key == 127 || key == 8) backspace();
    else if (key == 9) insertTab();
    else if (key == 1) moveHome();
    else if (key == 5) moveEnd();
    else if (key == L'\n' || key == L'\r') newLine();
    else if (key >= 32) insertChar((wchar_t)key);

    return false;
}

int main(int argc, char** argv) {
    setlocale(LC_ALL, "");
    loadSettings();

    if (argc >= 2) {
        filename = argv[1];
    } else {
        initCurses();
        filename = startMenu();

        if (filename.empty()) {
            endwin();
            return 0;
        }
    }

    if (!fs::exists(filename)) {
        std::ofstream create(filename);
        create.close();
    }

    loadFile();
    initCurses();

    while (true) {
        checkExternalUpdate();
        draw();

        wint_t ch;
        int result = get_wch(&ch);

        if (result == ERR) continue;

        if (result == KEY_CODE_YES) {
            handleSpecialKey(ch);
            continue;
        }

        if (ch == 27) {
            handleAltKey();
            continue;
        }

        if (handleTextKey(ch)) break;
    }

    endwin();

    return 0;
}
