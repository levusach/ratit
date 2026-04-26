# Rat.it Roadmap for Future Agents

This document is for the next AI/dev working on Rat.it. Keep the editor small, friendly, terminal-native, and easy to understand. Rat.it should feel like a modern nano-like editor with its own rat-themed personality, not like a vim/emacs clone.

## Current Style Rules

- Keep the first screen usable. Do not turn Rat.it into a landing page.
- Keep the bottom status line compact. Detailed keybindings belong in `F1` help.
- Prefer simple, readable C++ over clever abstractions.
- Preserve the rat companion and status messages. They are part of the identity.
- Do not add heavy dependencies. `ncursesw`, standard C++17, and simple files are preferred.
- Every modal/popup must restore input mode correctly after closing.
- Mouse support must stay toggleable with `Alt+M`.
- New features should appear in `F1` help and README.
- After changes, run:

```sh
make clean && make
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow main.cpp -lncursesw -o /tmp/ratit-strict
```

## Feature 1: Tabs / Buffers

Add multiple open files.

### UX

- `Ctrl+Right`: switch to next tab.
- `Ctrl+Left`: switch to previous tab.
- `Ctrl+T`: open file picker and open selected file in a new tab.
- `Ctrl+W`: close current tab.
- If the current tab has unsaved changes, closing it must show the same kind of in-editor confirmation used by quit.
- Show tabs compactly near the top border, for example:

```text
 rat.it  [main.cpp] [README.md*] [theme.ini]
```

Use `*` for modified buffers.

### Implementation Notes

Replace the single global file/buffer state with a `Buffer` struct:

```cpp
struct Buffer {
    std::vector<std::wstring> lines{L""};
    std::string filename;
    std::wstring searchQuery;
    int cx = 0;
    int cy = 0;
    int rowoff = 0;
    int coloff = 0;
    bool dirty = false;
    fs::file_time_type lastWrite;
    std::deque<EditorState> undoStack;
    std::deque<EditorState> redoStack;
};
```

Then keep:

```cpp
std::vector<Buffer> buffers;
int activeBuffer = 0;
```

If this refactor is too big for one pass, do it in two commits:

1. Introduce `Buffer` while keeping one buffer.
2. Add multiple buffers and tab switching.

### Done When

- Opening a file from CLI creates one tab.
- Opening via `Ctrl+T` creates another tab.
- `Ctrl+Left/Right` switches tabs without losing cursor/scroll state.
- Saving affects only the active tab.
- Quit checks all dirty tabs.

## Feature 2: Search Highlighting

Search should highlight all visible matches, not only jump to the next match.

### UX

- `Ctrl+F`: prompt for search.
- `n`: next match.
- While `searchQuery` is not empty, all matches visible on screen are highlighted.
- If there are no matches, status should say `not found` and the rat can show search/confused mood.
- `Esc` in search prompt cancels cleanly and must never trap input.

### Implementation Notes

Add a render pass after syntax drawing and before/after selection drawing:

```cpp
void drawSearchMatchesForLine(int screenY, int screenX, int lineIndex, int maxWidth, int horizontalOffset);
```

Keep selection visually stronger than search matches. Suggested order:

1. Syntax line
2. Search match overlay
3. Selection overlay

Use a separate color pair, for example `C_SEARCH`.

### Done When

- Matches are visible while typing/searching.
- Horizontal scroll does not break match positions.
- Search highlight does not crash on empty query.
- Selection still looks stronger than search matches.

## Feature 3: Theme System

Add a large theme collection in a separate directory and allow users to create custom themes.

### Directory Layout

In repo:

```text
themes/
  rat.ini
  midnight.ini
  forest.ini
  paper.ini
  amber.ini
  ocean.ini
  mono.ini
  toxic-green.ini
  rose.ini
  cyber.ini
  nord.ini
  solarized-dark.ini
  solarized-light.ini
```

Installed location:

```text
/usr/share/ratit/themes/
```

User themes:

```text
~/.config/ratit/themes/
```

Config:

```ini
theme=rat
```

### Theme Format

Use a simple `.ini` format:

```ini
name=rat
keyword=cyan
string=green
comment=blue
number=yellow
status=magenta
line=blue
search=black,yellow
selection=black,white
background=default
```

Support color names first:

```text
black red green yellow blue magenta cyan white default
```

Optional later: support 256-color numbers.

### UX

- `theme=<name>` in config loads a theme.
- `Ctrl+P` command palette can come later, but if added now, include `Theme: next` and `Theme: previous`.
- If a theme fails to load, fall back to `rat` and show status `theme fallback`.

### Implementation Notes

Current color pairs are hardcoded in `initCurses()`. Replace hardcoding with a small `Theme` struct:

```cpp
struct Theme {
    short keywordFg;
    short stringFg;
    short commentFg;
    short numberFg;
    short statusFg;
    short lineFg;
    short searchFg;
    short searchBg;
    short selectionFg;
    short selectionBg;
};
```

Add:

```cpp
Theme currentTheme;
bool loadTheme(const std::string& name);
void applyTheme();
```

Update `Makefile install` to install `themes/`.

### Done When

- Built-in themes install with `sudo make install`.
- `theme=midnight` works from config.
- User theme overrides built-in theme with the same name.
- Bad theme does not crash Rat.it.
- README documents custom themes.

## Feature 4: Focus Mode

Focus mode should make Rat.it feel calm and distraction-free.

### UX

- `Alt+F`: toggle focus mode.
- In focus mode:
  - Hide rat companion.
  - Hide line numbers.
  - Hide decorative outer box if possible.
  - Keep a tiny status line only, or hide it until a status message changes.
  - Keep `F1 help` available.
- Status should say `focus on` / `focus off`.

### Implementation Notes

Add:

```cpp
bool focusMode = false;
```

Rendering should derive layout from mode:

```cpp
int textLeft = focusMode ? 1 : editorLeft;
int textHeight = focusMode ? h - 2 : h - 4;
```

Avoid duplicating the whole `draw()` function. Split it if needed:

```cpp
void updateViewport(int textHeight, int textWidth);
void drawEditorLines(int textLeft, int textWidth, int textHeight);
void drawStatusLine(...);
```

### Done When

- `Alt+F` toggles focus mode.
- Cursor positions stay correct in both modes.
- Search highlights and selections still align.
- Mouse clicks still place the cursor correctly in both modes.

## Nice Follow-Ups

- Command palette (`Ctrl+P`) for discoverable commands.
- Replace (`Ctrl+H`).
- Toggle comments for C++/Python.
- Duplicate line / move line up/down.
- Recovery autosave file for crashes.
- Man page: `man ratit`.

## Do Not Break

- `Alt+S` selection.
- `Alt+M` mouse toggle.
- `F1` help.
- Dirty-file confirmation on quit.
- `make clean && make`.
- Strict warning-free build.

