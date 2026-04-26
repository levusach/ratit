# ratit

`ratit` is a small terminal text editor built with ncurses. It keeps the interface compact, supports syntax highlighting for C++ and Python, and includes a tiny rat-themed status companion.

## Features

- File picker when started without a file path
- UTF-8 text editing
- C++ and Python syntax highlighting
- Selection, copy, cut, paste, undo, redo
- Search, go to line, save as
- Horizontal scrolling for long lines
- Configurable tab width and rat visibility through `~/.ratitrc`

## Build

```sh
make
```

## Install

```sh
sudo make install
```

## Usage

```sh
ratit
ratit path/to/file.cpp
```

Press `F1` inside the editor to see key bindings.

## Config

Optional `~/.ratitrc`:

```ini
tab_width=4
show_rat=true
```

