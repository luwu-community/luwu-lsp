#pragma once

#include <string>
#include <unordered_map>
#include <functional>

using ErrorCallback = std::function<void(const std::string& message)>;

void registerFastFlags(std::unordered_map<std::string, std::string>& fastFlags, ErrorCallback onError, ErrorCallback onWarning);

/// Register FFlags but emit errors straight to stderr
void registerFastFlagsCLI(std::unordered_map<std::string, std::string>& fastFlags);

/// Enable Luwu's own language feature flags (`Luwu`-prefixed, plus the class `DebugLuau` flags),
/// or disable them when the user asked for Luau compatibility mode
void applyLuwuFlags(bool luauCompatibilityMode);

/// Whether Luwu's own language features are enabled, i.e. Luau compatibility mode is off
bool luwuFeaturesEnabled();

void applyRequiredFlags();
