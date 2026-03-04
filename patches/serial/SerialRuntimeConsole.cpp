#include "SerialRuntimeConsole.h"

namespace padre {

SerialRuntimeConsole::SerialRuntimeConsole(RuntimeConfigEntry* entries,
                                           size_t entry_count,
                                           Print& out)
    : entries_(entries), entry_count_(entry_count), out_(&out) {}

bool SerialRuntimeConsole::handleLine(const String& line) {
  if (!out_) return false;

  size_t pos = 0;
  const String command = nextToken(line, pos);
  if (command.length() == 0) return false;

  if (command == "help") {
    out_->println("Commands: help, debug <on|off|toggle|status>, set <key> <value>, get <key>, list");
    return true;
  }

  if (command == "debug") {
    const String arg = nextToken(line, pos);
    if (arg == "toggle") {
      debug_enabled_ = !debug_enabled_;
      out_->printf("debug=%s\n", debug_enabled_ ? "on" : "off");
      return true;
    }
    if (arg == "status" || arg.length() == 0) {
      out_->printf("debug=%s\n", debug_enabled_ ? "on" : "off");
      return true;
    }

    bool value = false;
    if (!parseBool(arg, &value)) {
      out_->println("debug: expected on/off");
      return false;
    }
    debug_enabled_ = value;
    out_->printf("debug=%s\n", debug_enabled_ ? "on" : "off");
    return true;
  }

  if (command == "set") {
    const String key = nextToken(line, pos);
    const String value_token = nextToken(line, pos);

    if (key.length() == 0 || value_token.length() == 0) {
      out_->println("set: usage set <key> <value>");
      return false;
    }

    RuntimeConfigEntry* entry = findEntry(key);
    if (!entry || !entry->value) {
      out_->printf("set: unknown key '%s'\n", key.c_str());
      return false;
    }

    float value = value_token.toFloat();
    if (value < entry->min) value = entry->min;
    if (value > entry->max) value = entry->max;
    *entry->value = value;

    out_->printf("%s=%.3f\n", entry->key, static_cast<double>(*entry->value));
    return true;
  }

  if (command == "get") {
    const String key = nextToken(line, pos);
    RuntimeConfigEntry* entry = findEntry(key);
    if (!entry || !entry->value) {
      out_->printf("get: unknown key '%s'\n", key.c_str());
      return false;
    }
    out_->printf("%s=%.3f\n", entry->key, static_cast<double>(*entry->value));
    return true;
  }

  if (command == "list") {
    for (size_t i = 0; i < entry_count_; ++i) {
      RuntimeConfigEntry& entry = entries_[i];
      if (!entry.key || !entry.value) continue;
      out_->printf("%s=%.3f [%.3f..%.3f]\n", entry.key,
                   static_cast<double>(*entry.value), static_cast<double>(entry.min),
                   static_cast<double>(entry.max));
    }
    return true;
  }

  out_->printf("unknown command: %s\n", command.c_str());
  return false;
}

bool SerialRuntimeConsole::debugEnabled() const { return debug_enabled_; }

void SerialRuntimeConsole::setDebugEnabled(bool enabled) { debug_enabled_ = enabled; }

RuntimeConfigEntry* SerialRuntimeConsole::findEntry(const String& key) {
  for (size_t i = 0; i < entry_count_; ++i) {
    RuntimeConfigEntry& entry = entries_[i];
    if (!entry.key) continue;
    if (key.equalsIgnoreCase(entry.key)) return &entry;
  }
  return nullptr;
}

String SerialRuntimeConsole::nextToken(const String& line, size_t& pos) {
  while (pos < static_cast<size_t>(line.length()) &&
         isspace(static_cast<unsigned char>(line[pos]))) {
    ++pos;
  }

  const size_t start = pos;
  while (pos < static_cast<size_t>(line.length()) &&
         !isspace(static_cast<unsigned char>(line[pos]))) {
    ++pos;
  }
  if (start >= static_cast<size_t>(line.length())) return String();
  return line.substring(start, pos);
}

bool SerialRuntimeConsole::parseBool(const String& token, bool* out) {
  if (!out) return false;
  if (token.equalsIgnoreCase("on") || token == "1" || token.equalsIgnoreCase("true")) {
    *out = true;
    return true;
  }
  if (token.equalsIgnoreCase("off") || token == "0" || token.equalsIgnoreCase("false")) {
    *out = false;
    return true;
  }
  return false;
}

}  // namespace padre
