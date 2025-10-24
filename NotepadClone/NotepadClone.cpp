#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "resource.h"

namespace
{
constexpr wchar_t kClassName[] = L"NotepadCloneMainWindow";
HWND g_hEdit = nullptr;
HWND g_hStatus = nullptr;
HFONT g_hFont = nullptr;
LOGFONTW g_logFont = {};
std::wstring g_currentFilePath;
bool g_isDirty = false;
bool g_wordWrapEnabled = true;
FINDREPLACEW g_findReplace = {};
wchar_t g_findBuffer[256] = {};
wchar_t g_replaceBuffer[256] = {};
UINT g_findMsg = 0;
HWND g_findDialogWnd = nullptr;

INT_PTR CALLBACK GotoDlgProc(HWND dlg, UINT message, WPARAM wParam, LPARAM lParam);
void ResizeChildren(HWND hwnd);

std::wstring ToLower(const std::wstring& input)
{
    std::wstring copy = input;
    std::transform(copy.begin(), copy.end(), copy.begin(), ::towlower);
    return copy;
}

bool IsWordChar(wchar_t ch)
{
    return std::iswalnum(static_cast<wint_t>(ch)) != 0 || ch == L'_';
}

bool IsWholeWordMatch(const std::wstring& text, size_t index, size_t matchLength)
{
    if (text.empty())
        return false;

    const bool startOk = index == 0 || !IsWordChar(text[index - 1]);
    const bool endOk = (index + matchLength) >= text.size() || !IsWordChar(text[index + matchLength]);
    return startOk && endOk;
}

void UpdateWindowTitle(HWND hwnd)
{
    std::wstring title = g_currentFilePath.empty() ? L"Untitled" : g_currentFilePath;
    title.append(g_isDirty ? L" * - Notepad Clone" : L" - Notepad Clone");
    SetWindowTextW(hwnd, title.c_str());
}

void SetDirty(HWND hwnd, bool dirty)
{
    if (g_isDirty != dirty)
    {
        g_isDirty = dirty;
        UpdateWindowTitle(hwnd);
    }
}

void UpdateStatusBar(HWND hwnd)
{
    if (!hwnd || !g_hStatus || !g_hEdit)
        return;

    DWORD selStart = 0;
    DWORD selEnd = 0;
    SendMessageW(g_hEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart), reinterpret_cast<LPARAM>(&selEnd));
    LRESULT lineIndex = SendMessageW(g_hEdit, EM_LINEFROMCHAR, selEnd, 0);
    LRESULT lineStart = SendMessageW(g_hEdit, EM_LINEINDEX, lineIndex, 0);
    int column = static_cast<int>(selEnd - lineStart) + 1;
    int line = static_cast<int>(lineIndex) + 1;

    wchar_t buffer[128];
    swprintf_s(buffer, L"Ln %d, Col %d", line, column);
    SendMessageW(g_hStatus, SB_SETTEXT, 0, reinterpret_cast<LPARAM>(buffer));

    size_t length = static_cast<size_t>(GetWindowTextLengthW(g_hEdit));
    swprintf_s(buffer, L"Chars: %zu", length);
    SendMessageW(g_hStatus, SB_SETTEXT, 1, reinterpret_cast<LPARAM>(buffer));
}

std::wstring GetEditText()
{
    if (!g_hEdit)
        return {};

    const int length = GetWindowTextLengthW(g_hEdit);
    if (length <= 0)
        return {};

    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(g_hEdit, buffer.data(), static_cast<int>(buffer.size()));
    return std::wstring(buffer.data());
}

bool PromptToSave(HWND hwnd)
{
    if (!g_isDirty)
        return true;

    std::wstring message = L"Do you want to save changes to ";
    message += g_currentFilePath.empty() ? L"Untitled?" : g_currentFilePath + L"?";

    const int result = MessageBoxW(hwnd, message.c_str(), L"Notepad Clone", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (result == IDCANCEL)
        return false;
    if (result == IDYES)
    {
        SendMessageW(hwnd, WM_COMMAND, IDM_FILE_SAVE, 0);
        return !g_isDirty;
    }
    return true;
}

bool WriteTextFile(const std::wstring& path, const std::wstring& text)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
        return false;

    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    if (fwrite(bom, 1, sizeof(bom), file) != sizeof(bom))
    {
        fclose(file);
        return false;
    }

    int required = WideCharToMultiByte(
        CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required < 0)
    {
        fclose(file);
        return false;
    }

    if (required > 0)
    {
        std::vector<char> buffer(static_cast<size_t>(required));
        WideCharToMultiByte(
            CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), buffer.data(), required, nullptr, nullptr);

        if (fwrite(buffer.data(), 1, buffer.size(), file) != buffer.size())
        {
            fclose(file);
            return false;
        }
    }

    fclose(file);
    return true;
}

int ConvertToWide(UINT codePage, const char* data, int length, std::wstring& outText)
{
    if (length <= 0)
    {
        outText.clear();
        return 1;
    }

    const int wideLength = MultiByteToWideChar(codePage, 0, data, length, nullptr, 0);
    if (wideLength <= 0)
        return 0;

    std::vector<wchar_t> wide(static_cast<size_t>(wideLength));
    if (MultiByteToWideChar(codePage, 0, data, length, wide.data(), wideLength) <= 0)
        return 0;

    outText.assign(wide.data(), wide.data() + wide.size());
    return wideLength;
}

bool ReadTextFile(const std::wstring& path, std::wstring& outText)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
        return false;

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return false;
    }

    long fileSizeLong = ftell(file);
    if (fileSizeLong < 0)
    {
        fclose(file);
        return false;
    }

    if (fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return false;
    }

    const size_t fileSize = static_cast<size_t>(fileSizeLong);
    if (fileSize == 0)
    {
        fclose(file);
        outText.clear();
        return true;
    }

    if (fileSize > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        fclose(file);
        return false;
    }

    std::vector<char> buffer(fileSize);
    const size_t actuallyRead = fread(buffer.data(), 1, buffer.size(), file);
    fclose(file);
    if (actuallyRead != buffer.size())
        return false;

    if (buffer.size() >= 2)
    {
        const unsigned char b0 = static_cast<unsigned char>(buffer[0]);
        const unsigned char b1 = static_cast<unsigned char>(buffer[1]);
        if (b0 == 0xFF && b1 == 0xFE)
        {
            if ((buffer.size() - 2) % sizeof(wchar_t) != 0)
                return false;
            const size_t wcharCount = (buffer.size() - 2) / sizeof(wchar_t);
            std::wstring result(wcharCount, L'\0');
            if (wcharCount > 0)
                std::memcpy(&result[0], buffer.data() + 2, wcharCount * sizeof(wchar_t));
            outText = std::move(result);
            return true;
        }
        if (b0 == 0xFE && b1 == 0xFF)
        {
            if ((buffer.size() - 2) % sizeof(wchar_t) != 0)
                return false;
            const size_t wcharCount = (buffer.size() - 2) / sizeof(wchar_t);
            outText.resize(wcharCount);
            const unsigned char* src = reinterpret_cast<const unsigned char*>(buffer.data() + 2);
            for (size_t i = 0; i < wcharCount; ++i)
            {
                const unsigned char hi = src[i * 2];
                const unsigned char lo = src[i * 2 + 1];
                outText[i] = static_cast<wchar_t>((hi << 8) | lo);
            }
            return true;
        }
    }

    if (buffer.size() >= 3 && static_cast<unsigned char>(buffer[0]) == 0xEF &&
        static_cast<unsigned char>(buffer[1]) == 0xBB && static_cast<unsigned char>(buffer[2]) == 0xBF)
    {
        return ConvertToWide(CP_UTF8, buffer.data() + 3, static_cast<int>(buffer.size() - 3), outText) > 0;
    }

    if (ConvertToWide(CP_UTF8, buffer.data(), static_cast<int>(buffer.size()), outText) > 0)
        return true;

    if (ConvertToWide(CP_ACP, buffer.data(), static_cast<int>(buffer.size()), outText) > 0)
        return true;

    return false;
}

void DoFileNew(HWND hwnd)
{
    if (!PromptToSave(hwnd))
        return;
    SetWindowTextW(g_hEdit, L"");
    SendMessageW(g_hEdit, EM_SETMODIFY, FALSE, 0);
    g_currentFilePath.clear();
    SetDirty(hwnd, false);
    UpdateStatusBar(hwnd);
}

void DoFileOpen(HWND hwnd)
{
    if (!PromptToSave(hwnd))
        return;

    wchar_t fileBuffer[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Text Documents (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = static_cast<DWORD>(_countof(fileBuffer));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"txt";

    if (GetOpenFileNameW(&ofn))
    {
        std::wstring text;
        if (ReadTextFile(fileBuffer, text))
        {
            SetWindowTextW(g_hEdit, text.c_str());
            SendMessageW(g_hEdit, EM_SETMODIFY, FALSE, 0);
            g_currentFilePath = fileBuffer;
            SetDirty(hwnd, false);
            UpdateStatusBar(hwnd);
        }
        else
        {
            MessageBoxW(hwnd, L"Unable to open the file.", L"Error", MB_ICONERROR);
        }
    }
}

bool DoFileSave(HWND hwnd)
{
    if (g_currentFilePath.empty())
        return SendMessageW(hwnd, WM_COMMAND, IDM_FILE_SAVEAS, 0), !g_isDirty;

    std::wstring text = GetEditText();
    if (WriteTextFile(g_currentFilePath, text))
    {
        SendMessageW(g_hEdit, EM_SETMODIFY, FALSE, 0);
        SetDirty(hwnd, false);
        UpdateStatusBar(hwnd);
        return true;
    }
    MessageBoxW(hwnd, L"Unable to save the file.", L"Error", MB_ICONERROR);
    return false;
}

void DoFileSaveAs(HWND hwnd)
{
    wchar_t fileBuffer[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Text Documents (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = static_cast<DWORD>(_countof(fileBuffer));
    ofn.Flags = OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = L"txt";
    if (GetSaveFileNameW(&ofn))
    {
        g_currentFilePath = fileBuffer;
        DoFileSave(hwnd);
    }
}

void ToggleWordWrap(HWND hwnd)
{
    g_wordWrapEnabled = !g_wordWrapEnabled;

    std::wstring existingText = GetEditText();
    const bool wasDirtyFlag = g_isDirty;
    DWORD selStart = 0;
    DWORD selEnd = 0;
    SendMessageW(g_hEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart), reinterpret_cast<LPARAM>(&selEnd));
    const BOOL wasModified = static_cast<BOOL>(SendMessageW(g_hEdit, EM_GETMODIFY, 0, 0));

    HFONT currentFont = reinterpret_cast<HFONT>(SendMessageW(g_hEdit, WM_GETFONT, 0, 0));

    RECT rect;
    GetClientRect(hwnd, &rect);

    const DWORD baseStyle = WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL;
    const DWORD wrapStyle = g_wordWrapEnabled ? 0 : WS_HSCROLL | ES_AUTOHSCROLL;

    DestroyWindow(g_hEdit);
    g_hEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        nullptr,
        baseStyle | wrapStyle,
        0,
        0,
        rect.right,
        rect.bottom,
        hwnd,
        reinterpret_cast<HMENU>(IDC_MAIN_EDIT),
        GetModuleHandleW(nullptr),
        nullptr);

    SendMessageW(g_hEdit, EM_LIMITTEXT, 0, -1);
    SendMessageW(g_hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));
    SendMessageW(g_hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(currentFont), TRUE);
    SetWindowTextW(g_hEdit, existingText.c_str());
    SendMessageW(g_hEdit, EM_SETSEL, selStart, selEnd);
    SendMessageW(g_hEdit, EM_SETMODIFY, wasModified, 0);
    SetDirty(hwnd, wasDirtyFlag);
    UpdateStatusBar(hwnd);
    ResizeChildren(hwnd);
}

void ApplyFont(HWND hwnd, HFONT font)
{
    if (font)
        SendMessageW(g_hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    UpdateStatusBar(hwnd);
}

void ChooseEditorFont(HWND hwnd)
{
    CHOOSEFONTW cf = {};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = hwnd;
    cf.lpLogFont = &g_logFont;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;

    if (ChooseFontW(&cf))
    {
        if (g_hFont)
            DeleteObject(g_hFont);
        g_hFont = CreateFontIndirectW(&g_logFont);
        ApplyFont(hwnd, g_hFont);
    }
}

void InsertDateTime()
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t buffer[64];
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, buffer, static_cast<int>(_countof(buffer)));
    size_t len = wcslen(buffer);
    buffer[len++] = L' ';
    buffer[len] = L'\0';
    GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, buffer + len, static_cast<int>(_countof(buffer) - len));

    SendMessageW(g_hEdit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(buffer));
}

void GoToLine(HWND hwnd)
{
    if (!g_hEdit)
        return;

    const INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_GOTO), hwnd, GotoDlgProc, 0);
    if (result == -1)
        MessageBoxW(hwnd, L"Unable to open Go To dialog.", L"Error", MB_ICONERROR);
}

void FindReplaceInit(HWND hwnd)
{
    if (!g_findMsg)
        g_findMsg = RegisterWindowMessageW(FINDMSGSTRINGW);

    g_findReplace = {};
    g_findReplace.lStructSize = sizeof(g_findReplace);
    g_findReplace.hwndOwner = hwnd;
    g_findReplace.lpstrFindWhat = g_findBuffer;
    g_findReplace.wFindWhatLen = static_cast<WORD>(_countof(g_findBuffer));
    g_findReplace.lpstrReplaceWith = g_replaceBuffer;
    g_findReplace.wReplaceWithLen = static_cast<WORD>(_countof(g_replaceBuffer));
}

bool PerformSearch(const std::wstring& needle, DWORD flags, bool wrap)
{
    if (!g_hEdit)
        return false;

    if (needle.empty())
    {
        SendMessageW(GetParent(g_hEdit), WM_COMMAND, IDM_EDIT_FIND, 0);
        return false;
    }

    std::wstring text = GetEditText();
    if (text.empty())
    {
        MessageBoxW(GetParent(g_hEdit), L"Cannot find the specified text.", L"Notepad Clone", MB_ICONINFORMATION);
        return false;
    }

    DWORD selStart = 0;
    DWORD selEnd = 0;
    SendMessageW(g_hEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart), reinterpret_cast<LPARAM>(&selEnd));

    const bool searchDown = (flags & FR_DOWN) != 0;
    const bool matchCase = (flags & FR_MATCHCASE) != 0;
    const bool wholeWord = (flags & FR_WHOLEWORD) != 0;

    std::wstring searchSpace = matchCase ? text : ToLower(text);
    std::wstring match = matchCase ? needle : ToLower(needle);

    auto searchForward = [&](size_t start) -> size_t
    {
        size_t pos = searchSpace.find(match, start);
        while (pos != std::wstring::npos)
        {
            if (!wholeWord || IsWholeWordMatch(text, pos, match.length()))
                return pos;
            pos = searchSpace.find(match, pos + 1);
        }
        return std::wstring::npos;
    };

    auto searchBackward = [&](size_t start) -> size_t
    {
        if (searchSpace.empty())
            return std::wstring::npos;

        size_t pos = start;
        while (true)
        {
            pos = searchSpace.rfind(match, pos);
            if (pos == std::wstring::npos)
                break;
            if (!wholeWord || IsWholeWordMatch(text, pos, match.length()))
                return pos;
            if (pos == 0)
                break;
            pos--;
        }
        return std::wstring::npos;
    };

    size_t found = std::wstring::npos;
    if (searchDown)
    {
        size_t startPos = std::min(static_cast<size_t>(selEnd), searchSpace.length());
        found = searchForward(startPos);
        if (wrap && found == std::wstring::npos)
            found = searchForward(0);
    }
    else
    {
        size_t startPos = selStart == 0 ? 0 : static_cast<size_t>(selStart - 1);
        startPos = std::min(startPos, searchSpace.length() == 0 ? 0 : searchSpace.length() - 1);
        found = searchBackward(startPos);
        if (wrap && found == std::wstring::npos && !searchSpace.empty())
            found = searchBackward(searchSpace.length() - 1);
    }

    if (found != std::wstring::npos)
    {
        const size_t end = found + match.length();
        SendMessageW(g_hEdit, EM_SETSEL, static_cast<WPARAM>(found), static_cast<LPARAM>(end));
        SendMessageW(g_hEdit, EM_SCROLLCARET, 0, 0);
        UpdateStatusBar(GetParent(g_hEdit));
        return true;
    }

    MessageBoxW(GetParent(g_hEdit), L"Cannot find the specified text.", L"Notepad Clone", MB_ICONINFORMATION);
    return false;
}

void HandleFindReplaceMessage(HWND hwnd, const FINDREPLACEW* fr)
{
    if (fr->Flags & FR_DIALOGTERM)
    {
        g_findDialogWnd = nullptr;
        return;
    }

    if (fr->Flags & FR_FINDNEXT)
    {
        PerformSearch(fr->lpstrFindWhat, fr->Flags, true);
        return;
    }

    if (fr->Flags & FR_REPLACE)
    {
        DWORD selStart = 0;
        DWORD selEnd = 0;
        SendMessageW(g_hEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart), reinterpret_cast<LPARAM>(&selEnd));

        std::wstring text = GetEditText();
        const bool matchCase = (fr->Flags & FR_MATCHCASE) != 0;
        if (selEnd > selStart && selEnd <= text.length())
        {
            std::wstring selection = text.substr(selStart, selEnd - selStart);
            std::wstring selectedNormalized = matchCase ? selection : ToLower(selection);
            std::wstring target = matchCase ? std::wstring(fr->lpstrFindWhat) : ToLower(std::wstring(fr->lpstrFindWhat));
            if (selectedNormalized == target)
            {
                SendMessageW(g_hEdit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(fr->lpstrReplaceWith));
                SetDirty(hwnd, true);
            }
        }

        PerformSearch(fr->lpstrFindWhat, fr->Flags, true);
        return;
    }

    if (fr->Flags & FR_REPLACEALL)
    {
        std::wstring text = GetEditText();

        std::wstring needle = fr->lpstrFindWhat;
        std::wstring replacement = fr->lpstrReplaceWith;

        std::wstring searchIn = (fr->Flags & FR_MATCHCASE) ? text : ToLower(text);
        std::wstring needleNormalized = (fr->Flags & FR_MATCHCASE) ? needle : ToLower(needle);
        std::wstring replacementNormalized =
            (fr->Flags & FR_MATCHCASE) ? replacement : ToLower(replacement);

        size_t pos = 0;
        size_t count = 0;
        if (!needleNormalized.empty())
        {
            while (true)
            {
                pos = searchIn.find(needleNormalized, pos);
                if (pos == std::wstring::npos)
                    break;
                if ((fr->Flags & FR_WHOLEWORD) && !IsWholeWordMatch(text, pos, needle.length()))
                {
                    pos += 1;
                    continue;
                }
                text.replace(pos, needle.length(), replacement);
                searchIn.replace(pos, needle.length(), replacementNormalized);
                pos += replacement.length();
                count++;
            }
        }

        SetWindowTextW(g_hEdit, text.c_str());
        SendMessageW(g_hEdit, EM_SETMODIFY, TRUE, 0);
        SetDirty(hwnd, true);
        UpdateStatusBar(hwnd);

        wchar_t buffer[64];
        swprintf_s(buffer, L"Replaced %zu occurrence(s).", count);
        MessageBoxW(hwnd, buffer, L"Notepad Clone", MB_ICONINFORMATION);
    }
}

INT_PTR CALLBACK GotoDlgProc(HWND dlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_INITDIALOG:
        SetDlgItemInt(dlg, IDC_EDIT_GOTO, 1, FALSE);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            BOOL success = FALSE;
            const UINT lineNumber = GetDlgItemInt(dlg, IDC_EDIT_GOTO, &success, FALSE);
            if (!success || lineNumber == 0)
            {
                MessageBeep(MB_ICONERROR);
                break;
            }

            const LRESULT totalLines = SendMessageW(g_hEdit, EM_GETLINECOUNT, 0, 0);
            if (lineNumber > static_cast<UINT>(totalLines))
            {
                MessageBeep(MB_ICONERROR);
                break;
            }

            const LRESULT charIndex = SendMessageW(g_hEdit, EM_LINEINDEX, static_cast<WPARAM>(lineNumber - 1), 0);
            SendMessageW(g_hEdit, EM_SETSEL, charIndex, charIndex);
            SendMessageW(g_hEdit, EM_SCROLLCARET, 0, 0);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

void InitializeEditor(HWND hwnd, HINSTANCE hInstance)
{
    RECT rect;
    GetClientRect(hwnd, &rect);

    g_hEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        nullptr,
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | ES_NOHIDESEL,
        0,
        0,
        rect.right,
        rect.bottom,
        hwnd,
        reinterpret_cast<HMENU>(IDC_MAIN_EDIT),
        hInstance,
        nullptr);

    SendMessageW(g_hEdit, EM_LIMITTEXT, 0, -1);
    SendMessageW(g_hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));

    g_logFont = {};
    lstrcpyW(g_logFont.lfFaceName, L"Consolas");
    HDC hdc = GetDC(hwnd);
    if (hdc)
    {
        g_logFont.lfHeight = -MulDiv(11, GetDeviceCaps(hdc, LOGPIXELSY), 72);
        ReleaseDC(hwnd, hdc);
    }
    else
    {
        g_logFont.lfHeight = -MulDiv(11, 96, 72);
    }
    g_logFont.lfWeight = FW_NORMAL;
    g_logFont.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    g_logFont.lfCharSet = DEFAULT_CHARSET;

    if (g_hFont)
        DeleteObject(g_hFont);
    g_hFont = CreateFontIndirectW(&g_logFont);
    ApplyFont(hwnd, g_hFont);
}

void ResizeChildren(HWND hwnd)
{
    RECT rcClient;
    GetClientRect(hwnd, &rcClient);

    int statusHeight = 0;
    if (g_hStatus)
    {
        SendMessageW(g_hStatus, WM_SIZE, 0, 0);
        RECT rcStatus;
        GetWindowRect(g_hStatus, &rcStatus);
        statusHeight = rcStatus.bottom - rcStatus.top;
    }

    MoveWindow(g_hEdit, 0, 0, rcClient.right, rcClient.bottom - statusHeight, TRUE);
}

void InitStatusBar(HWND hwnd)
{
    g_hStatus = CreateStatusWindowW(WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, nullptr, hwnd, IDC_MAIN_STATUS);
    int parts[] = {150, -1};
    SendMessageW(g_hStatus, SB_SETPARTS, _countof(parts), reinterpret_cast<LPARAM>(parts));
    UpdateStatusBar(hwnd);
}

} // namespace

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == g_findMsg)
    {
        HandleFindReplaceMessage(hwnd, reinterpret_cast<FINDREPLACEW*>(lParam));
        return 0;
    }

    switch (msg)
    {
    case WM_CREATE:
    {
        const auto* createStruct = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        InitializeEditor(hwnd, createStruct->hInstance);
        InitStatusBar(hwnd);
        FindReplaceInit(hwnd);
        UpdateWindowTitle(hwnd);
        CheckMenuItem(GetMenu(hwnd), IDM_FORMAT_WORDWRAP, MF_BYCOMMAND | (g_wordWrapEnabled ? MF_CHECKED : MF_UNCHECKED));
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }
    case WM_SETFOCUS:
        SetFocus(g_hEdit);
        return 0;
    case WM_SIZE:
        ResizeChildren(hwnd);
        return 0;
    case WM_DROPFILES:
    {
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        wchar_t file[MAX_PATH];
        if (DragQueryFileW(hDrop, 0, file, MAX_PATH))
        {
            DragFinish(hDrop);
            SetForegroundWindow(hwnd);
            if (!g_isDirty || PromptToSave(hwnd))
            {
                std::wstring droppedPath = file;
                std::wstring text;
                if (ReadTextFile(droppedPath, text))
                {
                    SetWindowTextW(g_hEdit, text.c_str());
                    SendMessageW(g_hEdit, EM_SETMODIFY, FALSE, 0);
                    g_currentFilePath = std::move(droppedPath);
                    SetDirty(hwnd, false);
                    UpdateStatusBar(hwnd);
                }
                else
                {
                    MessageBoxW(hwnd, L"Unable to open the dropped file.", L"Error", MB_ICONERROR);
                }
            }
        }
        else
        {
            DragFinish(hDrop);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_MAIN_EDIT:
            if (HIWORD(wParam) == EN_CHANGE)
            {
                SetDirty(hwnd, true);
                UpdateStatusBar(hwnd);
            }
            break;
        case IDM_FILE_NEW:
            DoFileNew(hwnd);
            break;
        case IDM_FILE_OPEN:
            DoFileOpen(hwnd);
            break;
        case IDM_FILE_SAVE:
            DoFileSave(hwnd);
            break;
        case IDM_FILE_SAVEAS:
            DoFileSaveAs(hwnd);
            break;
        case IDM_FILE_PAGESETUP:
        case IDM_FILE_PRINT:
            MessageBoxW(hwnd, L"Printing is not implemented in this sample.", L"Notepad Clone", MB_ICONINFORMATION);
            break;
        case IDM_FILE_EXIT:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        case IDM_EDIT_UNDO:
            SendMessageW(g_hEdit, WM_UNDO, 0, 0);
            break;
        case IDM_EDIT_CUT:
            SendMessageW(g_hEdit, WM_CUT, 0, 0);
            break;
        case IDM_EDIT_COPY:
            SendMessageW(g_hEdit, WM_COPY, 0, 0);
            break;
        case IDM_EDIT_PASTE:
            SendMessageW(g_hEdit, WM_PASTE, 0, 0);
            break;
        case IDM_EDIT_DELETE:
            SendMessageW(g_hEdit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
            break;
        case IDM_EDIT_SELECTALL:
            SendMessageW(g_hEdit, EM_SETSEL, 0, -1);
            break;
        case IDM_EDIT_TIME_DATE:
            InsertDateTime();
            break;
        case IDM_EDIT_FIND:
            if (g_findDialogWnd && IsWindow(g_findDialogWnd))
            {
                DestroyWindow(g_findDialogWnd);
                g_findDialogWnd = nullptr;
            }
            FindReplaceInit(hwnd);
            g_findReplace.Flags &= ~(FR_REPLACE | FR_REPLACEALL);
            g_findReplace.Flags |= FR_DOWN;
            if (HWND findWnd = FindTextW(&g_findReplace))
            {
                g_findDialogWnd = findWnd;
                SetForegroundWindow(findWnd);
            }
            else
            {
                g_findDialogWnd = nullptr;
            }
            break;
        case IDM_EDIT_FINDNEXT:
            PerformSearch(g_findBuffer, g_findReplace.Flags | FR_DOWN, true);
            break;
        case IDM_EDIT_REPLACE:
            if (g_findDialogWnd && IsWindow(g_findDialogWnd))
            {
                DestroyWindow(g_findDialogWnd);
                g_findDialogWnd = nullptr;
            }
            FindReplaceInit(hwnd);
            g_findReplace.Flags |= FR_REPLACE;
            if (HWND replaceWnd = ReplaceTextW(&g_findReplace))
            {
                g_findDialogWnd = replaceWnd;
                SetForegroundWindow(replaceWnd);
            }
            else
            {
                g_findDialogWnd = nullptr;
            }
            break;
        case IDM_EDIT_GOTO:
            GoToLine(hwnd);
            break;
        case IDM_FORMAT_WORDWRAP:
            ToggleWordWrap(hwnd);
            CheckMenuItem(GetMenu(hwnd), IDM_FORMAT_WORDWRAP, MF_BYCOMMAND | (g_wordWrapEnabled ? MF_CHECKED : MF_UNCHECKED));
            break;
        case IDM_FORMAT_FONT:
            ChooseEditorFont(hwnd);
            break;
        case IDM_HELP_ABOUT:
            MessageBoxW(hwnd, L"Notepad Clone\nBuilt with the Win32 API.", L"About Notepad Clone", MB_OK);
            break;
        default:
            break;
        }
        return 0;
    case WM_CLOSE:
        if (!PromptToSave(hwnd))
            return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_hFont)
        {
            DeleteObject(g_hFont);
            g_hFont = nullptr;
        }
        if (g_findDialogWnd)
        {
            DestroyWindow(g_findDialogWnd);
            g_findDialogWnd = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int nCmdShow)
{
    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);
    wc.lpszClassName = kClassName;
    wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc))
    {
        MessageBoxW(nullptr, L"Failed to register window class.", L"Error", MB_ICONERROR);
        return 0;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        kClassName,
        L"Notepad Clone",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        800,
        600,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    if (!hwnd)
        return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    HACCEL hAccel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDR_ACCEL));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        if (hAccel && TranslateAcceleratorW(hwnd, hAccel, &msg))
            continue;
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}
