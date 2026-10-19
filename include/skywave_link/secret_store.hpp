#pragma once

// Account passwords (SMTP, POP3, Winlink) are kept out of config.toml.
// With the system store enabled (the app does this at start-up; tests do
// not) they live in the macOS Keychain. Without it - other platforms, or
// tests - they are written to config.toml and the file is made readable
// by its owner only.
#include <optional>
#include <string>

namespace skylink::secrets {

// Opt in to the OS credential store. Off by default so tests never touch
// the user's Keychain.
void useSystemStore(bool on);
// True when secrets go to the OS store (enabled and supported here).
bool systemStoreActive();

// Value stored under `key`, if any.
std::optional<std::string> load(const std::string& key);
// Store `value` under `key`; an empty value removes the entry. False on
// failure (the caller keeps the value in memory and reports it).
bool store(const std::string& key, const std::string& value);

}  // namespace skylink::secrets
