#include "renut_engine/app/crash_dump.h"

#ifdef _WIN32

#include <windows.h>

#include <dbghelp.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>

namespace renut::crash {

namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                           PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION,
                                           PMINIDUMP_CALLBACK_INFORMATION);

// Exception codes recorded for crashes that raise no exception.
constexpr DWORD kCodeAbort = 0xE0000001;
constexpr DWORD kCodeTerminate = 0xE0000002;
constexpr DWORD kCodeInvalidParameter = 0xE0000003;
constexpr DWORD kCodePureCall = 0xE0000004;

MiniDumpWriteDumpFn g_write_dump = nullptr;
wchar_t g_dump_dir[MAX_PATH] = {};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

// The dump is written by a thread created up front: the crashing thread may be
// out of stack (stack overflow) or hold locks the writer would need.
HANDLE g_request = nullptr;
HANDLE g_done = nullptr;
std::atomic<bool> g_dumping{false};
DWORD g_crash_thread_id = 0;
EXCEPTION_POINTERS *g_crash_pointers = nullptr;

DWORD WINAPI DumpThread(void *) {
  WaitForSingleObject(g_request, INFINITE);
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t path[MAX_PATH];
  std::swprintf(path, MAX_PATH, L"%ls\\renut_%04u%02u%02u_%02u%02u%02u.dmp", g_dump_dir,
                unsigned(t.wYear), unsigned(t.wMonth), unsigned(t.wDay), unsigned(t.wHour),
                unsigned(t.wMinute), unsigned(t.wSecond));
  const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = g_crash_thread_id;
    info.ExceptionPointers = g_crash_pointers;
    info.ClientPointers = FALSE;
    const auto type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                    MiniDumpWithDataSegs | MiniDumpWithUnloadedModules |
                                    MiniDumpWithHandleData);
    const BOOL written = g_write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                      g_crash_pointers ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
    wchar_t message[MAX_PATH + 64];
    std::swprintf(message, MAX_PATH + 64, L"renut: %ls crash dump %ls\n",
                  written ? L"wrote" : L"failed to write", path);
    OutputDebugStringW(message);
    std::fputws(message, stderr);
  }
  SetEvent(g_done);
  return 0;
}

void WriteDump(EXCEPTION_POINTERS *pointers) {
  if (!g_write_dump || !g_request || g_dumping.exchange(true))
    return;
  g_crash_thread_id = GetCurrentThreadId();
  g_crash_pointers = pointers;
  SetEvent(g_request);
  WaitForSingleObject(g_done, 60000);
}

// For crashes without an exception: the calling thread's own context.
void WriteDumpHere(DWORD code) {
  CONTEXT context{};
  RtlCaptureContext(&context);
  EXCEPTION_RECORD record{};
  record.ExceptionCode = code;
#if defined(_M_X64)
  record.ExceptionAddress = reinterpret_cast<void *>(context.Rip);
#elif defined(_M_ARM64)
  record.ExceptionAddress = reinterpret_cast<void *>(context.Pc);
#endif
  EXCEPTION_POINTERS pointers{&record, &context};
  WriteDump(&pointers);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS *pointers) {
  WriteDump(pointers);
  return g_previous_filter ? g_previous_filter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

void OnAbort(int) {
  WriteDumpHere(kCodeAbort);
}

void OnTerminate() {
  WriteDumpHere(kCodeTerminate);
  std::abort();
}

// Both default to terminating the process; keep that after the dump.
void OnInvalidParameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t) {
  WriteDumpHere(kCodeInvalidParameter);
  std::abort();
}

void OnPureCall() {
  WriteDumpHere(kCodePureCall);
  std::abort();
}

void Initialize() {
  if (const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
    g_write_dump =
        reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
  }
  const DWORD length = GetModuleFileNameW(nullptr, g_dump_dir, MAX_PATH);
  if (length == 0 || length >= MAX_PATH)
    return;
  if (wchar_t *slash = std::wcsrchr(g_dump_dir, L'\\'))
    *slash = L'\0';
  if (wcscat_s(g_dump_dir, L"\\crashdumps") != 0)
    return;
  CreateDirectoryW(g_dump_dir, nullptr);

  g_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (const HANDLE thread = CreateThread(nullptr, 256 * 1024, DumpThread, nullptr, 0, nullptr))
    CloseHandle(thread);

  std::signal(SIGABRT, OnAbort);
  std::set_terminate(OnTerminate);
  _set_invalid_parameter_handler(OnInvalidParameter);
  _set_purecall_handler(OnPureCall);
}

} // namespace

void InstallCrashDumps() {
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    Initialize();
  }
  const LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(CrashFilter);
  if (previous != CrashFilter)
    g_previous_filter = previous;
}

} // namespace renut::crash

#else

namespace renut::crash {

void InstallCrashDumps() {}

} // namespace renut::crash

#endif
