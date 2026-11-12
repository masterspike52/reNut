/**
 * @file        crashreporter/ui_win32.cpp
 * @brief       Fallback crash reporter dialog, no graphics device of any kind
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Used only when RunImGuiUi() cannot start Direct3D 11, even on WARP.
 *              Plain user32 controls, so it works wherever the desktop does.
 *
 *              The session is UTF-8 throughout; this file is the one place that
 *              still needs UTF-16, so it widens at each use rather than making
 *              the shared state carry two representations.
 */

#include "util.h"

#include <CommCtrl.h>

#include <string>

#include "reporter.h"

namespace reporter {

namespace {

constexpr int kIdHeadline = 1001;
constexpr int kIdSummary = 1002;
constexpr int kIdShowDetails = 1003;
constexpr int kIdShowLog = 1004;
constexpr int kIdContent = 1005;
constexpr int kIdPrompt = 1006;
constexpr int kIdMessage = 1007;
constexpr int kIdAutoSend = 1008;
constexpr int kIdSend = 1009;
constexpr int kIdRestart = 1010;
constexpr int kIdQuit = 1011;
constexpr int kIdStatus = 1012;

HFONT g_font = nullptr;

/// Edit controls only break lines on CRLF.
std::wstring WideCrlf(const std::string& utf8) {
  const std::wstring text = Widen(utf8);
  std::wstring out;
  out.reserve(text.size() + text.size() / 16);
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) {
      out += L"\r\n";
    } else {
      out += text[i];
    }
  }
  return out;
}

HWND Create(HWND parent, const wchar_t* klass, const wchar_t* text, DWORD style, int x, int y,
            int width, int height, int id) {
  HWND control = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
                                 parent, reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
  return control;
}

// Says what is true right now rather than what was true on arrival: telling
// someone nothing has been sent immediately after they sent it reads like the
// send failed.
void UpdateSubtitle(HWND window) {
  const wchar_t* text;
  if (g_session.sent) {
    text = L"Your report has been sent. Thank you.";
  } else if (g_session.bug_report) {
    text = L"Describe the problem below. The log is attached as it stands now.";
  } else if (!g_session.can_send()) {
    text = L"No crash report was captured, so there is nothing to send.";
  } else {
    text = L"Nothing has been sent. You can look over what would be reported below.";
  }
  SetDlgItemTextW(window, kIdSummary, text);
}

void ShowContent(HWND window, bool details) {
  SetWindowTextW(GetDlgItem(window, kIdContent),
                 WideCrlf(details ? g_session.details : g_session.log).c_str());
  if (!g_session.bug_report) {
    SendDlgItemMessageW(window, kIdShowDetails, BM_SETCHECK,
                        details ? BST_CHECKED : BST_UNCHECKED, 0);
    SendDlgItemMessageW(window, kIdShowLog, BM_SETCHECK, details ? BST_UNCHECKED : BST_CHECKED, 0);
  }
}

void BuildLayout(HWND window) {
  const int margin = 14;
  const int width = 760;
  int y = margin;

  const std::wstring headline = g_session.bug_report
                                    ? std::wstring(L"Send a bug report")
                                    : (Widen(g_session.app_name) + L" has crashed.");
  Create(window, L"STATIC", headline.c_str(), 0, margin, y, width, 22, kIdHeadline);
  y += 26;

  Create(window, L"STATIC", L"", 0, margin, y, width, 20, kIdSummary);
  y += 26;

  // Nothing crashed in bug report mode, so there is no exception to show.
  if (!g_session.bug_report) {
    Create(window, L"BUTTON", L"Crash details", BS_AUTORADIOBUTTON | WS_GROUP, margin, y, 120, 22,
           kIdShowDetails);
    Create(window, L"BUTTON", L"Log", BS_AUTORADIOBUTTON, margin + 130, y, 100, 22, kIdShowLog);
    y += 26;
  }

  Create(window, L"EDIT", L"",
         ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL | WS_BORDER, margin,
         y, width, 260, kIdContent);
  y += 272;

  Create(window, L"STATIC",
         g_session.bug_report ? L"What went wrong?"
                              : L"What were you doing when it crashed? (optional)",
         0, margin, y, width, 20, kIdPrompt);
  y += 22;

  Create(window, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, margin, y,
         width, 70, kIdMessage);
  y += 82;

  if (!g_session.reporting_enabled && !g_session.bug_report) {
    Create(window, L"BUTTON", L"Send future crash reports automatically", BS_AUTOCHECKBOX, margin,
           y, 320, 22, kIdAutoSend);
  }
  Create(window, L"STATIC", L"", 0, margin + 330, y, 420, 22, kIdStatus);
  y += 30;

  Create(window, L"BUTTON", L"Send report", BS_DEFPUSHBUTTON, margin, y, 130, 30, kIdSend);
  if (!g_session.bug_report) {
    Create(window, L"BUTTON", L"Restart", 0, margin + 140, y, 110, 30, kIdRestart);
  }
  Create(window, L"BUTTON", g_session.bug_report ? L"Cancel" : L"Quit", 0, margin + 260, y, 110, 30,
         kIdQuit);

  if (!g_session.can_send()) {
    EnableWindow(GetDlgItem(window, kIdSend), FALSE);
  }
  if (!g_session.bug_report && g_session.game_exe.empty()) {
    EnableWindow(GetDlgItem(window, kIdRestart), FALSE);
  }

  UpdateSubtitle(window);
  ShowContent(window, !g_session.bug_report);
}

void OnSend(HWND window) {
  const HWND message_box = GetDlgItem(window, kIdMessage);
  const int length = GetWindowTextLengthW(message_box);
  std::wstring wide(static_cast<size_t>(length), L'\0');
  if (length > 0) {
    GetWindowTextW(message_box, wide.data(), length + 1);
  }
  const std::string message = Narrow(wide);

  // A bug report with nothing written in it is just a log dump.
  if (g_session.bug_report && message.empty()) {
    SetDlgItemTextW(window, kIdStatus, L"Type a description first.");
    return;
  }

  EnableWindow(GetDlgItem(window, kIdSend), FALSE);
  SetDlgItemTextW(window, kIdStatus, L"Sending...");
  SetCursor(LoadCursorW(nullptr, IDC_WAIT));

  // In bug report mode the game owns the Sentry connection, so all this window
  // does is hand the text back and close.
  const bool ok = g_session.bug_report ? WriteMessageForGame(message) : SendReport(message);

  SetCursor(LoadCursorW(nullptr, IDC_ARROW));
  if (ok && g_session.bug_report) {
    g_session.sent = true;
    DestroyWindow(window);
    return;
  }
  if (ok) {
    g_session.sent = true;
    if (IsDlgButtonChecked(window, kIdAutoSend) == BST_CHECKED) {
      EnableReportingInConfig();
    }
    SetDlgItemTextW(window, kIdStatus, L"");
    UpdateSubtitle(window);
    EnableWindow(message_box, FALSE);
  } else {
    SetDlgItemTextW(window, kIdStatus, L"Could not send the report.");
    EnableWindow(GetDlgItem(window, kIdSend), TRUE);
  }
}

void Finish(HWND window, bool restart) {
  // Only a crash leaves a report on disk to clean up.
  if (!g_session.sent && !g_session.bug_report) {
    DiscardReport();
  }
  if (restart) {
    RestartGame();
  }
  DestroyWindow(window);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      BuildLayout(window);
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kIdShowDetails:
          ShowContent(window, true);
          return 0;
        case kIdShowLog:
          ShowContent(window, false);
          return 0;
        case kIdSend:
          OnSend(window);
          return 0;
        case kIdRestart:
          Finish(window, true);
          return 0;
        case kIdQuit:
          Finish(window, false);
          return 0;
        default:
          break;
      }
      break;
    case WM_CTLCOLORSTATIC: {
      // Labels sit on the dialog background, so they need both the transparent
      // background and an explicit colour; without the latter they inherit
      // whatever the DC happened to carry.
      HDC dc = reinterpret_cast<HDC>(wparam);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_CLOSE:
      Finish(window, false);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

void RunWin32Ui() {
  const HINSTANCE instance = GetModuleHandleW(nullptr);

  INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);

  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
  g_font = CreateFontIndirectW(&metrics.lfMessageFont);

  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
  window_class.lpszClassName = L"RexCrashReporter";
  window_class.hIcon = LoadIconW(nullptr, IDI_ERROR);
  RegisterClassExW(&window_class);

  const std::wstring title =
      Widen(g_session.app_name) + (g_session.bug_report ? L" - bug report" : L" - crash report");
  RECT bounds = {0, 0, 788, 620};
  AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, FALSE);
  HWND window = CreateWindowExW(
      0, window_class.lpszClassName, title.c_str(),
      (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_VISIBLE, CW_USEDEFAULT,
      CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr,
      instance, nullptr);
  if (!window) {
    return;
  }
  SetForegroundWindow(window);

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }

  DeleteObject(g_font);
}

}  // namespace reporter
