// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <optional>

namespace chatterino {

/// The running Flatpak, as described by /.flatpak-info
struct FlatpakInstance {
    QString appId;
    QString branch;
    QString commit;
    QString instanceId;
    /// Whether it's installed for all users rather than only this one
    bool system = false;
};

std::optional<FlatpakInstance> readFlatpakInstance();

/// Has the host start the Flatpak again once this instance is gone
///
/// Processes started inside the sandbox die with this instance, so the
/// restart has to come from the host.
bool startFlatpakRestart(const FlatpakInstance &instance);

/// Quits, and starts JilChat Chatterino again once this instance is gone
///
/// Returns false if no restart could be set up, nothing happens then.
bool restartApplication();

/// Starts the instance restartApplication asked for, if it did
///
/// Call this right before exiting, once everything has been saved.
void startPendingRestart();

}  // namespace chatterino
