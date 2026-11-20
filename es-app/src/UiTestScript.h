//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  UiTestScript.h
//
//  Parser for the native UI test scenario language.
//
//  The language can only inject input events, inspect the scene and quit. It has no shell,
//  file-write or network primitive, and the parser rejects anything it does not understand
//  instead of ignoring it.
//

#ifndef ES_APP_UI_TEST_SCRIPT_H
#define ES_APP_UI_TEST_SCRIPT_H

#include <string>
#include <vector>

namespace UiTest
{
    enum class Verb {
        waitFrames,
        waitText,
        waitView,
        assertText,
        assertAbsent,
        assertCursor,
        capture,
        key,
        controller,
        text,
        expectExit,
        quit
    };

    struct Step {
        Verb verb {Verb::quit};
        // Text to wait for, assert, type, use as a capture name or name a view mode.
        std::string text;
        // Key or controller button name for the key and controller verbs.
        std::string name;
        // True for a key or button press, false for a release.
        bool pressed {false};
        // Frame budget for wait-frames and wait-text.
        int frames {0};
        // Source line, used in failure messages.
        int line {0};
    };

    // Parses a scenario script. Returns false and sets error to a single-line message with the
    // offending line number when the script is malformed.
    bool parseScript(const std::string& script, std::vector<Step>& steps, std::string& error);

    // The largest accepted frame budget or frame count.
    constexpr int sMaxFrames {100000};
} // namespace UiTest

#endif // ES_APP_UI_TEST_SCRIPT_H
