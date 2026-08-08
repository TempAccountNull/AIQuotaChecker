#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

// Extern declarations - defined in notepad_logger.cpp
extern HWND CachedHWND;
extern void notepad_logger(const char* str, ...);

// Locate Notepad's text control. Classic Notepad (Win10) exposes a direct "Edit"
// child; the new Notepad (Win11) nests a "RichEditD2DPT" control several levels
// deep, so this also does a recursive descendant search. Returns nullptr when no
// suitable Notepad window/control is found.
extern HWND notepad_logger_find_target();
