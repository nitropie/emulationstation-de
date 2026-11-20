//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileSaveTests.cpp
//
//  Test cases for the profile save path: capture from the game tree, the exact state document,
//  mode-aware save points, the shared gamelist byte guarantee, flush failure and retry, capture
//  before teardown and the system preferences. Every expected document is an exact golden.
//

#include "TestHarness.h"
#include "TestEnvironment.h"

#include "FileData.h"
#include "GamelistFileParser.h"
#include "MetaData.h"
#include "Settings.h"
#include "SystemData.h"
#include "guis/GuiScraperSearch.h"
#include "profiles/ProfileMode.h"
#include "profiles/ProfileStateJsonStore.h"
#include "profiles/ProfileStateStore.h"
#include "scrapers/Scraper.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* sProfileId {"0b7a5c2e-3d94-4f61-8a17-5c2d9e6b4f08"};

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
        return readFile(fs::path {ES_APP_TEST_DATA_DIR} / "profile-save" / name);
    }

    void reportMismatch(const std::string& actual, const std::string& expected)
    {
        if (actual != expected)
            std::printf("--- actual ---\n%s\n--- expected ---\n%s\n", actual.c_str(),
                        expected.c_str());
    }

    // A ROM tree, the shared gamelist fixture and an optional personal state, staged below the
    // shared application data directory. Every test uses its own system name so the
    // process-wide stores and state files of another test can never interfere.
    struct Fixture {
        std::string system;
        fs::path root;
        fs::path roms;
        fs::path appdata;
        fs::path gamelist;
        fs::path stateDirectory;

        explicit Fixture(const std::string& systemName)
            : system {systemName}
        {
            root = fs::temp_directory_path() / ("es-de-profile-save-" + systemName);
            fs::remove_all(root);
            roms = root / "roms" / systemName;
            appdata = TestEnvironment::appDataDirectory();
            gamelist = appdata / "gamelists" / systemName / "gamelist.xml";
            stateDirectory = appdata / "profile-state";

            for (const std::string file :
                 {"Alpha Game (USA).sfc", "Beta Game (USA).sfc", "Gamma Game (USA).sfc",
                  "Folder One/Child One.sfc", "Folder One/Child Two.sfc"}) {
                writeFile(roms / file, "");
            }

            fs::create_directories(stateDirectory);
            writeFile(gamelist, testData("gamelist.xml"));

            // The stores are process-wide, and so is the application data directory, so a state
            // file left by an earlier test case has to be gone before this one starts.
            for (const char* suffix : {"", ".bak", ".tmp", ".bak.tmp"})
                fs::remove(fs::path {statePath().string() + suffix});

            Settings::getInstance()->setString("SaveGamelistsMode", "never");
            Settings::getInstance()->setBool("ParseGamelistOnly", false);
            Settings::getInstance()->setBool("IgnoreGamelist", false);
            Settings::getInstance()->setBool("ShowHiddenGames", false);
        }

        fs::path statePath() const { return stateDirectory / (system + ".json"); }

        // A catalog, M and the avatar manifest for one profile whose appdata is the test
        // application data directory, exported before the next SystemData is constructed.
        void enterProfileMode() const
        {
            const fs::path session {root / "session"};
            fs::create_directories(session);
            const fs::path metadata {session / "profile.json"};
            const fs::path avatars {session / "avatars.json"};
            const fs::path catalog {session / "catalog.json"};

            writeFile(metadata,
                      "{\"schema\": \"nitropie.profile\", \"version\": 1, \"id\": \"" +
                          std::string {sProfileId} +
                          "\", \"displayName\": \"Tester\", \"avatar\": \"dog\"}\n");
            writeFile(avatars, "{\"version\": 1, \"default\": \"dog\", \"avatars\": [{\"id\": "
                               "\"dog\", \"file\": \"dog.png\", \"label\": \"Dog\"}]}\n");

            const std::string home {(root / "home").string()};
            writeFile(catalog,
                      "{\"version\": 1, \"default\": \"" + std::string {sProfileId} +
                          "\", \"libraryRoot\": \"" + (root / "library").string() +
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
    };

    void leaveProfileMode()
    {
        unsetenv("NITROPIE_PROFILE_CATALOG");
        unsetenv("NITROPIE_PROFILE_ID");
        ProfileMode::initialize();
        // The settings are process-wide too; leave the default the other test files expect.
        Settings::getInstance()->setString("SaveGamelistsMode", "never");
    }

    SystemData* loadSystem(const Fixture& fixture)
    {
        SystemEnvironmentData* env {new SystemEnvironmentData};
        env->mStartPath = fixture.roms.string();
        env->mSearchExtensions = {".sfc"};
        env->mLaunchCommands = {{"%EMULATOR% %ROM%", "Snes9x - Current"},
                                {"%RETROARCH% %ROM%", "RetroArch"}};
        return new SystemData(fixture.system, "Super Nintendo", "Super Nintendo", env, "snes",
                              false, false);
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

    // The whole state document must survive unchanged. Used where the content is asserted field
    // by field elsewhere.
    bool fileMatches(const fs::path& path, const std::string& expected)
    {
        if (!fs::exists(path))
            return false;
        const std::string actual {readFile(path)};
        reportMismatch(actual, expected);
        return actual == expected;
    }
} // namespace

// The state document of a capture is byte exact: the personal fields keep their declaration
// order, the indentation is two spaces, the entries are sorted by path and the document ends with
// a newline.
TEST_CASE(profileCaptureWritesTheExactDocument)
{
    Fixture fixture {"snes"};
    fixture.enterProfileMode();

    const std::string gamelistBytes {readFile(fixture.gamelist)};
    const fs::file_time_type gamelistTime {fs::last_write_time(fixture.gamelist)};

    SystemData* system {loadSystem(fixture)};

    // The shared gamelist says Alpha is a favourite with nine plays; the overlay reset it.
    FileData* alpha {findNode(*system, "Alpha Game (USA).sfc")};
    EXPECT(alpha != nullptr);
    if (alpha != nullptr) {
        EXPECT(alpha->metadata.get("favorite") == "false");
        EXPECT(alpha->metadata.get("playcount") == "0");
        EXPECT(alpha->metadata.get("lastplayed") == "0");
        alpha->metadata.set("favorite", "true");
        alpha->metadata.set("completed", "true");
        alpha->metadata.set("playcount", "3");
    }

    FileData* beta {findNode(*system, "Beta Game (USA).sfc")};
    EXPECT(beta != nullptr);
    if (beta != nullptr)
        beta->metadata.set("playcount", "1");

    const ProfileState::FlushResult result {system->flushPersonalState()};
    EXPECT(result.status == ProfileState::FlushStatus::success);
    EXPECT(result.reason.empty());

    EXPECT(fileMatches(fixture.statePath(), testData("capture.json")));
    // The first write has no previous generation, so no backup is published.
    EXPECT(!fs::exists(fixture.statePath().string() + ".bak"));

    // No personal edit touches the shared gamelist.
    EXPECT(readFile(fixture.gamelist) == gamelistBytes);
    EXPECT(fs::last_write_time(fixture.gamelist) == gamelistTime);

    delete system;
    leaveProfileMode();
}

// A folder favourite and the last-played stamp that FileData::setPlayMetadata propagates to the
// parent folder are captured from the folder record.
TEST_CASE(profileCaptureStoresFolderFavouriteAndLastPlayed)
{
    Fixture fixture {"snes"};
    fixture.enterProfileMode();

    SystemData* system {loadSystem(fixture)};

    FileData* child {findNode(*system, "Folder One/Child One.sfc")};
    FileData* folder {findNode(*system, "Folder One")};
    EXPECT(child != nullptr);
    EXPECT(folder != nullptr);
    if (child != nullptr && folder != nullptr) {
        child->metadata.set("lastplayed", "20250102T030405");
        folder->metadata.set("lastplayed", "20250102T030405");
        folder->metadata.set("favorite", "true");
    }

    const ProfileState::FlushResult result {system->flushPersonalState()};
    EXPECT(result.status == ProfileState::FlushStatus::success);
    EXPECT(fileMatches(fixture.statePath(), testData("folder-propagation.json")));

    delete system;
    leaveProfileMode();
}

// Hiding a folder hides every child in the editor. All of them are captured while they are still
// in the tree, one record each.
TEST_CASE(profileCaptureStoresEveryHiddenChild)
{
    Fixture fixture {"snes"};
    fixture.enterProfileMode();

    SystemData* system {loadSystem(fixture)};

    FileData* folder {findNode(*system, "Folder One")};
    EXPECT(folder != nullptr);
    if (folder != nullptr) {
        folder->metadata.set("hidden", "true");
        for (FileData* child : folder->getChildrenRecursive())
            child->metadata.set("hidden", "true");
    }

    const ProfileState::FlushResult result {system->flushPersonalState()};
    EXPECT(result.status == ProfileState::FlushStatus::success);
    EXPECT(fileMatches(fixture.statePath(), testData("hidden-children.json")));

    delete system;
    leaveProfileMode();
}

// SaveGamelistsMode gates only the legacy writer. A personal change is persisted at the save
// point even when the setting is "never", and the shared gamelist keeps its bytes and mtime.
TEST_CASE(profileSavePointIgnoresSaveGamelistsNever)
{
    Fixture fixture {"atari2600"};
    Settings::getInstance()->setString("SaveGamelistsMode", "never");
    fixture.enterProfileMode();

    const std::string gamelistBytes {readFile(fixture.gamelist)};
    const fs::file_time_type gamelistTime {fs::last_write_time(fixture.gamelist)};

    SystemData* system {loadSystem(fixture)};
    FileData* alpha {findNode(*system, "Alpha Game (USA).sfc")};
    EXPECT(alpha != nullptr);
    if (alpha != nullptr)
        alpha->metadata.set("favorite", "true");

    system->onMetaDataSavePoint();

    EXPECT(fs::exists(fixture.statePath()));
    EXPECT(readFile(fixture.statePath()).find("\"favorite\": true") != std::string::npos);
    EXPECT(readFile(fixture.gamelist) == gamelistBytes);
    EXPECT(fs::last_write_time(fixture.gamelist) == gamelistTime);

    delete system;
    leaveProfileMode();
}

// Both gamelist-saving modes leave the shared file untouched in profile mode, including the
// writeMetaData() callers and the exit path of ~SystemData.
TEST_CASE(profileSavePointsNeverWriteTheSharedGamelist)
{
    for (const std::string mode : {"always", "on exit"}) {
        Fixture fixture {mode == "always" ? "saturn" : "dreamcast"};
        Settings::getInstance()->setString("SaveGamelistsMode", mode);
        fixture.enterProfileMode();

        const std::string gamelistBytes {readFile(fixture.gamelist)};
        const fs::file_time_type gamelistTime {fs::last_write_time(fixture.gamelist)};

        SystemData* system {loadSystem(fixture)};
        FileData* alpha {findNode(*system, "Alpha Game (USA).sfc")};
        EXPECT(alpha != nullptr);
        if (alpha != nullptr)
            alpha->metadata.set("favorite", "true");

        system->onMetaDataSavePoint();
        system->writeMetaData();
        delete system;

        EXPECT(fs::exists(fixture.statePath()));
        EXPECT(readFile(fixture.statePath()).find("\"favorite\": true") != std::string::npos);
        EXPECT(readFile(fixture.gamelist) == gamelistBytes);
        EXPECT(fs::last_write_time(fixture.gamelist) == gamelistTime);
        EXPECT(!fs::exists(fixture.gamelist.string() + ".bak"));

        leaveProfileMode();
    }
}

// A refused write is reported, keeps the dirty state and the previous file, and the next save
// point writes the same document. The read-only state directory makes the write tool's own
// writeFile fail.
TEST_CASE(profileWriteFailureKeepsDirtyStateAndRetries)
{
    Fixture fixture {"gamegear"};
    fixture.enterProfileMode();

    SystemData* system {loadSystem(fixture)};
    ProfileState::JsonStore* store {system->personalStore()};
    EXPECT(store != nullptr);

    FileData* alpha {findNode(*system, "Alpha Game (USA).sfc")};
    FileData* beta {findNode(*system, "Beta Game (USA).sfc")};
    EXPECT(alpha != nullptr);
    EXPECT(beta != nullptr);

    if (alpha != nullptr)
        alpha->metadata.set("favorite", "true");
    EXPECT(system->flushPersonalState().status == ProfileState::FlushStatus::success);

    const std::string firstBytes {readFile(fixture.statePath())};
    EXPECT(firstBytes.find("\"favorite\": true") != std::string::npos);

    // A second change, with the state directory refusing new files.
    if (beta != nullptr)
        beta->metadata.set("playcount", "4");

    std::error_code error;
    fs::permissions(fixture.stateDirectory, fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace, error);
    EXPECT(!error);

    const ProfileState::FlushResult failed {system->flushPersonalState()};
    EXPECT(failed.status == ProfileState::FlushStatus::failure);
    EXPECT(!failed.reason.empty());
    if (store != nullptr)
        EXPECT(store->hasPendingWrites());
    // The previous generation is untouched and the staged change is still pending.
    EXPECT(readFile(fixture.statePath()) == firstBytes);

    // Write permission restored: the retry repeats the whole sequence and publishes both the new
    // document and the previous generation as the backup.
    fs::permissions(fixture.stateDirectory,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace, error);
    EXPECT(!error);

    const ProfileState::FlushResult retried {system->flushPersonalState()};
    EXPECT(retried.status == ProfileState::FlushStatus::success);
    if (store != nullptr)
        EXPECT(!store->hasPendingWrites());

    EXPECT(fileMatches(fixture.statePath(), testData("retry.json")));
    EXPECT(readFile(fixture.statePath().string() + ".bak") == firstBytes);

    delete system;
    leaveProfileMode();
}

// A change made before a rescan survives it: the rescan hook stages the pending records before
// the old trees are destroyed, and the store outlives them.
TEST_CASE(profileCaptureBeforeRescanSurvives)
{
    Fixture fixture {"mastersystem"};
    fixture.enterProfileMode();

    SystemData* system {loadSystem(fixture)};
    FileData* alpha {findNode(*system, "Alpha Game (USA).sfc")};
    EXPECT(alpha != nullptr);
    if (alpha != nullptr)
        alpha->metadata.set("favorite", "true");

    // The rescan path: capture while the old tree is alive, then tear it down and load again.
    // The live set is normally filled by loadConfig, so the test fills it for this call.
    SystemData::sSystemVector.push_back(system);
    SystemData::captureAllPersonalStates();
    SystemData::sSystemVector.pop_back();

    EXPECT(!fs::exists(fixture.statePath()));
    delete system;

    SystemData* reloaded {loadSystem(fixture)};
    FileData* reloadedAlpha {findNode(*reloaded, "Alpha Game (USA).sfc")};
    EXPECT(reloadedAlpha != nullptr);
    if (reloadedAlpha != nullptr)
        EXPECT(reloadedAlpha->metadata.get("favorite") == "true");

    delete reloaded;
    leaveProfileMode();
}

// Destroying a SystemData stages the pending changes instead of writing the shared gamelist.
TEST_CASE(profileDestructorCapturesPendingChanges)
{
    Fixture fixture {"vectrex"};
    fixture.enterProfileMode();

    const std::string gamelistBytes {readFile(fixture.gamelist)};

    SystemData* system {loadSystem(fixture)};
    ProfileState::JsonStore* store {system->personalStore()};
    EXPECT(store != nullptr);

    FileData* alpha {findNode(*system, "Alpha Game (USA).sfc")};
    EXPECT(alpha != nullptr);
    if (alpha != nullptr)
        alpha->metadata.set("favorite", "true");

    delete system;

    if (store != nullptr)
        EXPECT(store->hasPendingWrites());
    // The capture does not write: the next save point owns the flush.
    EXPECT(!fs::exists(fixture.statePath()));
    EXPECT(readFile(fixture.gamelist) == gamelistBytes);

    // The staged change is visible to the next load.
    SystemData* reloaded {loadSystem(fixture)};
    FileData* reloadedAlpha {findNode(*reloaded, "Alpha Game (USA).sfc")};
    EXPECT(reloadedAlpha != nullptr);
    if (reloadedAlpha != nullptr)
        EXPECT(reloadedAlpha->metadata.get("favorite") == "true");
    delete reloaded;

    leaveProfileMode();
}

// Collection systems own no store, so neither a pending entry nor an explicit capture can create
// a state file for them.
TEST_CASE(profileCollectionSystemsAreNeverCaptured)
{
    Fixture fixture {"intellivision"};
    fixture.enterProfileMode();

    SystemData* source {loadSystem(fixture)};
    EXPECT(source->personalStore() != nullptr);

    SystemEnvironmentData* env {new SystemEnvironmentData};
    env->mStartPath = "";
    SystemData collection {"collections", "Collections", "Collections", env, "collections", true,
                           false};
    EXPECT(collection.personalStore() == nullptr);

    const ProfileState::FlushResult result {collection.flushPersonalState()};
    EXPECT(result.status == ProfileState::FlushStatus::nothingToDo);
    EXPECT(!fs::exists(fixture.stateDirectory / "collections.json"));

    // A collection entry is a view over a source game: even with the collection system in the
    // live set, nothing is staged for it.
    FileData* game {findNode(*source, "Alpha Game (USA).sfc")};
    EXPECT(game != nullptr);
    if (game != nullptr) {
        FileData* entry {new CollectionFileData(game, &collection)};
        entry->metadata.set("favorite", "true");
        collection.getRootFolder()->addChild(entry);
    }

    SystemData::sSystemVector.push_back(&collection);
    SystemData::captureAllPersonalStates();
    SystemData::sSystemVector.pop_back();

    EXPECT(!fs::exists(fixture.stateDirectory / "collections.json"));
    EXPECT(source->personalStore() != nullptr &&
           !source->personalStore()->hasPendingWrites());

    delete source;
    leaveProfileMode();
}

// The invalid-entry marker is a display state. It never reaches the store, and the stored label
// stays until a real choice is made.
TEST_CASE(profileInvalidMarkerIsNeverStored)
{
    Fixture fixture {"ngp"};
    fixture.enterProfileMode();

    SystemData* system {loadSystem(fixture)};
    ProfileState::JsonStore* store {system->personalStore()};
    EXPECT(store != nullptr);

    system->setAlternativeEmulator("<INVALID>Gone Emulator");
    system->storeAlternativeEmulator();

    EXPECT(!fs::exists(fixture.statePath()));
    if (store != nullptr) {
        EXPECT(!store->hasPendingWrites());
        EXPECT(!store->hasAlternativeEmulator());
    }

    // A real choice is stored and flushed.
    system->setAlternativeEmulator("RetroArch");
    system->storeAlternativeEmulator();

    EXPECT(fs::exists(fixture.statePath()));
    if (store != nullptr)
        EXPECT(store->alternativeEmulator() == "RetroArch");
    EXPECT(readFile(fixture.statePath()).find("\"alternativeEmulator\": \"RetroArch\"") !=
           std::string::npos);

    // The launch-on-other-screen choice is personal state of the same file, and the default is
    // omitted.
    system->setLaunchOnOtherScreen(false);
    system->storeLaunchOnOtherScreen();
    EXPECT(readFile(fixture.statePath()).find("\"launchOnOtherScreen\": false") !=
           std::string::npos);

    system->setLaunchOnOtherScreen(true);
    system->storeLaunchOnOtherScreen();
    EXPECT(readFile(fixture.statePath()).find("launchOnOtherScreen") == std::string::npos);

    delete system;
    leaveProfileMode();
}

// Legacy mode keeps its own writer and its settings gate. Nothing is written to a state file.
TEST_CASE(legacySavePointStillWritesTheGamelist)
{
    leaveProfileMode();
    Fixture fixture {"virtualboy"};

    // "never" writes nothing at a save point.
    Settings::getInstance()->setString("SaveGamelistsMode", "never");
    const fs::file_time_type neverTime {fs::last_write_time(fixture.gamelist)};

    SystemData* system {loadSystem(fixture)};
    FileData* alpha {findNode(*system, "Alpha Game (USA).sfc")};
    EXPECT(alpha != nullptr);
    if (alpha != nullptr) {
        alpha->metadata.set("favorite", "true");
        alpha->metadata.set("playcount", "7");
    }
    system->onMetaDataSavePoint();
    EXPECT(fs::last_write_time(fixture.gamelist) == neverTime);
    delete system;

    // "always" writes the shared gamelist, with the personal tags inside it.
    Settings::getInstance()->setString("SaveGamelistsMode", "always");
    SystemData* writer {loadSystem(fixture)};
    FileData* writerAlpha {findNode(*writer, "Alpha Game (USA).sfc")};
    EXPECT(writerAlpha != nullptr);
    if (writerAlpha != nullptr) {
        writerAlpha->metadata.set("favorite", "true");
        writerAlpha->metadata.set("playcount", "7");
    }
    writer->onMetaDataSavePoint();
    delete writer;

    const std::string written {readFile(fixture.gamelist)};
    EXPECT(written.find("<favorite>true</favorite>") != std::string::npos);
    EXPECT(written.find("<playcount>7</playcount>") != std::string::npos);
    EXPECT(!fs::exists(fixture.statePath()));
}

// A save point without a personal change writes nothing: no file is created, and the flush
// reports that there was nothing to do.
TEST_CASE(profileFlushWritesNothingWithoutChanges)
{
    Fixture fixture {"atari7800"};
    fixture.enterProfileMode();

    SystemData* system {loadSystem(fixture)};

    const ProfileState::FlushResult result {system->flushPersonalState()};
    EXPECT(result.status == ProfileState::FlushStatus::nothingToDo);
    EXPECT(result.reason.empty());
    EXPECT(!fs::exists(fixture.statePath()));

    system->onMetaDataSavePoint();
    system->writeMetaData();
    EXPECT(!fs::exists(fixture.statePath()));

    delete system;
    EXPECT(!fs::exists(fixture.statePath()));

    leaveProfileMode();
}

// The shutdown flush covers every open store, including a store whose SystemData was torn down
// earlier, for example by a rescan that removed the system.
TEST_CASE(profileShutdownFlushCoversStoresWithoutATree)
{
    Fixture fixture {"c64"};
    fixture.enterProfileMode();

    SystemData* system {loadSystem(fixture)};
    FileData* alpha {findNode(*system, "Alpha Game (USA).sfc")};
    EXPECT(alpha != nullptr);
    if (alpha != nullptr)
        alpha->metadata.set("favorite", "true");

    // Shutdown order: capture from the live tree, then the systems go away, then flush. The live
    // set is normally filled by loadConfig, so the test fills it for this call.
    SystemData::sSystemVector.push_back(system);
    SystemData::captureAllPersonalStates();
    SystemData::sSystemVector.pop_back();
    delete system;
    EXPECT(!fs::exists(fixture.statePath()));

    const auto failures {ProfileMode::flushStores()};
    EXPECT(failures.empty());
    EXPECT(fs::exists(fixture.statePath()));
    EXPECT(readFile(fixture.statePath()).find("\"favorite\": true") != std::string::npos);

    leaveProfileMode();
}

// An unresolved store accepts no capture, so the change flags stay set and the next save point
// still sees the pending change. Clearing them here would drop the edit without a write.
TEST_CASE(profileUnresolvedStoreKeepsPendingChanges)
{
    Fixture fixture {"zxspectrum"};

    // A version this reader does not support: the document is valid JSON but too new.
    const std::string tooNew {"{\n  \"schema\": \"nitropie.profile-state\",\n  \"version\": 99,\n"
                              "  \"system\": \"zxspectrum\",\n  \"systemPreferences\": {},\n"
                              "  \"entries\": []\n}\n"};
    writeFile(fixture.statePath(), tooNew);

    fixture.enterProfileMode();

    SystemData* system {loadSystem(fixture)};
    ProfileState::JsonStore* store {system->personalStore()};
    EXPECT(store != nullptr);
    if (store != nullptr)
        EXPECT(!store->isResolved());

    FileData* alpha {findNode(*system, "Alpha Game (USA).sfc")};
    EXPECT(alpha != nullptr);
    if (alpha != nullptr)
        alpha->metadata.set("favorite", "true");

    const ProfileState::FlushResult result {system->flushPersonalState()};
    EXPECT(result.status == ProfileState::FlushStatus::failure);
    EXPECT(!result.reason.empty());

    if (alpha != nullptr)
        EXPECT(alpha->metadata.wasChanged());
    // Nothing was written over the unusable file.
    EXPECT(readFile(fixture.statePath()) == tooNew);

    delete system;
    leaveProfileMode();
}

// A newly opened store is part of the process-wide gate. This is the state a rescan reaches when
// a configured system directory that was absent at startup appears and its state is unreadable:
// the already loaded systems have resolved stores, but the profile as a whole is unresolved.
TEST_CASE(profileNewlyOpenedUnresolvedStoreBlocksTheProfile)
{
    Fixture first {"saturn"};
    Fixture second {"dreamcast"};

    const std::string tooNew {"{\n  \"schema\": \"nitropie.profile-state\",\n  \"version\": "
                              "99,\n  \"system\": \"dreamcast\",\n  "
                              "\"systemPreferences\": {},\n  \"entries\": []\n}\n"};
    writeFile(second.statePath(), tooNew);

    first.enterProfileMode();

    SystemData* loaded {loadSystem(first)};
    EXPECT(ProfileMode::storesResolved());

    // The rescan discovers the second system and opens its store for the first time.
    SystemData* discovered {loadSystem(second)};
    EXPECT(!ProfileMode::storesResolved());

    delete discovered;
    delete loaded;
    leaveProfileMode();
}

// A partial recovery resolves one store but leaves another unresolved. The recovered store is
// dirty, yet the process-wide gate must keep every state byte untouched, including pending
// changes in the already loaded tree, until the whole profile resolves.
TEST_CASE(profilePartialRecoveryDoesNotPublishResolvedStores)
{
    Fixture first {"saturn"};
    Fixture second {"dreamcast"};

    const std::string corruptFirst {
        "{\n  \"schema\": \"nitropie.profile-state\",\n  \"version\": 99,\n  "
        "\"system\": \"saturn\",\n  \"systemPreferences\": {},\n  \"entries\": []\n}\n"};
    const std::string corruptSecond {
        "{\n  \"schema\": \"nitropie.profile-state\",\n  \"version\": 99,\n  "
        "\"system\": \"dreamcast\",\n  \"systemPreferences\": {},\n  \"entries\": []\n}\n"};
    const std::string backupFirst {testData("recovery-a-backup.json")};

    writeFile(first.statePath(), corruptFirst);
    writeFile(fs::path {first.statePath().string() + ".bak"}, backupFirst);
    writeFile(second.statePath(), corruptSecond);

    first.enterProfileMode();

    SystemData* recovered {loadSystem(first)};
    SystemData* unresolved {loadSystem(second)};
    EXPECT(!ProfileMode::storesResolved());

    // Only the first store can recover; the second has no backup.
    EXPECT(!ProfileMode::recoverStores());
    EXPECT(!ProfileMode::storesResolved());

    FileData* alpha {findNode(*recovered, "Alpha Game (USA).sfc")};
    EXPECT(alpha != nullptr);
    if (alpha != nullptr)
        alpha->metadata.set("favorite", "true");

    SystemData::sSystemVector.push_back(recovered);
    SystemData::sSystemVector.push_back(unresolved);
    SystemData::captureAllPersonalStates();
    SystemData::sSystemVector.pop_back();
    SystemData::sSystemVector.pop_back();

    const auto failures {ProfileMode::flushStores()};
    EXPECT(failures.size() == 2);
    if (failures.size() == 2) {
        EXPECT(failures.at(0).first == "dreamcast");
        EXPECT(failures.at(1).first == "saturn");
        EXPECT(!failures.at(0).second.empty());
        EXPECT(!failures.at(1).second.empty());
    }

    EXPECT(readFile(first.statePath()) == corruptFirst);
    EXPECT(readFile(second.statePath()) == corruptSecond);
    EXPECT(readFile(fs::path {first.statePath().string() + ".bak"}) == backupFirst);

    // Recovered state and the pending edit are still staged in memory, not dropped.
    if (recovered->personalStore() != nullptr)
        EXPECT(recovered->personalStore()->hasPendingWrites());
    if (alpha != nullptr)
        EXPECT(alpha->metadata.wasChanged());

    delete recovered;
    delete unresolved;
    leaveProfileMode();
}

// A session-level unresolved profile with no open stores still reports a failed flush instead of
// an empty, apparently successful result.
TEST_CASE(profileUnresolvedSessionReportsSkippedFlush)
{
    const fs::path session {fs::temp_directory_path() / "es-de-profile-unresolved-session"};
    fs::remove_all(session);
    const std::string catalog {(session / "missing.json").string()};
    setenv("NITROPIE_PROFILE_CATALOG", catalog.c_str(), 1);
    setenv("NITROPIE_PROFILE_ID", sProfileId, 1);
    ProfileMode::initialize();

    EXPECT(ProfileMode::isMode());
    EXPECT(!ProfileMode::storesResolved());

    const auto failures {ProfileMode::flushStores()};
    EXPECT(failures.size() == 1);
    if (failures.size() == 1) {
        EXPECT(failures.at(0).first == "profile");
        EXPECT(!failures.at(0).second.empty());
    }

    leaveProfileMode();
}

// A retry after the second store gains a usable backup resolves the whole profile, and only then
// are the recovered stores and the pending edit published.
TEST_CASE(profilePartialRecoveryRetryPublishesAllStores)
{
    Fixture first {"saturn"};
    Fixture second {"dreamcast"};

    const std::string corruptFirst {
        "{\n  \"schema\": \"nitropie.profile-state\",\n  \"version\": 99,\n  "
        "\"system\": \"saturn\",\n  \"systemPreferences\": {},\n  \"entries\": []\n}\n"};
    const std::string corruptSecond {
        "{\n  \"schema\": \"nitropie.profile-state\",\n  \"version\": 99,\n  "
        "\"system\": \"dreamcast\",\n  \"systemPreferences\": {},\n  \"entries\": []\n}\n"};
    const std::string backupFirst {testData("recovery-a-backup.json")};
    const std::string backupSecond {testData("recovery-b-backup.json")};

    writeFile(first.statePath(), corruptFirst);
    writeFile(fs::path {first.statePath().string() + ".bak"}, backupFirst);
    writeFile(second.statePath(), corruptSecond);

    first.enterProfileMode();

    SystemData* firstAttempt {loadSystem(first)};
    SystemData* secondAttempt {loadSystem(second)};
    EXPECT(!ProfileMode::recoverStores());

    // The second backup appears and the retry resolves every store.
    writeFile(fs::path {second.statePath().string() + ".bak"}, backupSecond);
    EXPECT(ProfileMode::recoverStores());
    EXPECT(ProfileMode::storesResolved());

    // The next rescan rebuilds the trees from the recovered stores before any capture happens.
    delete firstAttempt;
    delete secondAttempt;
    SystemData* firstSystem {loadSystem(first)};
    SystemData* secondSystem {loadSystem(second)};

    FileData* alpha {findNode(*firstSystem, "Alpha Game (USA).sfc")};
    EXPECT(alpha != nullptr);
    if (alpha != nullptr) {
        EXPECT(alpha->metadata.get("playcount") == "4");
        alpha->metadata.set("favorite", "true");
    }

    SystemData::sSystemVector.push_back(firstSystem);
    SystemData::sSystemVector.push_back(secondSystem);
    SystemData::captureAllPersonalStates();
    SystemData::sSystemVector.pop_back();
    SystemData::sSystemVector.pop_back();

    const auto failures {ProfileMode::flushStores()};
    EXPECT(failures.empty());

    EXPECT(readFile(first.statePath()) == testData("recovery-a-restored.json"));
    EXPECT(readFile(second.statePath()) == backupSecond);
    EXPECT(readFile(fs::path {first.statePath().string() + ".bak"}) == backupFirst);
    EXPECT(readFile(fs::path {second.statePath().string() + ".bak"}) == backupSecond);

    delete firstSystem;
    delete secondSystem;
    leaveProfileMode();
}

// A shared gamelist entry can name an absolute path that shares the system root's string prefix
// without a directory boundary. The legacy parser accepts it, but the profile key seam must not
// let it alias the valid descendant "other/Game.sfc" and overwrite that record.
TEST_CASE(profileOutsideRootSiblingCannotAliasAPersonalKey)
{
    Fixture fixture {"sega32x"};
    writeFile(fixture.roms / "other" / "zzz.sfc", "");

    // A legitimate descendant already has a personal record. The outside sibling below would map
    // to the same relative key with prefix stripping and overwrite it.
    const std::string existingState {
        "{\n  \"schema\": \"nitropie.profile-state\",\n  \"version\": 1,\n  "
        "\"system\": \"sega32x\",\n  \"systemPreferences\": {},\n  \"entries\": [\n"
        "    {\n      \"type\": \"game\",\n      \"path\": \"other/zzz.sfc\",\n      "
        "\"fields\": {\n        \"playcount\": 4\n      }\n    }\n  ]\n}\n"};
    writeFile(fixture.statePath(), existingState);

    fixture.enterProfileMode();

    SystemData* system {loadSystem(fixture)};
    FileData* legitimate {findNode(*system, "other/zzz.sfc")};
    EXPECT(legitimate != nullptr);
    if (legitimate != nullptr) {
        EXPECT(legitimate->metadata.get("playcount") == "4");
        EXPECT(!legitimate->metadata.wasChanged());
    }

    const std::string outsidePath {fixture.roms.string() + "-other/zzz.sfc"};
    FileData* outside {new FileData(GAME, outsidePath, system->getSystemEnvData(), system)};
    system->getRootFolder()->addChild(outside);
    outside->metadata.set("favorite", "true");

    // Nothing may be captured or written: the outside record has no key.
    const ProfileState::FlushResult result {system->flushPersonalState()};
    EXPECT(result.status == ProfileState::FlushStatus::nothingToDo);
    EXPECT(!outside->metadata.wasChanged());

    EXPECT(readFile(fixture.statePath()) == existingState);

    delete system;

    SystemData* reloaded {loadSystem(fixture)};
    FileData* reloadedLegitimate {findNode(*reloaded, "other/zzz.sfc")};
    EXPECT(reloadedLegitimate != nullptr);
    if (reloadedLegitimate != nullptr) {
        EXPECT(reloadedLegitimate->metadata.get("playcount") == "4");
        EXPECT(reloadedLegitimate->metadata.get("favorite") == "false");
    }

    delete reloaded;
    leaveProfileMode();
}

// The scraper's own writer is a protected seam. In profile mode it must refuse before mutating
// the shared metadata; in legacy mode the same result is applied.
TEST_CASE(profileScraperWriterRefusesInProfileMode)
{
    Fixture fixture {"pcenginecd"};
    fixture.enterProfileMode();

    SystemData* system {loadSystem(fixture)};
    FileData* game {findNode(*system, "Alpha Game (USA).sfc")};
    EXPECT(game != nullptr);

    MetaDataList metadata {GAME_METADATA};
    metadata.set("name", "Alpha Game");
    metadata.set("desc", "Original description");
    metadata.resetChangedFlag();

    ScraperSearchResult result;
    result.mdl.set("name", "Scraped Name");
    result.mdl.set("desc", "Scraped description");

    Settings::getInstance()->setBool("ScraperOverwriteData", true);
    Settings::getInstance()->setBool("ScrapeMetadata", true);
    Settings::getInstance()->setBool("ScrapeGameNames", true);

    EXPECT(!GuiScraperSearch::saveMetadata(result, metadata, game));
    EXPECT(metadata.get("name") == "Alpha Game");
    EXPECT(metadata.get("desc") == "Original description");
    EXPECT(!metadata.wasChanged());

    delete system;
    leaveProfileMode();

    // Legacy mode still applies the result through the same seam.
    Fixture legacy {"atari5200"};
    SystemData* legacySystem {loadSystem(legacy)};
    FileData* legacyGame {findNode(*legacySystem, "Alpha Game (USA).sfc")};
    EXPECT(legacyGame != nullptr);

    MetaDataList legacyMetadata {GAME_METADATA};
    legacyMetadata.set("name", "Alpha Game");
    legacyMetadata.set("desc", "Original description");
    legacyMetadata.resetChangedFlag();

    EXPECT(GuiScraperSearch::saveMetadata(result, legacyMetadata, legacyGame));
    EXPECT(legacyMetadata.get("name") == "Scraped Name");
    EXPECT(legacyMetadata.get("desc") == "Scraped description");

    delete legacySystem;
}

// Scraped media is written to the shared media directory. The resolver must refuse before it
// creates directories, removes existing files or writes the cached image; legacy still writes
// the exact bytes.
TEST_CASE(profileScraperMediaBoundaryRefusesInProfileMode)
{
    Fixture fixture {"wonderswan"};
    const fs::path mediaDirectory {fixture.root / "scraper-media"};
    Settings::getInstance()->setString("MediaDirectory", mediaDirectory.string());
    Settings::getInstance()->setBool("ScrapeCovers", true);
    Settings::getInstance()->setBool("ScraperOverwriteData", true);

    const std::string cover {testData("scraper-cover.png")};
    ScraperSearchResult result;
    result.coverUrl = "https://example.invalid/cover.png";
    result.coverFormat = ".png";
    result.thumbnailImageUrl = result.coverUrl;
    result.thumbnailImageData = cover;

    fixture.enterProfileMode();
    SystemData* system {loadSystem(fixture)};
    FileData* game {findNode(*system, "Alpha Game (USA).sfc")};
    ScraperSearchParams params;
    params.game = game;
    params.system = system;

    {
        std::unique_ptr<MDResolveHandle> handle {resolveMetaDataAssets(result, params)};
        handle->update();
        EXPECT(!handle->getSavedNewMedia());
    }
    EXPECT(!fs::exists(mediaDirectory));

    delete system;
    leaveProfileMode();

    // Legacy mode writes the same cached image to the exact shared-media path.
    Fixture legacy {"neogeocd"};
    SystemData* legacySystem {loadSystem(legacy)};
    FileData* legacyGame {findNode(*legacySystem, "Alpha Game (USA).sfc")};
    ScraperSearchParams legacyParams;
    legacyParams.game = legacyGame;
    legacyParams.system = legacySystem;

    {
        std::unique_ptr<MDResolveHandle> handle {resolveMetaDataAssets(result, legacyParams)};
        handle->update();
        EXPECT(handle->getSavedNewMedia());
    }

    const fs::path coverPath {mediaDirectory / "neogeocd" / "covers" / "Alpha Game (USA).png"};
    EXPECT(fs::exists(coverPath));
    EXPECT(readFile(coverPath) == cover);

    delete legacySystem;
    Settings::getInstance()->setString("MediaDirectory", "");
}

// The gamelist writer itself refuses to run in profile mode, whichever caller reaches it.
TEST_CASE(profileWriterRefusesTheSharedGamelist)
{
    Fixture fixture {"snes"};
    fixture.enterProfileMode();

    const std::string gamelistBytes {readFile(fixture.gamelist)};
    const fs::file_time_type gamelistTime {fs::last_write_time(fixture.gamelist)};

    SystemData* system {loadSystem(fixture)};
    FileData* alpha {findNode(*system, "Alpha Game (USA).sfc")};
    EXPECT(alpha != nullptr);
    if (alpha != nullptr)
        alpha->metadata.set("favorite", "true");

    GamelistFileParser::updateGamelist(system);

    EXPECT(readFile(fixture.gamelist) == gamelistBytes);
    EXPECT(fs::last_write_time(fixture.gamelist) == gamelistTime);
    EXPECT(!fs::exists(fixture.gamelist.string() + ".bak"));

    delete system;
    leaveProfileMode();
}

// The metadata editor edits the play count and the last-played value through a free-text popup,
// so a user can enter a value the state format cannot hold. Such a record must stay pending: the
// flag is not cleared, the stored value is kept, the rejection is reported, and correcting the
// value lets the next save point persist it.
TEST_CASE(profileUnencodableEditStaysPending)
{
    Fixture fixture {"snes"};
    fixture.enterProfileMode();

    SystemData* system {loadSystem(fixture)};
    ProfileState::JsonStore* store {system->personalStore()};
    EXPECT(store != nullptr);

    FileData* alpha {findNode(*system, "Alpha Game (USA).sfc")};
    EXPECT(alpha != nullptr);

    // "abc" and "-5" are both rejected: an integer must be a non-negative integer.
    for (const std::string value : {"abc", "-5"}) {
        if (alpha != nullptr)
            alpha->metadata.set("playcount", value);

        const ProfileState::FlushResult result {system->flushPersonalState()};
        // The rejection is a failure, not a quiet success: the user is told, and nothing was
        // staged for that field so the file is untouched.
        EXPECT(result.status == ProfileState::FlushStatus::failure);
        EXPECT(!result.reason.empty());
        // The edit is not dropped: the next save point retries it.
        if (alpha != nullptr)
            EXPECT(alpha->metadata.wasChanged());
        EXPECT(!fs::exists(fixture.statePath()));

        // The rejection is reported with the field that could not be written.
        bool reported {false};
        if (store != nullptr) {
            for (const std::string& error : store->loadErrors())
                reported = reported || error.find("playcount") != std::string::npos;
        }
        EXPECT(reported);
    }

    // A value in the state format is accepted, the flag clears and the document is written.
    if (alpha != nullptr)
        alpha->metadata.set("playcount", "4");
    const ProfileState::FlushResult result {system->flushPersonalState()};
    EXPECT(result.status == ProfileState::FlushStatus::success);
    if (alpha != nullptr)
        EXPECT(!alpha->metadata.wasChanged());
    EXPECT(fs::exists(fixture.statePath()));
    EXPECT(readFile(fixture.statePath()).find("\"playcount\": 4") != std::string::npos);

    delete system;
    leaveProfileMode();
}

// The editor refuses an unencodable personal value before it is committed, and it uses the same
// rule as the state writer, so the two can never disagree.
TEST_CASE(profileValueValidationMatchesTheStateFormat)
{
    const auto decl = [](const MetaDataListType type, const std::string& key) -> const MetaDataDecl& {
        for (const MetaDataDecl& candidate : getMDDByType(type)) {
            if (candidate.key == key)
                return candidate;
        }
        static const MetaDataDecl missing {"", MD_STRING, "", false, "", "", false, MD_OWNER_SHARED};
        return missing;
    };

    // playcount and playtime are non-negative integers.
    EXPECT(ProfileState::isEncodableValue(decl(GAME_METADATA, "playcount"), "0"));
    EXPECT(ProfileState::isEncodableValue(decl(GAME_METADATA, "playcount"), "42"));
    EXPECT(!ProfileState::isEncodableValue(decl(GAME_METADATA, "playcount"), "abc"));
    EXPECT(!ProfileState::isEncodableValue(decl(GAME_METADATA, "playcount"), "-5"));
    EXPECT(!ProfileState::isEncodableValue(decl(GAME_METADATA, "playcount"), "4.5"));
    EXPECT(!ProfileState::isEncodableValue(decl(GAME_METADATA, "playtime"), "12s"));

    // lastplayed is the same timestamp form the gamelist uses.
    EXPECT(ProfileState::isEncodableValue(decl(GAME_METADATA, "lastplayed"),
                                          "20250102T030405"));
    EXPECT(!ProfileState::isEncodableValue(decl(GAME_METADATA, "lastplayed"), "2025-01-02"));
    EXPECT(!ProfileState::isEncodableValue(decl(GAME_METADATA, "lastplayed"), "yesterday"));

    // Personal free-text fields must be valid UTF-8; booleans are always representable.
    EXPECT(ProfileState::isEncodableValue(decl(GAME_METADATA, "collectionsortname"), "Hacks"));
    EXPECT(!ProfileState::isEncodableValue(decl(GAME_METADATA, "collectionsortname"),
                                           std::string {"bad\xfftext"}));
    EXPECT(ProfileState::isEncodableValue(decl(GAME_METADATA, "favorite"), "true"));
}

// A rejected value stays reported after the tree is torn down, so it cannot disappear without a
// trace at a rescan or at shutdown.
TEST_CASE(profileRejectedValueIsReportedAfterTeardown)
{
    Fixture fixture {"snes"};
    fixture.enterProfileMode();

    SystemData* system {loadSystem(fixture)};
    ProfileState::JsonStore* store {system->personalStore()};
    EXPECT(store != nullptr);

    FileData* alpha {findNode(*system, "Alpha Game (USA).sfc")};
    EXPECT(alpha != nullptr);
    if (alpha != nullptr)
        alpha->metadata.set("playcount", "not a number");

    // The save point reports the rejection and leaves the value pending.
    EXPECT(system->flushPersonalState().status == ProfileState::FlushStatus::failure);

    // The tree goes away, as it does on a rescan or at shutdown.
    delete system;

    bool reported {false};
    if (store != nullptr) {
        for (const std::string& error : store->loadErrors())
            reported = reported || error.find("playcount") != std::string::npos;
    }
    EXPECT(reported);
    // Nothing unencodable was written.
    EXPECT(!fs::exists(fixture.statePath()));

    leaveProfileMode();
}
