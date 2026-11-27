//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GamelistSplitTests.cpp
//
//  Test cases for the owner-aware gamelist serialisation. The legacy writer must stay
//  byte-identical, including its loss of unrecognized child elements; the shared-only writer drops
//  the personal fields and the system-preference elements and keeps unrecognized children; and
//  setApplied must not set the change flag.
//

#include "TestHarness.h"
#include "TestEnvironment.h"

#include "FileData.h"
#include "GamelistFileParser.h"
#include "MetaData.h"
#include "Settings.h"
#include "SystemData.h"

#include <pugixml.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // The ROM tree is staged per fixture; the application data directory is the one the shared
    // test environment set up for the whole process.
    const fs::path& fixtureRoot()
    {
        static const fs::path root {[] {
            const fs::path path {fs::temp_directory_path() / "es-de-gamelist-split"};
            fs::remove_all(path);
            fs::create_directories(path);
            return path;
        }()};

        return root;
    }

    std::string readFile(const fs::path& path)
    {
        std::ifstream file {path, std::ios::binary};
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    void writeFile(const fs::path& path, const std::string& contents)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file {path, std::ios::binary | std::ios::trunc};
        file << contents;
    }

    std::string testData(const std::string& name)
    {
        return readFile(fs::path {ES_APP_TEST_DATA_DIR} / "gamelist-split" / name);
    }

    void reportMismatch(const std::string& actual, const std::string& expected)
    {
        if (actual != expected) {
            std::printf("--- actual ---\n%s\n--- expected ---\n%s\n", actual.c_str(),
                        expected.c_str());
        }
    }

    // A system with three games, one folder, and the shared gamelist fixture.
    struct Fixture {
        fs::path roms;
        fs::path gamelist;
        SystemData* system {nullptr};

        Fixture()
        {
            const fs::path root {fixtureRoot()};
            fs::remove_all(root / "roms");
            roms = root / "roms" / "snes";
            gamelist = TestEnvironment::appDataDirectory() / "gamelists" / "snes" / "gamelist.xml";

            writeFile(roms / "First Game (USA).sfc", "");
            writeFile(roms / "Second Game (USA).sfc", "");
            writeFile(roms / "Hacks" / "Snes Hack.sfc", "");
            writeFile(gamelist, testData("gamelist.xml"));

            Settings::getInstance()->setString("SaveGamelistsMode", "never");
            Settings::getInstance()->setBool("ParseGamelistOnly", false);
            Settings::getInstance()->setBool("IgnoreGamelist", false);
            Settings::getInstance()->setBool("ShowHiddenGames", true);

            SystemEnvironmentData* env {new SystemEnvironmentData};
            env->mStartPath = roms.string();
            env->mSearchExtensions = {".sfc"};
            env->mLaunchCommands = {{"%EMULATOR% %ROM%", "Snes9x - Current"}};
            system = new SystemData("snes", "Super Nintendo", "Super Nintendo", env, "snes", false,
                                    false);
        }

        ~Fixture() { delete system; }

        FileData* node(const std::string& relativePath)
        {
            for (FileData* candidate : system->getRootFolder()->getChildrenRecursive()) {
                if (candidate->getPath() == (roms / relativePath).string())
                    return candidate;
            }

            return nullptr;
        }

        std::string gamelistBytes() { return readFile(gamelist); }
    };
} // namespace

TEST_CASE(legacyWriterStaysByteIdentical)
{
    Fixture fixture;
    FileData* changed {fixture.node("Second Game (USA).sfc")};
    EXPECT(changed != nullptr);
    if (changed == nullptr)
        return;

    // A shared-field change is enough to rewrite the record.
    changed->metadata.set("name", "Second Game Renamed");
    GamelistFileParser::updateGamelist(fixture.system);

    const std::string actual {fixture.gamelistBytes()};
    const std::string expected {testData("gamelist-legacy.xml")};
    reportMismatch(actual, expected);
    EXPECT(actual == expected);
}

TEST_CASE(legacyWriterDropsUnrecognizedChildrenOfARewrittenEntry)
{
    Fixture fixture;
    FileData* changed {fixture.node("Second Game (USA).sfc")};
    EXPECT(changed != nullptr);
    if (changed == nullptr)
        return;

    changed->metadata.set("name", "Second Game Renamed");
    GamelistFileParser::updateGamelist(fixture.system);

    const std::string output {fixture.gamelistBytes()};

    // The rewritten entry lost its unrecognized children, as the legacy writer does today.
    EXPECT(output.find("scraperNotes") == std::string::npos);
    EXPECT(output.find("also keep") == std::string::npos);

    // Entries the writer did not rewrite keep theirs.
    EXPECT(output.find("<mysteryTag>keep me</mysteryTag>") != std::string::npos);
    EXPECT(output.find("<mysteryFolderTag>keep</mysteryFolderTag>") != std::string::npos);
}

TEST_CASE(ownerFilterSelectsTheKeysToWrite)
{
    MetaDataList metadata {GAME_METADATA};
    metadata.set("name", "Fixture Game");
    metadata.set("favorite", "true");
    metadata.set("playcount", "4");
    metadata.set("lastplayed", "20240102T030405");

    pugi::xml_document document;

    // The default filter is the legacy one and writes every non-default key.
    pugi::xml_node everything {document.append_child("game")};
    metadata.appendToXML(everything, true, "");
    EXPECT(everything.child("name"));
    EXPECT(everything.child("favorite"));
    EXPECT(everything.child("playcount"));
    EXPECT(everything.child("lastplayed"));

    pugi::xml_node shared {document.append_child("game")};
    metadata.appendToXML(shared, true, "", MD_FILTER_SHARED);
    EXPECT(shared.child("name"));
    EXPECT(!shared.child("favorite"));
    EXPECT(!shared.child("playcount"));
    EXPECT(!shared.child("lastplayed"));
}

TEST_CASE(sharedWriterDropsPersonalStateAndKeepsUnrecognizedChildren)
{
    Fixture fixture;

    std::vector<const FileData*> records;
    for (const std::string path : {"First Game (USA).sfc", "Second Game (USA).sfc", "Hacks",
                                   "Hacks/Snes Hack.sfc"}) {
        FileData* record {fixture.node(path)};
        EXPECT(record != nullptr);
        records.push_back(record);
    }

    const std::string output {
        GamelistFileParser::writeSharedDocument(testData("gamelist.xml"), *fixture.system, records)};
    const std::string expected {testData("gamelist-shared.xml")};
    reportMismatch(output, expected);
    EXPECT(output == expected);

    for (const std::string key : {"<favorite>", "<completed>", "<hidden>", "<hidemetadata>",
                                  "<playcount>", "<playtime>", "<lastplayed>", "<altemulator>",
                                  "<controller>", "<screen>", "alternativeEmulator",
                                  "launchOnOtherScreen"}) {
        if (output.find(key) != std::string::npos)
            std::printf("personal or preference key in the shared output: %s\n", key.c_str());
        EXPECT(output.find(key) == std::string::npos);
    }

    // Unrecognized children of a rewritten entry survive, unlike in the legacy writer.
    EXPECT(output.find("<mysteryTag>keep me</mysteryTag>") != std::string::npos);
    EXPECT(output.find("<mysteryFolderTag>keep</mysteryFolderTag>") != std::string::npos);
    EXPECT(output.find("<scraperNotes>") != std::string::npos);
    EXPECT(output.find("<note>unknown child element</note>") != std::string::npos);
}

TEST_CASE(sharedWriterPreservesNestedUnknownChildren)
{
    Fixture fixture;
    FileData* record {fixture.node("First Game (USA).sfc")};
    EXPECT(record != nullptr);
    if (record == nullptr)
        return;

    const std::string existing {
        "<?xml version=\"1.0\"?><gameList><game><path>./First Game (USA).sfc</path>"
        "<name>First Game</name><desc>First fixture game.</desc><favorite>true</favorite>"
        "<mystery attr=\"keep\"><payload><item>one</item><item>two</item></payload>"
        "<note>plain</note></mystery></game></gameList>"};

    const std::string output {
        GamelistFileParser::writeSharedDocument(existing, *fixture.system, {record})};

    pugi::xml_document document;
    const pugi::xml_parse_result parsed {document.load_buffer(output.data(), output.size())};
    EXPECT(parsed);
    if (!parsed)
        return;

    const pugi::xml_node game {document.child("gameList").child("game")};
    const pugi::xml_node mystery {game.child("mystery")};
    EXPECT(mystery);
    EXPECT(std::string {mystery.attribute("attr").value()} == "keep");

    // A nested unknown subtree keeps its structure and values, not just its outer element.
    const pugi::xml_node payload {mystery.child("payload")};
    const pugi::xml_node firstItem {payload.child("item")};
    EXPECT(payload);
    EXPECT(std::string {firstItem.text().get()} == "one");
    EXPECT(std::string {firstItem.next_sibling().text().get()} == "two");
    EXPECT(std::string {mystery.child("note").text().get()} == "plain");

    // The rewritten entry keeps shared fields but loses personal state.
    EXPECT(game.child("name"));
    EXPECT(!game.child("favorite"));
}

TEST_CASE(sharedWriterReplacesEntriesWrittenInAnyPathForm)
{
    Fixture fixture;
    FileData* first {fixture.node("First Game (USA).sfc")};
    FileData* second {fixture.node("Second Game (USA).sfc")};
    EXPECT(first != nullptr);
    EXPECT(second != nullptr);
    if (first == nullptr || second == nullptr)
        return;

    // "./" spells the first entry. The second is an absolute path written through a symlink to
    // the system directory, so only the canonical-path fallback can match it.
    const fs::path alias {fixture.roms.parent_path() / "snes-link"};
    fs::remove(alias);
    fs::create_symlink(fixture.roms, alias);
    const std::string absoluteSecond {(alias / "Second Game (USA).sfc").string()};

    const std::string existing {
        "<?xml version=\"1.0\"?><gameList>"
        "<game><path>./First Game (USA).sfc</path><name>Old First</name></game>"
        "<game><path>" +
        absoluteSecond +
        "</path><name>Old Second</name></game>"
        "</gameList>"};

    first->metadata.set("name", "First Shared");
    second->metadata.set("name", "Second Shared");

    const std::string output {
        GamelistFileParser::writeSharedDocument(existing, *fixture.system, {first, second})};

    pugi::xml_document document;
    const pugi::xml_parse_result parsed {document.load_buffer(output.data(), output.size())};
    EXPECT(parsed);
    if (!parsed)
        return;

    std::vector<std::string> paths;
    std::vector<std::string> names;
    for (pugi::xml_node node {document.child("gameList").child("game")}; node;
         node = node.next_sibling("game")) {
        paths.emplace_back(node.child("path").text().get());
        names.emplace_back(node.child("name").text().get());
    }

    // One entry per record: a missed lookup would keep the old entry and append a second node
    // for the same record. The names prove the old contents were replaced, not just counted.
    const std::vector<std::string> expectedPaths {"./First Game (USA).sfc",
                                                  "./Second Game (USA).sfc"};
    const std::vector<std::string> expectedNames {"First Shared", "Second Shared"};
    EXPECT(paths == expectedPaths);
    EXPECT(names == expectedNames);
}

TEST_CASE(sharedWriterRefusesAnUnusableDocument)
{
    Fixture fixture;

    EXPECT(GamelistFileParser::writeSharedDocument("not xml", *fixture.system, {}).empty());
    EXPECT(GamelistFileParser::writeSharedDocument("<other/>", *fixture.system, {}).empty());
}

TEST_CASE(setAppliedLeavesTheChangeFlagUnset)
{
    MetaDataList metadata {GAME_METADATA};
    metadata.resetChangedFlag();

    metadata.setApplied("favorite", "true");
    EXPECT(metadata.get("favorite") == "true");
    EXPECT(!metadata.wasChanged());

    metadata.set("favorite", "false");
    EXPECT(metadata.get("favorite") == "false");
    EXPECT(metadata.wasChanged());
}
