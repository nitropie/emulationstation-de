//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  FileSystemUtilTests.cpp
//
//  Test cases for the low-level filesystem functions: relative path normalization, the
//  directory boundary of removeCommonPath() and resolveRelativePath(), canonicalization of
//  symlink chains and cycles, and shell escaping.
//

#include "TestHarness.h"

#include "utils/FileSystemUtil.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // One fixture directory per test process, recreated before the first test needs it.
    const fs::path& fixtureRoot()
    {
        static const fs::path root {[] {
            const fs::path path {fs::temp_directory_path() / "es-de-filesystem-util"};
            fs::remove_all(path);
            fs::create_directories(path);
            return path;
        }()};

        return root;
    }

#if !defined(_WIN64)
    // An empty directory that no earlier test has touched.
    fs::path freshDirectory(const std::string& name)
    {
        const fs::path directory {fixtureRoot() / name};
        fs::remove_all(directory);
        fs::create_directories(directory);
        return directory;
    }

    // Runs the escaped text through /bin/sh (popen) and reports whether the shell produced
    // exactly the expected bytes.
    bool shellPrintsExactly(const std::string& escaped, const std::string& expected)
    {
        std::FILE* pipe {popen(("printf %s " + escaped).c_str(), "r")};
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

TEST_CASE(isNormalizedRelativePathAcceptsRelativePaths)
{
    EXPECT(Utils::FileSystem::isNormalizedRelativePath("Game.sfc"));
    EXPECT(Utils::FileSystem::isNormalizedRelativePath("Hacks/Snes Hack.sfc"));
    EXPECT(Utils::FileSystem::isNormalizedRelativePath(".hidden/Game.sfc"));
    EXPECT(Utils::FileSystem::isNormalizedRelativePath("a..b/c."));
}

TEST_CASE(isNormalizedRelativePathRejectsAbsoluteTraversalAndEmptySegments)
{
    EXPECT(!Utils::FileSystem::isNormalizedRelativePath(""));
    EXPECT(!Utils::FileSystem::isNormalizedRelativePath("/Game.sfc"));
    EXPECT(!Utils::FileSystem::isNormalizedRelativePath("Hacks/"));
    EXPECT(!Utils::FileSystem::isNormalizedRelativePath("Hacks//Game.sfc"));
    EXPECT(!Utils::FileSystem::isNormalizedRelativePath("."));
    EXPECT(!Utils::FileSystem::isNormalizedRelativePath(".."));
    EXPECT(!Utils::FileSystem::isNormalizedRelativePath("./Game.sfc"));
    EXPECT(!Utils::FileSystem::isNormalizedRelativePath("../Game.sfc"));
    EXPECT(!Utils::FileSystem::isNormalizedRelativePath("Hacks/../Game.sfc"));
    EXPECT(!Utils::FileSystem::isNormalizedRelativePath(std::string {"a\0b", 3}));
}

TEST_CASE(removeCommonPathRejectsASiblingThatOnlySharesTheStringPrefix)
{
    bool contains {true};
    const std::string relative {
        Utils::FileSystem::removeCommonPath("/roms/snes-other/Game.sfc", "/roms/snes", contains)};

    EXPECT(!contains);
    EXPECT(relative == "/roms/snes-other/Game.sfc");
}

TEST_CASE(removeCommonPathKeepsTheDirectoryBoundaryForANonexistentRoot)
{
    bool contains {false};
    const std::string relative {Utils::FileSystem::removeCommonPath(
        "/nonexistent/roms/snes/Game.sfc", "/nonexistent/roms/snes", contains)};

    EXPECT(contains);
    EXPECT(relative == "Game.sfc");
}

TEST_CASE(removeCommonPathRejectsTraversalInTheRelativePart)
{
    bool contains {true};
    const std::string relative {Utils::FileSystem::removeCommonPath(
        "/roms/snes/../../etc/passwd", "/roms/snes", contains)};

    EXPECT(!contains);
    EXPECT(relative == "/roms/snes/../../etc/passwd");
}

TEST_CASE(removeCommonPathHasNoRelativeFormForTheRootItself)
{
    bool contains {true};
    const std::string relative {
        Utils::FileSystem::removeCommonPath("/roms/snes", "/roms/snes", contains)};

    EXPECT(!contains);
    EXPECT(relative == "/roms/snes");
}

TEST_CASE(removeCommonPathHandlesTheFilesystemRoot)
{
    bool contains {false};
    EXPECT(Utils::FileSystem::removeCommonPath("/roms/snes/Game.sfc", "/", contains) ==
           "roms/snes/Game.sfc");
    EXPECT(contains);

    contains = true;
    EXPECT(Utils::FileSystem::removeCommonPath("/", "/", contains) == "/");
    EXPECT(!contains);
}

TEST_CASE(resolveRelativePathTreatsRelativeToAsADirectory)
{
    // The directory does not exist, so probing it with isDirectory() must not change the answer.
    EXPECT(Utils::FileSystem::resolveRelativePath("./Game.sfc", "/nonexistent/roms/snes", false) ==
           "/nonexistent/roms/snes/Game.sfc");
    EXPECT(Utils::FileSystem::resolveRelativePath("Hacks/Game.sfc", "/nonexistent/roms/snes",
                                                  false) == "Hacks/Game.sfc");
    EXPECT(Utils::FileSystem::resolveRelativePath(".hidden", "/nonexistent/roms/snes", false) ==
           ".hidden");
}

TEST_CASE(resolveRelativePathAvoidsADoubleSeparatorAtTheFilesystemRoot)
{
    EXPECT(Utils::FileSystem::resolveRelativePath("./Game.sfc", "/", false) == "/Game.sfc");
    EXPECT(Utils::FileSystem::resolveRelativePath("./Game.sfc", "/roms/snes/", false) ==
           "/roms/snes/Game.sfc");
}

#if !defined(_WIN64)
TEST_CASE(getCanonicalPathTerminatesOnASymlinkCycle)
{
    const fs::path directory {freshDirectory("canonical-cycle")};
    fs::create_symlink(directory / "b", directory / "a");
    fs::create_symlink(directory / "a", directory / "b");

    const std::string cycle {(directory / "a").string()};
    const std::string canonical {Utils::FileSystem::getCanonicalPath(cycle)};

    // The cycle cannot be resolved, so the path is returned normalized instead of being
    // followed forever. The exact text is the canonical directory plus the unresolved entry.
    EXPECT(!canonical.empty());
    EXPECT(canonical == (fs::canonical(directory) / "a").generic_string());
}

TEST_CASE(getCanonicalPathResolvesASymlinkChain)
{
    const fs::path directory {freshDirectory("canonical-chain")};
    const fs::path target {directory / "target.sfc"};
    std::ofstream {target}.close();
    fs::create_symlink(target, directory / "b");
    fs::create_symlink(directory / "b", directory / "a");

    EXPECT(Utils::FileSystem::getCanonicalPath((directory / "a").string()) ==
           fs::canonical(target).generic_string());
}

TEST_CASE(resolveSymlinkReturnsTheStoredTargetText)
{
    const fs::path directory {freshDirectory("symlink-target")};
    fs::create_symlink("relative/target.sfc", directory / "link.sfc");

    EXPECT(Utils::FileSystem::resolveSymlink((directory / "link.sfc").string()) ==
           "relative/target.sfc");
    EXPECT(Utils::FileSystem::resolveSymlink((directory / "missing").string()).empty());
}

TEST_CASE(getEscapedPathRoundTripsThroughTheShell)
{
    // One name per protected character. A backtick starts a command substitution, while '#'
    // and '~' are only special at the start of a word, so those characters sit exactly where
    // the shell would act on them. Every name is bare, never a full path.
    const std::vector<std::string> names {"a$(echo INJECTED)b",
                                          "a)b",
                                          "a|b",
                                          "a;b",
                                          "a&b",
                                          "a'b",
                                          "a\"b",
                                          "a b",
                                          "a`echo INJECTED`b",
                                          "#x",
                                          "~",
                                          "~root",
                                          "a\tb",
                                          "a\nb",
                                          "a\rb"};

    for (const std::string& name : names) {
        const std::string escaped {Utils::FileSystem::getEscapedPath(name)};
        const bool exact {shellPrintsExactly(escaped, name)};

        if (!exact)
            std::printf("    did not round-trip: \"%s\" escaped as \"%s\"\n", name.c_str(),
                        escaped.c_str());

        EXPECT(exact);
        EXPECT(escaped != name);
    }

    // A raw carriage return is still an ordinary word character to the shell, so its case
    // cannot detect lost quoting on its own. Pin the quoted text of both control characters.
    EXPECT(Utils::FileSystem::getEscapedPath("a\nb") == "a'\n'b");
    EXPECT(Utils::FileSystem::getEscapedPath("a\rb") == "a'\r'b");

    // The full path form stays covered: separators and the fixture prefix are protected too.
    const std::string path {(fixtureRoot() / "shell" / "a b|c#d.sfc").string()};
    const std::string escapedPath {Utils::FileSystem::getEscapedPath(path)};
    EXPECT(escapedPath != path);
    EXPECT(shellPrintsExactly(escapedPath, path));
}
#endif
