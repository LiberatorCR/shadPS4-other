// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace Common {

template <size_t N>
struct DiagnosticEnvName {
    char value[N];
    constexpr DiagnosticEnvName(const char (&name)[N]) {
        for (size_t i = 0; i < N; ++i) {
            value[i] = name[i];
        }
    }
};

// Diagnostic settings are launch-time settings, not live UI preferences. Each name is
// read once on first use. Own the value so subsequent environment changes cannot
// invalidate a pointer, and use C++ static initialization for cross-thread safety.
// Preserve getenv semantics: an unset name is null; a present empty value is not.
template <DiagnosticEnvName Name>
const char* DiagnosticEnv() {
    static const auto value = []() -> std::optional<std::string> {
        if (const char* setting = std::getenv(Name.value)) {
            return std::string{setting};
        }
        return std::nullopt;
    }();
    return value ? value->c_str() : nullptr;
}

} // namespace Common
