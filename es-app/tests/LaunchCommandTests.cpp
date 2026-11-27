//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  LaunchCommandTests.cpp
//
//  Test cases for the launch-command variable substitution: ordinary names keep their bytes, the
//  file variables are shell-escaped on Unix, and hostile names still reach the command as exactly
//  one word each.
//

#include "TestHarness.h"

#include "FileData.h"
#include "utils/FileSystemUtil.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
#if !defined(_WIN64)
    // Runs the expanded text through /bin/sh and reports whether the shell produced exactly the
    // expected bytes.
    bool shellPrintsExactly(const std::string& expanded, const std::string& expected)
    {
        std::FILE* pipe {popen(("printf %s " + expanded).c_str(), "r")};
        if (pipe == nullptr)
            return false;

        std::string output;
        char buffer[512];
        std::size_t read {0};
        while ((read = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0)
            output.append(buffer, read);

        if (pclose(pipe) != 0)
            return false;

        return output == expected;
    }
#endif
} // namespace

TEST_CASE(launchVariablesMatchThePreRefactorExpressionsForOrdinaryNames)
{
    const std::string path {"/roms/snes/Game.sfc"};
    const std::string escapedPath {Utils::FileSystem::getEscapedPath(path)};

    // The expressions the pre-refactor inline substitutions used, kept verbatim here. For an
    // ordinary name the extracted functions have to reproduce them byte for byte.
    const std::string oldBaseName {Utils::FileSystem::getStem(path)};
    const std::string oldFileName {oldBaseName + Utils::FileSystem::getExtension(escapedPath)};
    const std::string oldGameDirectory {
        Utils::FileSystem::getEscapedPath(Utils::FileSystem::getParent(
            Utils::String::replace(escapedPath, "\\", "")))};
    const std::string oldGameDirectoryRaw {Utils::FileSystem::getParent(
        Utils::String::replace(escapedPath, "\\", ""))};

    std::string command {"%ROM% %BASENAME% %FILENAME% %GAMEDIR% %GAMEDIRRAW%"};
    command = FileDataDetail::substituteFileVariables(command, escapedPath, oldBaseName,
                                                      oldFileName, true);
    command = FileDataDetail::substituteGameDirectoryVariables(
        command, Utils::FileSystem::getParent(path), false);

    const std::string expected {escapedPath + " " + oldBaseName + " " + oldFileName + " " +
                                oldGameDirectory + " " + oldGameDirectoryRaw};
    EXPECT(command == expected);
}

TEST_CASE(launchFileVariablesKeepOrdinaryCommandBytes)
{
    // A name without shell metacharacters. These are the exact bytes the pre-refactor inline
    // substitutions produced, because getEscapedPath() is the identity for such a path.
    const std::string rom {"/roms/snes/Game.sfc"};
    const std::string baseName {"/roms/snes/Game"};
    const std::string fileName {"/roms/snes/Game.sfc"};

    EXPECT(FileDataDetail::substituteFileVariables("%ROM%|%BASENAME%|%FILENAME%", rom, baseName,
                                                   fileName, true) ==
           "/roms/snes/Game.sfc|/roms/snes/Game|/roms/snes/Game.sfc");
}

TEST_CASE(launchFileVariablesEscapeNamesWithMetacharacters)
{
    // Spaces and parentheses are escaped for %ROM% as before, and now for %BASENAME% and
    // %FILENAME% too. The pre-refactor code substituted the raw stem and file name there.
    const std::string rom {Utils::FileSystem::getEscapedPath("/roms/snes/Game (USA).sfc")};
    const std::string baseName {"/roms/snes/Game (USA)"};
    const std::string fileName {"/roms/snes/Game (USA).sfc"};

    EXPECT(FileDataDetail::substituteFileVariables("%ROM%", rom, baseName, fileName, true) ==
           "/roms/snes/Game\\ \\(USA\\).sfc");
    EXPECT(FileDataDetail::substituteFileVariables("%BASENAME%", rom, baseName, fileName, true) ==
           "/roms/snes/Game\\ \\(USA\\)");
    EXPECT(FileDataDetail::substituteFileVariables("%FILENAME%", rom, baseName, fileName, true) ==
           "/roms/snes/Game\\ \\(USA\\).sfc");

    // Windows passes the names verbatim, because its command line is not run through a POSIX shell.
    EXPECT(FileDataDetail::substituteFileVariables("%BASENAME%", rom, baseName, fileName, false) ==
           "/roms/snes/Game (USA)");
    EXPECT(FileDataDetail::substituteFileVariables("%FILENAME%", rom, baseName, fileName, false) ==
           "/roms/snes/Game (USA).sfc");
}

TEST_CASE(launchGameDirectoryVariablesEscapeOnlyTheDirectory)
{
    const std::string raw {"/roms/my snes"};

    EXPECT(FileDataDetail::substituteGameDirectoryVariables("%GAMEDIR%", raw, false) ==
           "/roms/my\\ snes");
    EXPECT(FileDataDetail::substituteGameDirectoryVariables("%GAMEDIRRAW%", raw, false) ==
           "/roms/my snes");

    // The Windows launch path asks for backslash separators in the raw value.
    EXPECT(FileDataDetail::substituteGameDirectoryVariables("%GAMEDIRRAW%", "/roms/snes", true) ==
           "\\roms\\snes");
}

#if !defined(_WIN64)
TEST_CASE(launchVariablesSurviveTheShellForHostileNames)
{
    const std::vector<std::string> names {"a;touch x$(id).sfc",
                                          "a`touch x`b.sfc",
                                          "a 'quoted' name.sfc",
                                          "a\tname.sfc",
                                          "a\nname.sfc"};

    for (const std::string& name : names) {
        const std::string path {"/roms/snes/" + name};
        const std::string parent {Utils::FileSystem::getParent(path)};
        const std::string baseName {Utils::FileSystem::getStem(path)};
        const std::string fileName {baseName + Utils::FileSystem::getExtension(path)};

        const auto expectRoundTrip = [&name](const char* variable, const std::string& expanded,
                                             const std::string& expected) {
            const bool exact {shellPrintsExactly(expanded, expected)};
            if (!exact)
                std::printf("    %s did not round-trip for \"%s\"\n", variable, name.c_str());
            EXPECT(exact);
        };

        expectRoundTrip("%ROM%",
                        FileDataDetail::substituteFileVariables(
                            "%ROM%", Utils::FileSystem::getEscapedPath(path), "", "", true),
                        path);
        expectRoundTrip("%BASENAME%",
                        FileDataDetail::substituteFileVariables("%BASENAME%", "", baseName, "",
                                                                true),
                        baseName);
        expectRoundTrip("%FILENAME%",
                        FileDataDetail::substituteFileVariables("%FILENAME%", "", "", fileName,
                                                                true),
                        fileName);
        expectRoundTrip("%GAMEDIR%",
                        FileDataDetail::substituteGameDirectoryVariables("%GAMEDIR%", parent,
                                                                         false),
                        parent);

        // %GAMEDIRRAW% is deliberately raw, so it is compared byte for byte instead of through a
        // shell that would split it.
        const std::string rawDirectory {
            FileDataDetail::substituteGameDirectoryVariables("%GAMEDIRRAW%", parent, false)};
        if (rawDirectory != parent)
            std::printf("    %%GAMEDIRRAW%% was altered for \"%s\"\n", name.c_str());
        EXPECT(rawDirectory == parent);
    }
}
#endif
