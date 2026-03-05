#pragma once

#include <Arduino.h>

namespace padre {

struct RuntimeConfigEntry {
  const char* key = nullptr;
  float* value = nullptr;
  float min = 0.0f;
  float max = 1.0f;
};

struct RuntimeCommandEntry {
  const char* command = nullptr;
  bool (*handler)(void* ctx, const String& line, Print& out) = nullptr;
  void* ctx = nullptr;
  const char* help = nullptr;
};

class SerialRuntimeConsole {
 public:
  SerialRuntimeConsole(RuntimeConfigEntry* entries,
                       size_t entry_count,
                       Print& out,
                       RuntimeCommandEntry* commands = nullptr,
                       size_t command_count = 0);

  bool handleLine(const String& line);
  bool debugEnabled() const;
  void setDebugEnabled(bool enabled);

 private:
  RuntimeConfigEntry* findEntry(const String& key);
  RuntimeCommandEntry* findCommand(const String& command);
  static String nextToken(const String& line, size_t& pos);
  static bool parseBool(const String& token, bool* out);

  RuntimeConfigEntry* entries_ = nullptr;
  size_t entry_count_ = 0;
  RuntimeCommandEntry* commands_ = nullptr;
  size_t command_count_ = 0;
  Print* out_ = nullptr;
  bool debug_enabled_ = false;
};

}  // namespace padre
