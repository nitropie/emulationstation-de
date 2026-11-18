//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  UiTestScriptTests.cpp
//
//  Test cases for the native UI scenario parser. Malformed input has to be rejected with a
//  line number, never ignored.
//

#include "TestHarness.h"

#include "UiTestScript.h"

#include <string>
#include <vector>

namespace
{
    bool parses(const std::string& script, std::vector<UiTest::Step>& steps)
    {
        std::string error;
        return UiTest::parseScript(script, steps, error);
    }

    std::string parseError(const std::string& script)
    {
        std::vector<UiTest::Step> steps;
        std::string error;
        if (UiTest::parseScript(script, steps, error))
            return "";

        return error;
    }
} // namespace

TEST_CASE(everyScenarioVerbParses)
{
    const std::string script {"# a comment\n"
                              "wait-frames 5\n"
                              "wait-view gamelist 300\n"
                              "wait-text \"MAIN MENU\" 120\n"
                              "assert-text \"UI SETTINGS\"\n"
                              "assert-absent \"SCRAPER\"\n"
                              "capture menu-open\n"
                              "key escape down\n"
                              "key escape up\n"
                              "controller down down\n"
                              "controller down up\n"
                              "text \"David\"\n"
                              "quit\n"};

    std::vector<UiTest::Step> steps;
    std::string error;
    EXPECT(UiTest::parseScript(script, steps, error));
    EXPECT(steps.size() == 12);

    EXPECT(steps[0].verb == UiTest::Verb::waitFrames && steps[0].frames == 5);
    EXPECT(steps[1].verb == UiTest::Verb::waitView && steps[1].text == "gamelist" &&
           steps[1].frames == 300);
    EXPECT(steps[2].verb == UiTest::Verb::waitText && steps[2].text == "MAIN MENU" &&
           steps[2].frames == 120);
    EXPECT(steps[3].verb == UiTest::Verb::assertText && steps[3].text == "UI SETTINGS");
    EXPECT(steps[4].verb == UiTest::Verb::assertAbsent && steps[4].text == "SCRAPER");
    EXPECT(steps[5].verb == UiTest::Verb::capture && steps[5].text == "menu-open");
    EXPECT(steps[6].verb == UiTest::Verb::key && steps[6].name == "escape" && steps[6].pressed);
    EXPECT(steps[7].verb == UiTest::Verb::key && steps[7].name == "escape" && !steps[7].pressed);
    EXPECT(steps[8].verb == UiTest::Verb::controller && steps[8].name == "down" &&
           steps[8].pressed);
    EXPECT(steps[9].verb == UiTest::Verb::controller && steps[9].name == "down" &&
           !steps[9].pressed);
    EXPECT(steps[10].verb == UiTest::Verb::text && steps[10].text == "David");
    EXPECT(steps[11].verb == UiTest::Verb::quit);
}

TEST_CASE(quotedArgumentsDecodeEscapes)
{
    std::vector<UiTest::Step> steps;
    EXPECT(parses("assert-text \"a \\\"b\\\" \\\\ c\"\nquit\n", steps));
    EXPECT(steps.size() == 2);
    EXPECT(steps[0].text == "a \"b\" \\ c");
}

TEST_CASE(unknownVerbIsRejected)
{
    EXPECT(parseError("open-menu\nquit\n").find("line 1") == 0);
    EXPECT(parseError("open-menu\nquit\n").find("unknown verb") != std::string::npos);
}

TEST_CASE(missingArgumentIsRejected)
{
    EXPECT(parseError("wait-text \"MAIN MENU\"\n").find("line 1") == 0);
    EXPECT(parseError("key escape\nquit\n").find("line 1") == 0);
    EXPECT(parseError("quit now\n").find("line 1") == 0);
}

TEST_CASE(badFrameBudgetIsRejected)
{
    EXPECT(parseError("wait-frames 0\n").find("1..") != std::string::npos);
    EXPECT(parseError("wait-frames -3\n").find("1..") != std::string::npos);
    EXPECT(parseError("wait-frames 100001\n").find("1..") != std::string::npos);
    EXPECT(parseError("wait-frames abc\n").find("not a number") != std::string::npos);
    EXPECT(parseError("wait-text \"MAIN MENU\" 0\n").find("1..") != std::string::npos);
    EXPECT(parseError("wait-text \"MAIN MENU\" many\n").find("not a number") != std::string::npos);
}

TEST_CASE(badViewNameIsRejected)
{
    EXPECT(parseError("wait-view menu 30\n").find("system-view or gamelist") != std::string::npos);
    EXPECT(parseError("wait-view gamelist\n").find("frame budget") != std::string::npos);
    EXPECT(parseError("wait-view gamelist 0\n").find("1..") != std::string::npos);
}

TEST_CASE(unterminatedQuoteIsRejected)
{
    EXPECT(parseError("assert-text \"MAIN MENU\nquit\n").find("unterminated") != std::string::npos);
}

TEST_CASE(trailingArgumentIsRejected)
{
    EXPECT(parseError("assert-text \"MAIN MENU\" extra\n").find("one argument") !=
           std::string::npos);
    EXPECT(parseError("capture a b\n").find("one name") != std::string::npos);
}

TEST_CASE(emptyArgumentIsRejected)
{
    EXPECT(parseError("assert-text \"\"\n").find("non-empty") != std::string::npos);
    EXPECT(parseError("capture \"\"\n").find("non-empty") != std::string::npos);
}

TEST_CASE(badPressStateIsRejected)
{
    EXPECT(parseError("key escape sideways\n").find("down or up") != std::string::npos);
    EXPECT(parseError("controller a pressed\n").find("down or up") != std::string::npos);
}

TEST_CASE(emptyScriptIsRejected)
{
    EXPECT(parseError("# only a comment\n").find("no steps") != std::string::npos);
    EXPECT(parseError("").find("no steps") != std::string::npos);
}

TEST_CASE(scriptWithoutQuitIsAcceptedByTheParser)
{
    // The driver fails a script that ends without quit, but the parser only checks syntax.
    std::vector<UiTest::Step> steps;
    EXPECT(parses("capture only-a-capture\n", steps));
    EXPECT(steps.size() == 1);
    EXPECT(steps[0].verb == UiTest::Verb::capture);
}
