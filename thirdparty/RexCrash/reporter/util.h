/**
 * @file        crashreporter/util.h
 * @brief       String and file helpers shared by the crash reporter UIs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Everything here works in UTF-8 std::string, which is what ImGui
 *              and sentry-native want. Widening happens only at the
 *              Win32 boundary, where std::filesystem and fstream would
 *              otherwise interpret a narrow path in the ANSI code page and
 *              mangle any account name that is not plain ASCII.
 */

#pragma once

#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace reporter {

#if defined(_WIN32)

inline std::wstring Widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int length =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
  return out;
}

inline std::string Narrow(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length,
                      nullptr, nullptr);
  return out;
}

#endif  // _WIN32

/// A UTF-8 path string as a filesystem path, without the ANSI round trip that
/// the narrow std::filesystem::path constructor performs on Windows.
inline std::filesystem::path ToPath(const std::string& utf8) {
#if defined(_WIN32)
  return std::filesystem::path(Widen(utf8));
#else
  return std::filesystem::path(utf8);
#endif
}

inline std::string FromPath(const std::filesystem::path& path) {
#if defined(_WIN32)
  return Narrow(path.wstring());
#else
  return path.string();
#endif
}

inline std::string Trim(const std::string& text) {
  const size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  const size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

inline bool FileExists(const std::string& path) {
  std::error_code ec;
  return !path.empty() && std::filesystem::is_regular_file(ToPath(path), ec);
}

inline std::string DirectoryOf(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

inline std::string ReadTextFile(const std::string& path, size_t max_bytes = 512 * 1024) {
  if (path.empty()) {
    return {};
  }
  std::ifstream file(ToPath(path), std::ios::binary);
  if (!file) {
    return {};
  }
  std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  // Only the tail matters when a log is huge, and it is the part nearest the
  // crash. The full log still rides along as an attachment.
  if (text.size() > max_bytes) {
    text = "[... earlier lines trimmed from this view; the full log is attached ...]\n"
           + text.substr(text.size() - max_bytes);
  }
  return text;
}

inline bool WriteTextFile(const std::string& path, const std::string& text) {
  if (path.empty()) {
    return false;
  }
  std::ofstream file(ToPath(path), std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  file << text;
  return file.good();
}

}  // namespace reporter
