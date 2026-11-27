//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileSwitch.h
//
//  Client side of the session supervisor's switch protocol (design section 9). The launcher
//  hands this process a session nonce, a readiness pipe and a request path; in legacy mode
//  none of that exists and every entry point is a safe no-op.
//
//  This file contains no UI: the menu entry and the chooser belong to 9q0.5.
//

#ifndef ES_APP_PROFILES_PROFILE_SWITCH_H
#define ES_APP_PROFILES_PROFILE_SWITCH_H

#include "profiles/ProfileStateFile.h"

#include <functional>
#include <string>
#include <vector>

namespace ProfileSwitch
{
    // The four flush groups the supervisor re-validates. Each value is "ok", "nothingToDo" or
    // "failure".
    struct FlushReport {
        std::string overlays {"nothingToDo"};
        std::string metadata {"nothingToDo"};
        std::string settings {"nothingToDo"};
        std::string collections {"nothingToDo"};
        std::string reason;

        bool allOk() const;
    };

    // Reads the switch environment and sets FD_CLOEXEC on the readiness descriptor. Must run at
    // the very top of main(), before anything can spawn a process, so emulators never inherit
    // it. Safe to call more than once.
    void initialize();

    // True when the environment names a mode, the nonce and request path are valid and, for
    // profile mode, a profile session resolved. Evaluated lazily because the session is only
    // available after ProfileMode::initialize().
    bool isEnabled();
    bool isChooser();
    const std::string& disabledReason();
    const std::string& nonce();
    // The active profile id, or "chooser" in chooser mode.
    const std::string& sourceId();
    const std::string& requestPath();

    // Writes exactly "READY <nonce> <profileId-or-chooser>\n" once and closes the descriptor.
    // Called after the first completed frame; a write failure is logged, never fatal.
    void acknowledgeReady();

    // Aggregates the registered blockers. Returns false and sets `reason` to the first blocker.
    bool isIdle(std::string& reason);
    void registerBlocker(const std::string& name, std::function<bool()> probe);

    FlushReport flushAll();

    // Profile mode. Refuses with `error` and writes nothing unless everything is idle and every
    // flush group is ok or nothingToDo. On success it writes the request atomically and asks the
    // platform layer to quit with the switch exit code.
    bool requestSwitch(const std::string& targetId, std::string& error);
    // Chooser mode: source "chooser", idle true, every flush group nothingToDo, no flush calls.
    bool requestSelection(const std::string& targetId, std::string& error);

    bool hasRequest();

    // Maps the platform exit status to the process status. A switch whose final flush failed
    // must not report the switch outcome: the request is removed and a failure status returned
    // so the supervisor restarts the same profile.
    int finalizeExitCode(int platformExitCode, bool finalFlushOk);
    // Removes a written request, for example when the final exit flush failed.
    void discardRequest();

    // Test seams.
    void setFileOperationsForTesting(ProfileState::FileOperations& files);
    void setQuitHandlerForTesting(std::function<void()> handler);
    void setRequestIdForTesting(const std::string& requestId);
    // Replaces flushAll() so a test can inject a group failure without relying on file
    // permissions (which root ignores).
    void setFlushReportForTesting(std::function<FlushReport()> provider);
    void resetForTesting();
} // namespace ProfileSwitch

#endif // ES_APP_PROFILES_PROFILE_SWITCH_H
