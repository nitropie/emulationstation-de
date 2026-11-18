//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileLoadTests.cpp
//
//  Test cases for the gamelist load path with and without a profile. The legacy dump goldens were
//  captured from the code before the profile overlay existed and must stay untouched.
//

#include "TestHarness.h"
#include "TestEnvironment.h"

#include "FileData.h"
#include "MetaData.h"
#include "Settings.h"
#include "SystemData.h"
#include "profiles/ProfileMode.h"
#include "utils/FileSystemUtil.h"

#include <algorithm>
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

    constexpr const char* sProfileId {"6f1c2a6e-0e8a-4c5b-9d0a-6f8f0e3b1c2d"};

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
        return readFile(fs::path {ES_APP_TEST_DATA_DIR} / "profile-load" / name);
    }

    // The dump is one space-separated line per metadata value, so spaces and control characters
    // in any field are escaped and the value itself is quoted.
    std::string escape(const std::string& text)
    {
        std::string result;
        for (const char character : text) {
            if (character == '\\')
                result += "\\\\";
            else if (character == ' ')
                result += "\\s";
            else if (character == '\t')
                result += "\\t";
            else if (character == '\n')
                result += "\\n";
            else
                result += character;
        }
        return result;
    }

    void reportMismatch(const std::string& actual, const std::string& expected)
    {
        if (actual != expected)
            std::printf("--- actual ---\n%s\n--- expected ---\n%s\n", actual.c_str(),
                        expected.c_str());
    }

    // A ROM tree, the shared gamelist fixture and an optional session, staged below the shared
    // application data directory.
    struct Fixture {
        fs::path root;
        fs::path roms;
        fs::path appdata;
        fs::path gamelist;

        explicit Fixture(const std::string& gamelistFixture = "gamelist.xml")
        {
            root = fs::temp_directory_path() / "es-de-profile-load";
            fs::remove_all(root);
            roms = root / "roms" / "snes";
            appdata = TestEnvironment::appDataDirectory();
            gamelist = appdata / "gamelists" / "snes" / "gamelist.xml";

            for (const std::string file :
                 {"First Game (USA).sfc", "Second Game (USA).sfc", "Third Game (USA).sfc",
                  "Hidden Folder/Hidden Child.sfc", "Visible Folder/Visible Child.sfc",
                  "Extra Folder/Extra Child.sfc", "Gone Folder/Gone Child.sfc",
                  "Deep1/Deep2/Deep3.sfc", "Secret/Deep/A.sfc"}) {
                writeFile(roms / file, "");
            }

            writeFile(gamelist, testData(gamelistFixture));

            Settings::getInstance()->setString("SaveGamelistsMode", "never");
            Settings::getInstance()->setBool("ParseGamelistOnly", false);
            Settings::getInstance()->setBool("IgnoreGamelist", false);
            Settings::getInstance()->setBool("ShowHiddenGames", false);
        }

        fs::path statePath() const { return appdata / "profile-state" / "snes.json"; }

        // Copies one of the state fixtures to the profile's state directory.
        void stageState(const std::string& fixture, const bool withBackup = false) const
        {
            const fs::path source {fs::path {ES_APP_TEST_DATA_DIR} / "profile-load" / fixture};
            writeFile(statePath(), readFile(source / "snes.json"));
            if (withBackup)
                writeFile(statePath().string() + ".bak", readFile(source / "snes.json.bak"));
        }
    };

    SystemData* loadSystem(const Fixture& fixture)
    {
        SystemEnvironmentData* env {new SystemEnvironmentData};
        env->mStartPath = fixture.roms.string();
        env->mSearchExtensions = {".sfc"};
        env->mLaunchCommands = {{"%EMULATOR% %ROM%", "Snes9x - Current"}};
        return new SystemData("snes", "Super Nintendo", "Super Nintendo", env, "snes", false,
                              false);
    }

    // A deterministic dump of every game and folder record: type, relative path, then every
    // metadata key and its value, sorted by type and path.
    std::string dumpSystem(SystemData& system)
    {
        struct Node {
            int order;
            std::string type;
            std::string path;
            MetaDataList* metadata;
        };

        std::vector<Node> nodes;
        for (FileData* node : system.getRootFolder()->getChildrenRecursive()) {
            if (node->getType() != GAME && node->getType() != FOLDER)
                continue;

            bool contains {false};
            const std::string relative {Utils::FileSystem::removeCommonPath(
                node->getPath(), system.getStartPath(), contains)};
            if (!contains) {
                std::printf("node outside the system root: %s\n", node->getPath().c_str());
                continue;
            }

            nodes.push_back({node->getType() == GAME ? 0 : 1,
                             node->getType() == GAME ? "game" : "folder", relative,
                             &node->metadata});
        }

        std::sort(nodes.begin(), nodes.end(), [](const Node& left, const Node& right) {
            if (left.order != right.order)
                return left.order < right.order;
            return left.path < right.path;
        });

        std::string dump;
        for (const Node& node : nodes) {
            for (const MetaDataDecl& decl : getMDDByType(node.metadata->getType())) {
                dump += escape(node.type) + " " + escape(node.path) + " " + escape(decl.key) +
                        " \"" + escape(node.metadata->get(decl.key)) + "\"\n";
            }
        }
        return dump;
    }

    FileData* findNode(SystemData& system, const std::string& relativePath)
    {
        for (FileData* node : system.getRootFolder()->getChildrenRecursive()) {
            if (node->getType() != GAME && node->getType() != FOLDER)
                continue;

            bool contains {false};
            const std::string relative {Utils::FileSystem::removeCommonPath(
                node->getPath(), system.getStartPath(), contains)};
            if (contains && relative == relativePath)
                return node;
        }

        return nullptr;
    }

    // Every record of the tree, and whether every one of them has its change flag cleared.
    bool everyChangeFlagCleared(SystemData& system, int& nodes)
    {
        nodes = 0;
        bool cleared {true};
        for (FileData* node : system.getRootFolder()->getChildrenRecursive()) {
            if (node->getType() != GAME && node->getType() != FOLDER)
                continue;

            ++nodes;
            cleared = cleared && !node->metadata.wasChanged();
        }

        return cleared;
    }

    // Writes a session directory (catalog, metadata M and avatar manifest) and exports it, so the
    // next SystemData construction loads its overlay from the profile state below `appdata`.
    void enterProfileMode(const fs::path& appdata)
    {
        const fs::path session {fs::temp_directory_path() / "es-de-profile-load" / "session"};
        fs::create_directories(session);
        const fs::path metadata {session / "profile.json"};
        const fs::path avatars {session / "avatars.json"};
        const fs::path catalog {session / "catalog.json"};

        writeFile(metadata, "{\"schema\": \"nitropie.profile\", \"version\": 1, \"id\": \"" +
                                std::string {sProfileId} +
                                "\", \"displayName\": \"David\", \"avatar\": \"dog\"}\n");
        writeFile(avatars, "{\"version\": 1, \"default\": \"dog\", \"avatars\": [{\"id\": "
                           "\"dog\", \"file\": \"dog.png\", \"label\": \"Dog\"}]}\n");

        const std::string home {(appdata.parent_path() / "home").string()};
        writeFile(catalog,
                  "{\"version\": 1, \"default\": \"" + std::string {sProfileId} +
                      "\", \"libraryRoot\": \"" + (session / "library").string() +
                      "\", \"avatarManifest\": \"" + avatars.string() +
                      "\", \"profiles\": [{\"id\": \"" + std::string {sProfileId} +
                      "\", \"metadata\": \"" + metadata.string() + "\", \"home\": \"" + home +
                      "\", \"appdata\": \"" + appdata.string() + "\", \"saves\": \"" + home +
                      "/saves\", \"xdgConfigHome\": \"" + home + "/.config\"" +
                      ", \"xdgDataHome\": \"" + home + "/.local/share\"" +
                      ", \"xdgStateHome\": \"" + home + "/.local/state\"" +
                      ", \"xdgCacheHome\": \"" + home + "/.cache\"}]}\n");

        setenv("NITROPIE_PROFILE_CATALOG", catalog.c_str(), 1);
        setenv("NITROPIE_PROFILE_ID", sProfileId, 1);
        ProfileMode::initialize();
    }

    void leaveProfileMode()
    {
        unsetenv("NITROPIE_PROFILE_CATALOG");
        unsetenv("NITROPIE_PROFILE_ID");
        ProfileMode::initialize();
    }
} // namespace

TEST_CASE(legacySystemLoadMatchesGoldenDump)
{
    leaveProfileMode();
    Fixture fixture;
    SystemData* system {loadSystem(fixture)};
    const std::string actual {dumpSystem(*system)};
    const std::string expected {testData("legacy-dump.txt")};
    reportMismatch(actual, expected);
    EXPECT(actual == expected);
    delete system;
}

TEST_CASE(legacySystemLoadIgnoresAStateFile)
{
    leaveProfileMode();
    Fixture fixture;
    fixture.stageState("profile-state");
    SystemData* system {loadSystem(fixture)};
    const std::string actual {dumpSystem(*system)};
    const std::string expected {testData("legacy-dump-with-state.txt")};
    reportMismatch(actual, expected);
    EXPECT(actual == expected);
    delete system;
}

TEST_CASE(profileOverlayResetsStalePersonalTags)
{
    Fixture fixture;
    fixture.stageState("profile-state");
    enterProfileMode(fixture.appdata);
    SystemData* system {loadSystem(fixture)};

    FileData* first {findNode(*system, "First Game (USA).sfc")};
    EXPECT(first != nullptr);
    if (first != nullptr) {
        EXPECT(first->metadata.get("favorite") == "false");
        EXPECT(first->metadata.get("playcount") == "0");
        EXPECT(first->metadata.get("playtime") == "0");
        EXPECT(first->metadata.get("lastplayed") == "0");
    }

    FileData* visible {findNode(*system, "Visible Folder/Visible Child.sfc")};
    EXPECT(visible != nullptr);
    if (visible != nullptr)
        EXPECT(visible->metadata.get("completed") == "false");

    delete system;
    leaveProfileMode();
}

TEST_CASE(profileOverlayAppliesTheStateFile)
{
    Fixture fixture;
    fixture.stageState("profile-state");
    enterProfileMode(fixture.appdata);
    SystemData* system {loadSystem(fixture)};

    // A hidden record is filtered and its now-empty parent removed.
    EXPECT(findNode(*system, "Second Game (USA).sfc") == nullptr);
    EXPECT(findNode(*system, "Gone Folder/Gone Child.sfc") == nullptr);
    EXPECT(findNode(*system, "Gone Folder") == nullptr);

    // Nested folders whose only game is hidden go away bottom up.
    EXPECT(findNode(*system, "Deep1/Deep2/Deep3.sfc") == nullptr);
    EXPECT(findNode(*system, "Deep1/Deep2") == nullptr);
    EXPECT(findNode(*system, "Deep1") == nullptr);

    // Hidden tags in the shared gamelist are stale and must not hide anything by themselves.
    EXPECT(findNode(*system, "Hidden Folder") != nullptr);
    EXPECT(findNode(*system, "Hidden Folder/Hidden Child.sfc") != nullptr);
    FileData* secret {findNode(*system, "Secret/Deep/A.sfc")};
    EXPECT(secret != nullptr);
    if (secret != nullptr)
        EXPECT(secret->metadata.get("hidden") == "false");

    // Records for newly discovered files and folders carry the overlay values.
    FileData* third {findNode(*system, "Third Game (USA).sfc")};
    EXPECT(third != nullptr);
    if (third != nullptr) {
        EXPECT(third->metadata.get("favorite") == "true");
        EXPECT(third->metadata.get("playcount") == "5");
        EXPECT(third->metadata.get("lastplayed") == "20250102T030405");
    }

    FileData* extraFolder {findNode(*system, "Extra Folder")};
    EXPECT(extraFolder != nullptr);
    if (extraFolder != nullptr)
        EXPECT(extraFolder->metadata.get("favorite") == "true");

    FileData* extraChild {findNode(*system, "Extra Folder/Extra Child.sfc")};
    EXPECT(extraChild != nullptr);
    if (extraChild != nullptr)
        EXPECT(extraChild->metadata.get("playcount") == "2");

    delete system;
    leaveProfileMode();
}

TEST_CASE(profileLoadLeavesNoChangeFlags)
{
    {
        Fixture fixture;
        fixture.stageState("profile-state");
        enterProfileMode(fixture.appdata);
        SystemData* system {loadSystem(fixture)};
        int nodes {0};
        EXPECT(everyChangeFlagCleared(*system, nodes));
        EXPECT(nodes > 0);
        delete system;
        leaveProfileMode();
    }

    // Legacy flags are unchanged: a freshly loaded tree has nothing pending either.
    {
        leaveProfileMode();
        Fixture fixture;
        SystemData* system {loadSystem(fixture)};
        int nodes {0};
        EXPECT(everyChangeFlagCleared(*system, nodes));
        EXPECT(nodes > 0);
        delete system;
    }
}

TEST_CASE(profileLoadDoesNotTouchTheFiles)
{
    Fixture fixture;
    fixture.stageState("profile-state");
    // The legacy exit path writes the gamelist in this mode; profile mode must not.
    Settings::getInstance()->setString("SaveGamelistsMode", "on exit");
    enterProfileMode(fixture.appdata);

    const std::string gamelistBytes {readFile(fixture.gamelist)};
    const std::string stateBytes {readFile(fixture.statePath())};
    const fs::file_time_type gamelistTime {fs::last_write_time(fixture.gamelist)};
    const fs::file_time_type stateTime {fs::last_write_time(fixture.statePath())};

    SystemData* system {loadSystem(fixture)};
    delete system;

    EXPECT(readFile(fixture.gamelist) == gamelistBytes);
    EXPECT(readFile(fixture.statePath()) == stateBytes);
    EXPECT(fs::last_write_time(fixture.gamelist) == gamelistTime);
    EXPECT(fs::last_write_time(fixture.statePath()) == stateTime);

    leaveProfileMode();
}

TEST_CASE(profilePreferencesOverrideTheSharedGamelist)
{
    Fixture fixture {"gamelist-preferences.xml"};
    fixture.stageState("profile-state");

    // Legacy mode reads the stale preference elements from the shared gamelist.
    leaveProfileMode();
    SystemData* legacy {loadSystem(fixture)};
    EXPECT(legacy->getAlternativeEmulator() == "<INVALID>Stale Emulator");
    EXPECT(legacy->getLaunchOnOtherScreen());
    delete legacy;

    // Profile mode resets them and applies the stored ones, including the launch screen
    // preference, which the XML reader only handles on Android.
    enterProfileMode(fixture.appdata);
    SystemData* system {loadSystem(fixture)};
    EXPECT(system->getAlternativeEmulator() == "Snes9x - Current");
    EXPECT(!system->getLaunchOnOtherScreen());
    delete system;

    leaveProfileMode();
}

TEST_CASE(profileInvalidStoredEmulatorKeepsTheMarkerAndTheFile)
{
    Fixture fixture;
    fixture.stageState("profile-state-invalid");
    enterProfileMode(fixture.appdata);

    const std::string stateBytes {readFile(fixture.statePath())};

    SystemData* system {loadSystem(fixture)};
    EXPECT(system->getAlternativeEmulator() == "<INVALID>No Such Emulator");
    EXPECT(!system->getLaunchOnOtherScreen());
    delete system;

    // The stored label stays until the user picks another emulator.
    EXPECT(readFile(fixture.statePath()) == stateBytes);

    leaveProfileMode();
}

TEST_CASE(unresolvedStoreIsReportedAndRecoverable)
{
    Fixture fixture;
    fixture.stageState("profile-state-too-new");
    enterProfileMode(fixture.appdata);

    SystemData* system {loadSystem(fixture)};
    EXPECT(!ProfileMode::storesResolved());
    EXPECT(!ProfileMode::hasRecoverableStore());
    EXPECT(ProfileMode::unresolvedReason().find("snes") != std::string::npos);
    EXPECT(ProfileMode::unresolvedReason().find("version") != std::string::npos);
    delete system;

    // A usable backup makes the store recoverable.
    fixture.stageState("profile-state-too-new", true);
    EXPECT(ProfileMode::hasRecoverableStore());
    EXPECT(ProfileMode::recoverStores());
    EXPECT(ProfileMode::storesResolved());

    // The recovered state is applied on the next load, and the unusable file is still untouched.
    SystemData* recovered {loadSystem(fixture)};
    EXPECT(findNode(*recovered, "Second Game (USA).sfc") == nullptr);
    FileData* third {findNode(*recovered, "Third Game (USA).sfc")};
    EXPECT(third != nullptr);
    if (third != nullptr)
        EXPECT(third->metadata.get("favorite") == "true");
    delete recovered;

    leaveProfileMode();
}

TEST_CASE(collectionSystemsOwnNoStore)
{
    Fixture fixture;
    enterProfileMode(fixture.appdata);

    EXPECT(ProfileMode::storeForSystem("snes", false) != nullptr);
    EXPECT(ProfileMode::storeForSystem("collections", true) == nullptr);

    SystemEnvironmentData* env {new SystemEnvironmentData};
    env->mStartPath = "";
    SystemData collection {"collections", "Collections", "Collections", env, "collections", true,
                           false};
    EXPECT(collection.personalStore() == nullptr);

    leaveProfileMode();
}
