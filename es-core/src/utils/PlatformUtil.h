//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  PlatformUtil.h
//
//  Platform utility functions.
//

#ifndef ES_CORE_UTILS_PLATFORM_UTIL_H
#define ES_CORE_UTILS_PLATFORM_UTIL_H

#include <functional>
#include <string>

#if defined(_WIN64)
#include <windows.h>
#endif

namespace Utils
{
    namespace Platform
    {
        enum QuitMode {
            QUIT = 0,
            REBOOT = 1,
            POWEROFF = 2,
            // The frontend was asked to hand over to another profile. No shutdown command runs;
            // main() returns SWITCH_PROFILE_EXIT_CODE so the session supervisor can tell this
            // apart from a quit, a crash, a reboot and a poweroff.
            SWITCH_PROFILE = 3
        };

        // The dedicated switch outcome. The supervisor's SWITCH_EXIT is 42 and the two
        // constants must match (see docs/profiles.md, "Session supervisor (phase 2)").
        constexpr int SWITCH_PROFILE_EXIT_CODE = 42;

        int runRebootCommand();
        int runPoweroffCommand();
        int runSuspendCommand();

        // Test seam: replaces the real reboot/poweroff/suspend command runner so a test can
        // prove that a profile switch runs no shutdown command. Pass nullptr to restore.
        void setShutdownCommandRunnerForTesting(std::function<int(const std::string&)> runner);

        // Uses UTF-8 for Unix and UTF-16 for Windows.
        int runSystemCommand(const std::string& cmdUtf8, bool blocking);
        int runSystemCommand(const std::wstring& cmdUtf16, bool blocking);

        int launchGameUnix(const std::string& cmdUtf8,
                           const std::string& startDirectory,
                           bool runInBackground);
        int launchGameWindows(const std::wstring& cmdUtf16,
                              const std::wstring& startDirectory,
                              bool runInBackground,
                              bool hideWindow);

        unsigned int getTaskbarState();
        void hideTaskbar();
        void revertTaskbarState(unsigned int& state);

        // Clean, normal shutdown.
        int quitES(QuitMode mode = QuitMode::QUIT);
        void processQuitMode();
        // The process exit status main() must return for the last quit mode.
        int quitExitCode();
        // Scripts still running in the background, used by the switch idle gate.
        int runningCommands();

        // Immediately shut down the application as cleanly as possible.
        void emergencyShutdown();

        static QuitMode sQuitMode = QuitMode::QUIT;
        // This is simply to get rid of a GCC false positive -Wunused-variable compiler warning.
        static QuitMode sQuitModeDummy = sQuitMode;

    } // namespace Platform

} // namespace Utils

#endif // ES_CORE_UTILS_PLATFORM_UTIL_H
