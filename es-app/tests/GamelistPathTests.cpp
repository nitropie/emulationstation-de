//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GamelistPathTests.cpp
//
//  Test cases for the path hardening of the gamelist loader and of the directory scan: entries
//  outside the system root are rejected, symlink cycles and broken symlinks are skipped while
//  links to sibling directories still work, and an update finds an existing entry written as a
//  relative, absolute or symlinked path instead of appending a duplicate.
//

#include "TestHarness.h"
#include "TestEnvironment.h"

#include "FileData.h"
#include "GamelistFileParser.h"
#include "Settings.h"
#include "SystemData.h"

#include <pugixml.hpp>

#include <algorithm>
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
            const fs::path path {fs::temp_directory_path() / "es-de-gamelist-paths"};
            fs::remove_all(path);
            fs::create_directories(path);
            return path;
        }()};

        return root;
    }

    void writeFile(const fs::path& path, const std::string& contents)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file {path, std::ios::binary | std::ios::trunc};
        file << contents;
    }

    fs::path gamelistPath(const std::string& system)
    {
        return TestEnvironment::appDataDirectory() / "gamelists" / system / "gamelist.xml";
    }

    // The settings every case relies on, set before the SystemData is constructed.
    void setTestSettings(const bool parseGamelistOnly, const bool ignoreGamelist)
    {
        Settings::getInstance()->setString("SaveGamelistsMode", "never");
        Settings::getInstance()->setBool("ParseGamelistOnly", parseGamelistOnly);
        Settings::getInstance()->setBool("IgnoreGamelist", ignoreGamelist);
        Settings::getInstance()->setBool("ShowHiddenGames", true);
        Settings::getInstance()->setBool("ShowHiddenFiles", true);
    }

    SystemEnvironmentData* environment(const fs::path& roms)
    {
        SystemEnvironmentData* env {new SystemEnvironmentData};
        env->mStartPath = roms.string();
        env->mSearchExtensions = {".sfc"};
        env->mLaunchCommands = {{"%EMULATOR% %ROM%", "Snes9x - Current"}};

        return env;
    }

    // Every path of `type` in the tree, relative to the system root and sorted. A path outside the
    // root is reported with a leading marker so it cannot pass as a relative one.
    std::vector<std::string> relativePaths(const SystemData& system, const FileType type)
    {
        std::vector<std::string> paths;

        for (const FileData* node : system.getRootFolder()->getChildrenRecursive()) {
            if (node->getType() != type)
                continue;

            bool contains {false};
            const std::string relative {Utils::FileSystem::removeCommonPath(
                node->getPath(), system.getStartPath(), contains)};

            paths.emplace_back(contains ? relative : ("!" + node->getPath()));
        }

        std::sort(paths.begin(), paths.end());
        return paths;
    }

    FileData* findNode(SystemData& system, const std::string& relativePath)
    {
        for (FileData* node : system.getRootFolder()->getChildrenRecursive()) {
            if (node->getType() != GAME)
                continue;

            bool contains {false};
            const std::string relative {Utils::FileSystem::removeCommonPath(
                node->getPath(), system.getStartPath(), contains)};

            if (contains && relative == relativePath)
                return node;
        }

        return nullptr;
    }
} // namespace

TEST_CASE(gamelistEntriesOutsideTheSystemRootAreRejected)
{
    const fs::path root {fixtureRoot() / "parse-only"};
    fs::remove_all(root);
    const fs::path roms {root / "roms" / "snes"};
    writeFile(roms / "Good Game.sfc", "");
    writeFile(roms / "Hacks" / "Snes Hack.sfc", "");
    writeFile(root / "roms" / "snes-other" / "Game.sfc", "");

    // ParseGamelistOnly makes the loader trust the file, so the entries below are not filtered
    // by an existence check before they reach the root-boundary check.
    const std::string absoluteOutside {(roms / ".." / "snes-other" / "Game.sfc").string()};
    writeFile(gamelistPath("parse"),
              "<?xml version=\"1.0\"?>\n"
              "<gameList>\n"
              "\t<game><path>./Good Game.sfc</path><name>Good Game</name></game>\n"
              "\t<game><path>./Hacks/Snes Hack.sfc</path><name>Snes Hack</name></game>\n"
              "\t<game><path>./../outside.sfc</path><name>Outside</name></game>\n"
              "\t<game><path>" +
                  absoluteOutside +
                  "</path><name>Sibling</name></game>\n"
                  "</gameList>\n");

    setTestSettings(true, false);

    SystemData system {"parse", "Parse", "Parse", environment(roms), "snes", false, false};

    // The exact sets prove both directions: the valid entries are present and neither the
    // traversal entry nor the absolute sibling entry produced a node.
    const std::vector<std::string> expectedGames {"Good Game.sfc", "Hacks/Snes Hack.sfc"};
    const std::vector<std::string> expectedFolders {"Hacks"};
    EXPECT(relativePaths(system, GAME) == expectedGames);
    EXPECT(relativePaths(system, FOLDER) == expectedFolders);
}

TEST_CASE(displayNamesAndExtensionMatchingFollowTheFilesystemSemantics)
{
    const fs::path root {fixtureRoot() / "names"};
    fs::remove_all(root);
    const fs::path roms {root / "roms" / "snes"};
    writeFile(roms / "Vol. 2" / "Inside.sfc", "");
    writeFile(roms / "Extensionless", "");
    writeFile(roms / "Game.m3u" / "Game.m3u", "");
    writeFile(roms / "Dr. Mario.nes", "");
    writeFile(roms / ".sfc", "");

    setTestSettings(false, true);

    SystemEnvironmentData* env {new SystemEnvironmentData};
    env->mStartPath = roms.string();
    env->mSearchExtensions = {".sfc", ".m3u", ".nes", "."};
    env->mLaunchCommands = {{"%EMULATOR% %ROM%", "Snes9x - Current"}};

    SystemData system {"names", "Names", "Names", env, "snes", false, false};

    const auto node = [&system](const std::string& relativePath) -> FileData* {
        for (FileData* candidate : system.getRootFolder()->getChildrenRecursive()) {
            bool contains {false};
            const std::string relative {Utils::FileSystem::removeCommonPath(
                candidate->getPath(), system.getStartPath(), contains)};
            if (contains && relative == relativePath)
                return candidate;
        }

        return nullptr;
    };

    // A folder's display name is its whole name: the dot in "Vol. 2" is not an extension.
    FileData* folder {node("Vol. 2")};
    EXPECT(folder != nullptr);
    if (folder != nullptr) {
        EXPECT(folder->getType() == FOLDER);
        EXPECT(folder->getDisplayName() == "Vol. 2");
        EXPECT(folder->getName() == "Vol. 2");
    }

    // An extensionless file is picked up when the system lists the "." token.
    FileData* extensionless {node("Extensionless")};
    EXPECT(extensionless != nullptr);
    if (extensionless != nullptr) {
        EXPECT(extensionless->getType() == GAME);
        EXPECT(extensionless->getName() == "Extensionless");
    }

    // A directory interpreted as a file keeps its whole name for media lookups, while the
    // metadata name is the stem.
    FileData* directoryGame {node("Game.m3u")};
    EXPECT(directoryGame != nullptr);
    if (directoryGame != nullptr) {
        EXPECT(directoryGame->getType() == GAME);
        EXPECT(directoryGame->getName() == "Game");
        EXPECT(directoryGame->getDisplayName() == "Game.m3u");
    }

    // A game name keeps its dots and loses only the extension.
    FileData* dotted {node("Dr. Mario.nes")};
    EXPECT(dotted != nullptr);
    if (dotted != nullptr) {
        EXPECT(dotted->getType() == GAME);
        EXPECT(dotted->getName() == "Dr. Mario");
    }

    // A file named only ".sfc" keeps that name as its stem instead of being skipped as nameless.
    FileData* dotFile {node(".sfc")};
    EXPECT(dotFile != nullptr);
    if (dotFile != nullptr) {
        EXPECT(dotFile->getType() == GAME);
        EXPECT(dotFile->getName() == ".sfc");
    }
}

TEST_CASE(updateGamelistKeepsADirectoryAsGameEntry)
{
    const fs::path root {fixtureRoot() / "dirgame"};
    fs::remove_all(root);
    const fs::path roms {root / "roms" / "snes"};
    writeFile(roms / "Game.m3u" / "Game.m3u", "");

    setTestSettings(false, false);

    SystemEnvironmentData* env {new SystemEnvironmentData};
    env->mStartPath = roms.string();
    env->mSearchExtensions = {".m3u"};
    env->mLaunchCommands = {{"%EMULATOR% %ROM%", "Snes9x - Current"}};

    SystemData system {"dirgame", "Dirgame", "Dirgame", env, "snes", false, false};

    FileData* game {nullptr};
    for (FileData* candidate : system.getRootFolder()->getChildrenRecursive()) {
        if (candidate->getPath() == (roms / "Game.m3u").string())
            game = candidate;
    }
    EXPECT(game != nullptr);
    if (game == nullptr)
        return;

    EXPECT(game->getType() == GAME);
    EXPECT(game->getName() == "Game");
    EXPECT(game->getDisplayName() == "Game.m3u");
    game->metadata.set("playcount", "3");

    GamelistFileParser::updateGamelist(&system);

    const fs::path gamelist {
        TestEnvironment::appDataDirectory() / "gamelists" / "dirgame" / "gamelist.xml"};
    pugi::xml_document document;
    EXPECT(document.load_file(gamelist.string().c_str()));

    // The entry's name differs from its display name, so the writer must not prune the node.
    pugi::xml_node entry;
    for (pugi::xml_node node {document.child("gameList").child("game")}; node;
         node = node.next_sibling("game")) {
        if (std::string {node.child("path").text().get()} == "./Game.m3u")
            entry = node;
    }

    EXPECT(entry);
    if (entry) {
        EXPECT(std::string {entry.child("name").text().get()} == "Game");
        EXPECT(std::string {entry.child("playcount").text().get()} == "3");
    }
}

#if !defined(_WIN64)
TEST_CASE(populateFolderSkipsSymlinkCyclesAndKeepsSiblingAliases)
{
    const fs::path root {fixtureRoot() / "cycles"};
    fs::remove_all(root);
    const fs::path roms {root / "roms" / "snes"};
    fs::create_directories(roms / "a");
    writeFile(roms / "Good Game.sfc", "");
    writeFile(roms / "Hacks" / "Snes Hack.sfc", "");

    // A direct loop back to the system root, a two-hop loop below a real directory, and a link to
    // a sibling directory that is not an ancestor of itself.
    fs::create_symlink(".", roms / "loop");
    fs::create_symlink("..", roms / "a" / "b");
    fs::create_symlink("Hacks", roms / "alias");

    setTestSettings(false, true);

    // The constructor runs the scan; with the ancestor tracking missing it never returns.
    SystemData system {"cycle", "Cycle", "Cycle", environment(roms), "snes", false, false};

    const std::vector<std::string> expectedGames {"Good Game.sfc", "Hacks/Snes Hack.sfc",
                                                  "alias/Snes Hack.sfc"};
    const std::vector<std::string> expectedFolders {"Hacks", "alias"};
    EXPECT(relativePaths(system, GAME) == expectedGames);
    EXPECT(relativePaths(system, FOLDER) == expectedFolders);
}

TEST_CASE(updateGamelistReplacesEntriesWrittenInAnyPathForm)
{
    const fs::path root {fixtureRoot() / "update-index"};
    fs::remove_all(root);
    const fs::path roms {root / "roms" / "snes"};
    writeFile(roms / "First Game.sfc", "");
    writeFile(roms / "Second Game.sfc", "");
    writeFile(roms / "Hacks" / "Snes Hack.sfc", "");
    fs::create_symlink("snes", root / "roms" / "snes-link");

    const std::string absoluteSecond {(roms / "Second Game.sfc").string()};
    const std::string symlinkedThird {
        (root / "roms" / "snes-link" / "Hacks" / "Snes Hack.sfc").string()};
    writeFile(gamelistPath("update"),
              "<?xml version=\"1.0\"?>\n"
              "<gameList>\n"
              "\t<game><path>./First Game.sfc</path><name>First Game</name></game>\n"
              "\t<game><path>" +
                  absoluteSecond +
                  "</path><name>Second Game</name></game>\n"
                  "\t<game><path>" +
                  symlinkedThird +
                  "</path><name>Snes Hack</name></game>\n"
                  "</gameList>\n");

    setTestSettings(false, false);

    SystemData system {"update", "Update", "Update", environment(roms), "snes", false, false};

    for (const char* name : {"First Game.sfc", "Second Game.sfc", "Hacks/Snes Hack.sfc"}) {
        FileData* game {findNode(system, name)};
        EXPECT(game != nullptr);
        if (game != nullptr)
            game->metadata.set("playcount", "4");
    }

    GamelistFileParser::updateGamelist(&system);

    pugi::xml_document document;
    EXPECT(document.load_file(gamelistPath("update").string().c_str()));
    const pugi::xml_node rootNode {document.child("gameList")};

    std::vector<std::string> paths;
    std::vector<std::string> playcounts;
    for (pugi::xml_node node {rootNode.child("game")}; node; node = node.next_sibling("game")) {
        paths.emplace_back(node.child("path").text().get());
        playcounts.emplace_back(node.child("playcount").text().get());
    }
    std::sort(paths.begin(), paths.end());
    std::sort(playcounts.begin(), playcounts.end());

    // Three entries, one per record: a missed lookup would leave the old node in place and append
    // a second one, which changes both vectors.
    const std::vector<std::string> expectedPaths {"./First Game.sfc", "./Hacks/Snes Hack.sfc",
                                                  "./Second Game.sfc"};
    const std::vector<std::string> expectedPlaycounts {"4", "4", "4"};

    if (paths != expectedPaths) {
        std::string actual {"    gamelist paths:"};
        for (const std::string& path : paths)
            actual += " \"" + path + "\"";
        std::printf("%s\n", actual.c_str());
    }

    EXPECT(paths == expectedPaths);
    EXPECT(playcounts == expectedPlaycounts);
}
#endif
