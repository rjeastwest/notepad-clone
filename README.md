# Notepad Clone

A lightweight single-document text editor for Windows built with the Win32 API. It mirrors the core experience of the classic Notepad application, including standard file commands and Common User Access (CUA) editing shortcuts.

## Features
- New/Open/Save/Save As with UTF-8 text persistence and change tracking prompts.
- Edit operations: undo, cut, copy, paste, delete, select all, time/date stamp.
- Find, Find Next, Replace, Replace All, and Go To line number dialogs.
- Word wrap toggle, font selection dialog, and status bar with caret position.
- Drag-and-drop file loading and accelerator keys matching Notepad defaults.

## Not Yet Implemented
- .LOG support
- Printing support

## Building
1. Open `NotepadClone.sln` in Visual Studio 2022 (or newer).
2. Choose your preferred configuration (Debug/Release, Win32/x64).
3. Build and run the `NotepadClone` project (`F5` to debug, `Ctrl+F5` to run without debugging).

No external dependencies are required beyond the Windows SDK that ships with Visual Studio.
