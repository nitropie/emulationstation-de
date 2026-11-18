//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  UiTestScript.cpp
//
//  Parser for the native UI test scenario language.
//

#include "UiTestScript.h"

#include <cstdlib>
#include <sstream>

namespace
{
    struct Token {
        std::string value;
        // True when the token was quoted, so callers can tell "text" from text.
        bool quoted {false};
    };

    bool isSpace(const char character)
    {
        return character == ' ' || character == '\t' || character == '\r';
    }

    // Splits one line into tokens. A quoted token may contain spaces and the escapes \" and \\.
    bool tokenize(const std::string& line, std::vector<Token>& tokens, std::string& error)
    {
        std::size_t index {0};
        while (index < line.size()) {
            while (index < line.size() && isSpace(line[index]))
                ++index;

            if (index >= line.size())
                break;

            if (line[index] == '#')
                break;

            Token token;
            if (line[index] == '"') {
                token.quoted = true;
                ++index;
                bool closed {false};
                while (index < line.size()) {
                    const char character {line[index]};
                    if (character == '\\' && index + 1 < line.size()) {
                        const char escaped {line[index + 1]};
                        if (escaped == '"' || escaped == '\\') {
                            token.value.push_back(escaped);
                            index += 2;
                            continue;
                        }

                        token.value.push_back(character);
                        ++index;
                        continue;
                    }

                    if (character == '"') {
                        closed = true;
                        ++index;
                        break;
                    }

                    token.value.push_back(character);
                    ++index;
                }

                if (!closed) {
                    error = "unterminated quoted argument";
                    return false;
                }
            }
            else {
                while (index < line.size() && !isSpace(line[index]))
                    token.value.push_back(line[index++]);
            }

            tokens.emplace_back(std::move(token));
        }

        return true;
    }

    bool parseFrames(const Token& token, int& frames, std::string& error)
    {
        if (token.quoted || token.value.empty()) {
            error = "frame budget must be a number";
            return false;
        }

        char* end {nullptr};
        const long value {std::strtol(token.value.c_str(), &end, 10)};
        if (end == token.value.c_str() || *end != '\0') {
            error = "frame budget \"" + token.value + "\" is not a number";
            return false;
        }

        if (value < 1 || value > UiTest::sMaxFrames) {
            error = "frame budget " + token.value + " is outside 1.." +
                    std::to_string(UiTest::sMaxFrames);
            return false;
        }

        frames = static_cast<int>(value);
        return true;
    }

    bool parsePress(const Token& token, bool& pressed, std::string& error)
    {
        if (token.quoted) {
            error = "expected down or up";
            return false;
        }

        if (token.value == "down")
            pressed = true;
        else if (token.value == "up")
            pressed = false;
        else {
            error = "expected down or up, got \"" + token.value + "\"";
            return false;
        }

        return true;
    }
} // namespace

bool UiTest::parseScript(const std::string& script, std::vector<Step>& steps, std::string& error)
{
    steps.clear();
    error.clear();

    std::istringstream stream {script};
    std::string line;
    int lineNumber {0};

    while (std::getline(stream, line)) {
        ++lineNumber;

        std::vector<Token> tokens;
        std::string tokenError;
        if (!tokenize(line, tokens, tokenError)) {
            error = "line " + std::to_string(lineNumber) + ": " + tokenError;
            steps.clear();
            return false;
        }

        if (tokens.empty())
            continue;

        const std::string& verb {tokens[0].value};
        if (tokens[0].quoted) {
            error = "line " + std::to_string(lineNumber) + ": the verb cannot be quoted";
            steps.clear();
            return false;
        }

        Step step;
        step.line = lineNumber;

        auto fail = [&](const std::string& message) {
            error = "line " + std::to_string(lineNumber) + ": " + message;
            steps.clear();
            return false;
        };

        if (verb == "wait-view") {
            step.verb = Verb::waitView;
            if (tokens.size() != 3)
                return fail("wait-view takes a view name and a frame budget");
            if (tokens[1].quoted ||
                (tokens[1].value != "system-view" && tokens[1].value != "gamelist"))
                return fail("wait-view expects system-view or gamelist");
            step.text = tokens[1].value;
            if (!parseFrames(tokens[2], step.frames, error))
                return fail(error);
        }
        else if (verb == "wait-frames") {
            step.verb = Verb::waitFrames;
            if (tokens.size() != 2)
                return fail("wait-frames takes one argument");
            if (!parseFrames(tokens[1], step.frames, error))
                return fail(error);
        }
        else if (verb == "wait-text") {
            step.verb = Verb::waitText;
            if (tokens.size() != 3)
                return fail("wait-text takes a string and a frame budget");
            if (!tokens[1].quoted || tokens[1].value.empty())
                return fail("wait-text needs a quoted, non-empty string");
            step.text = tokens[1].value;
            if (!parseFrames(tokens[2], step.frames, error))
                return fail(error);
        }
        else if (verb == "assert-text" || verb == "assert-absent" || verb == "text") {
            step.verb = verb == "assert-text"   ? Verb::assertText :
                        verb == "assert-absent" ? Verb::assertAbsent :
                                                  Verb::text;
            if (tokens.size() != 2)
                return fail(verb + " takes one argument");
            if (!tokens[1].quoted || tokens[1].value.empty())
                return fail(verb + " needs a quoted, non-empty string");
            step.text = tokens[1].value;
        }
        else if (verb == "capture") {
            step.verb = Verb::capture;
            if (tokens.size() != 2)
                return fail("capture takes one name");
            if (tokens[1].value.empty())
                return fail("capture needs a non-empty name");
            step.text = tokens[1].value;
        }
        else if (verb == "key" || verb == "controller") {
            step.verb = verb == "key" ? Verb::key : Verb::controller;
            if (tokens.size() != 3)
                return fail(verb + " takes a name and down or up");
            if (tokens[1].quoted || tokens[1].value.empty())
                return fail(verb + " needs an unquoted, non-empty name");
            step.name = tokens[1].value;
            if (!parsePress(tokens[2], step.pressed, error))
                return fail(error);
        }
        else if (verb == "quit") {
            step.verb = Verb::quit;
            if (tokens.size() != 1)
                return fail("quit takes no arguments");
        }
        else {
            return fail("unknown verb \"" + verb + "\"");
        }

        steps.emplace_back(std::move(step));
    }

    if (steps.empty()) {
        error = "the script contains no steps";
        return false;
    }

    return true;
}
