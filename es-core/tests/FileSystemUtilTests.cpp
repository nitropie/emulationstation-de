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
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(_WIN64)
#include <sys/stat.h>
#include <sys/types.h>
#endif

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

// The six helpers follow std::filesystem semantics. Every row of the ticket's breaking-change
// table is asserted here, including the rows that were discovered during characterization.
TEST_CASE(helperEdgeCasesFollowTheFilesystemSemantics)
{
    const fs::path root {fixtureRoot() / "helpers"};
    fs::remove_all(root);
    fs::create_directories(root / "Vol. 2");
    fs::create_directories(root / "Super Mario Bros. 3");
    const std::string helpers {root.string()};

    struct Row {
        std::string input;
        std::string parent;
        std::string fileName;
        std::string stem;
        std::string extension;
    };

    const std::vector<Row> rows {
        {"file", "", "file", "file", ""},
        {"/a", "/", "a", "a", ""},
        {"a/b", "a", "b", "b", ""},
        {"/a/b", "/a", "b", "b", ""},
        {"/a/b/", "/a", "b", "b", ""},
        {"a/b/", "a", "b", "b", ""},
        {"", "", "", "", ""},
        {".", "", ".", ".", ""},
        {"..", "", "..", "..", ""},
        {"/", "/", "", "", ""},
        {"a//b", "a", "b", "b", ""},
        {"a.tar.gz", "", "a.tar.gz", "a.tar", ".gz"},
        {"a..b", "", "a..b", "a.", ".b"},
        {"a.", "", "a.", "a", "."},
        {".bashrc", "", ".bashrc", ".bashrc", ""},
        // A file named just ".sfc" keeps that name as its stem; the old code returned "".
        {".sfc", "", ".sfc", ".sfc", ""},
        {helpers + "/Vol. 2", helpers, "Vol. 2", "Vol", ". 2"},
        {helpers + "/Vol. 2/", helpers, "Vol. 2", "Vol", ". 2"},
        {helpers + "/Super Mario Bros. 3", helpers, "Super Mario Bros. 3", "Super Mario Bros",
         ". 3"},
        {helpers + "/Super Mario Bros. 3/", helpers, "Super Mario Bros. 3", "Super Mario Bros",
         ". 3"},
    };

    for (const Row& row : rows) {
        if (Utils::FileSystem::getParent(row.input) != row.parent)
            std::printf("    parent mismatch for \"%s\"\n", row.input.c_str());

        EXPECT(Utils::FileSystem::getParent(row.input) == row.parent);
        EXPECT(Utils::FileSystem::getFileName(row.input) == row.fileName);
        EXPECT(Utils::FileSystem::getStem(row.input) == row.stem);
        EXPECT(Utils::FileSystem::getExtension(row.input) == row.extension);
    }
}

TEST_CASE(copyFileFollowsTheFilesystemContract)
{
    const fs::path root {fixtureRoot() / "copy"};
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path source {root / "source.txt"};
    const fs::path target {root / "target.txt"};
    {
        std::ofstream file {source, std::ios::binary};
        file << "payload";
    }

    // Success returns true and copies the bytes.
    EXPECT(Utils::FileSystem::copyFile(source.string(), target.string(), false));
    EXPECT(fs::exists(target));
    EXPECT(fs::file_size(target) == 7);

    // The path spelling is normalized before the copy, so a backslash separator works everywhere.
    std::string backslashed {source.string()};
    std::replace(backslashed.begin(), backslashed.end(), '/', '\\');
    const fs::path target2 {root / "target2.txt"};
    EXPECT(Utils::FileSystem::copyFile(backslashed, target2.string(), false));
    EXPECT(fs::exists(target2));

    // A missing source is an error: false, and nothing is created.
    const std::string missing {(root / "missing.txt").string()};
    EXPECT(!Utils::FileSystem::copyFile(missing, (root / "new.txt").string(), false));
    EXPECT(!fs::exists(root / "new.txt"));

    // A destination that is a directory is an error: false.
    EXPECT(!Utils::FileSystem::copyFile(source.string(), root.string(), false));

    // An existing destination without overwrite is an error: false, contents untouched.
    {
        std::ofstream file {target, std::ios::binary | std::ios::trunc};
        file << "old";
    }
    EXPECT(!Utils::FileSystem::copyFile(source.string(), target.string(), false));
    EXPECT(fs::file_size(target) == 3);

    // Overwrite replaces the contents and returns true.
    EXPECT(Utils::FileSystem::copyFile(source.string(), target.string(), true));
    EXPECT(fs::file_size(target) == 7);
}

#if !defined(_WIN64)
TEST_CASE(createDirectoryFollowsTheFilesystemContract)
{
    const fs::path root {fixtureRoot() / "create"};
    fs::remove_all(root);
    fs::create_directories(root);

    // Nested creation makes the parents, and an existing directory is success.
    const fs::path nested {root / "a" / "b" / "c"};
    EXPECT(Utils::FileSystem::createDirectory(nested.string()));
    EXPECT(fs::is_directory(nested));
    EXPECT(Utils::FileSystem::createDirectory(nested.string()));

    // The permissions come from the umask, not from the old hard-coded 0755.
    const mode_t previousUmask {umask(0)};
    umask(027);
    const bool created {Utils::FileSystem::createDirectory((root / "moded").string())};
    umask(previousUmask);

    EXPECT(created);
    struct stat info;
    EXPECT(stat((root / "moded").c_str(), &info) == 0);
    EXPECT((info.st_mode & 0777) == 0750);
}
#endif
