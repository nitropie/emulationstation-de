//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileStateJsonStoreTests.cpp
//
//  Test cases for the JSON personal-state store: exact file bytes, unknown data, wrong types,
//  corrupt and too-new files, atomic writes with injected failures, restart and backup recovery.
//

#include "TestHarness.h"

#include "MetaData.h"
#include "profiles/ProfileStateFile.h"
#include "profiles/ProfileStateJsonStore.h"
#include "profiles/ProfileStateStore.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "rapidjson/reader.h"

namespace
{
    namespace fs = std::filesystem;

    int sDirectoryCounter {0};

    fs::path makeTempDirectory()
    {
        const fs::path path {fs::temp_directory_path() /
                             ("es-de-profile-state-" + std::to_string(++sDirectoryCounter))};
        fs::remove_all(path);
        fs::create_directories(path);
        return path;
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
        return readFile(fs::path {ES_APP_TEST_DATA_DIR} / "profile-state" / name);
    }

    ProfileState::Session session(const fs::path& directory, const std::string& system)
    {
        ProfileState::Session result;
        result.profileId = "6f1c2a6e-0e8a-4c5b-9d0a-6f8f0e3b1c2d";
        result.profileRoot = directory.string();
        result.stateDirectory = (directory / "profile-state").string();
        result.system = system;
        fs::create_directories(result.stateDirectory);
        return result;
    }

    ProfileRecordKey key(const std::string& system,
                         const ProfileRecordType type,
                         const std::string& path)
    {
        return ProfileRecordKey::fromPath(system, type, path).value();
    }

    MetaDataList gameRecord() { return MetaDataList {GAME_METADATA}; }

    class FixedClock final : public ProfileState::Clock
    {
    public:
        std::string stamp {"20261118T120000"};
        std::string utcTimestamp() override { return stamp; }
    };

    class FaultyFiles final : public ProfileState::FileOperations
    {
    public:
        bool failRename {false};
        bool failDirectorySync {false};
        bool failWrite {false};
        std::string failWriteSuffix;
        std::string failReadSuffix;
        std::string failRenameContaining;

        ProfileState::ReadStatus readFile(const std::string& path, std::string& contents) override
        {
            if (!failReadSuffix.empty() && path.size() >= failReadSuffix.size() &&
                path.compare(path.size() - failReadSuffix.size(), failReadSuffix.size(),
                             failReadSuffix) == 0)
                return ProfileState::ReadStatus::error;
            return ProfileState::systemFileOperations().readFile(path, contents);
        }

        bool writeFile(const std::string& path, const std::string& contents) override
        {
            if (failWrite)
                return false;
            if (!failWriteSuffix.empty() && path.size() >= failWriteSuffix.size() &&
                path.compare(path.size() - failWriteSuffix.size(), failWriteSuffix.size(),
                             failWriteSuffix) == 0)
                return false;
            return ProfileState::systemFileOperations().writeFile(path, contents);
        }

        bool syncFile(const std::string& path) override
        {
            return ProfileState::systemFileOperations().syncFile(path);
        }

        bool renameFile(const std::string& from, const std::string& to) override
        {
            if (failRename)
                return false;
            if (!failRenameContaining.empty() && to.find(failRenameContaining) != std::string::npos)
                return false;
            return ProfileState::systemFileOperations().renameFile(from, to);
        }

        bool syncDirectory(const std::string& directory) override
        {
            if (failDirectorySync)
                return false;
            return ProfileState::systemFileOperations().syncDirectory(directory);
        }

        bool removeFile(const std::string& path) override
        {
            return ProfileState::systemFileOperations().removeFile(path);
        }
    };
} // namespace

TEST_CASE(exactFileBytesMatchTheGolden)
{
    const fs::path directory {makeTempDirectory()};
    ProfileState::FileOperations& files {ProfileState::systemFileOperations()};
    ProfileState::JsonStore store {session(directory, "snes"), files};

    store.setAlternativeEmulator("Snes9x - Current");
    store.setLaunchOnOtherScreen(false);

    MetaDataList kaizo {gameRecord()};
    kaizo.set("playcount", "4");
    kaizo.set("playtime", "5400");
    kaizo.set("lastplayed", "20261116T203000");
    store.capture(key("snes", ProfileRecordType::game, "Hacks/Kaizo Mario World.sfc"), kaizo);

    MetaDataList mario {gameRecord()};
    mario.set("favorite", "true");
    mario.set("completed", "true");
    mario.set("playcount", "12");
    mario.set("lastplayed", "20261115T191500");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), mario);

    const ProfileState::FlushResult result {store.flush()};
    EXPECT(result.status == ProfileState::FlushStatus::success);
    EXPECT(result.reason.empty());

    const std::string actual {readFile(store.statePath())};
    const std::string golden {testData("snes.json")};
    if (actual != golden)
        std::printf("--- actual ---\n%s\n--- expected ---\n%s\n", actual.c_str(), golden.c_str());
    EXPECT(actual == golden);
    fs::remove_all(directory);
}

TEST_CASE(serializingAnUnchangedGoldenIsIdempotent)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("snes.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    EXPECT(store.isResolved());
    EXPECT(store.loadErrors().empty());

    // Force exactly one write of the unchanged document.
    store.setLaunchOnOtherScreen(true);
    store.setLaunchOnOtherScreen(false);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    const std::string after {readFile(stateFile)};
    EXPECT(after == testData("snes.json"));
    fs::remove_all(directory);
}

TEST_CASE(roundTripKeepsUnknownKeysAtEveryLevel)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("unknown-keys.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    EXPECT(store.isResolved());

    MetaDataList mario {gameRecord()};
    mario.set("favorite", "true");
    mario.set("playcount", "3");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), mario);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);

    const std::string written {readFile(stateFile)};
    EXPECT(written.find("\"futureTop\"") != std::string::npos);
    EXPECT(written.find("\"unknown\": true") != std::string::npos);
    EXPECT(written.find("\"futurePreference\"") != std::string::npos);
    EXPECT(written.find("\"mode\": \"keep-me\"") != std::string::npos);
    EXPECT(written.find("\"futureField\"") != std::string::npos);
    EXPECT(written.find("\"futureEntry\": \"keep me too\"") != std::string::npos);
    EXPECT(written.find("\"playcount\": 3") != std::string::npos);
    EXPECT(written.find("\"favorite\": true") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE(wrongTypeRecordIsKeptAndReported)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("wrong-type.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    EXPECT(store.isResolved());
    EXPECT(store.loadErrors().size() == 1);
    if (!store.loadErrors().empty()) {
        EXPECT(store.loadErrors().front().find("Broken (USA).sfc") != std::string::npos);
        EXPECT(store.loadErrors().front().find("playcount") != std::string::npos);
    }

    MetaDataList broken {gameRecord()};
    store.apply(key("snes", ProfileRecordType::game, "Broken (USA).sfc"), broken);
    EXPECT(broken.get("playcount") == "0");
    EXPECT(broken.get("favorite") == "false");

    MetaDataList mario {gameRecord()};
    store.apply(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), mario);
    EXPECT(mario.get("favorite") == "true");

    // A write for another record keeps the wrong-typed record verbatim.
    MetaDataList changed {gameRecord()};
    changed.set("favorite", "true");
    changed.set("playcount", "9");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), changed);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);

    const std::string written {readFile(stateFile)};
    EXPECT(written.find("\"playcount\": \"many\"") != std::string::npos);
    EXPECT(written.find("\"playcount\": 9") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE(tooNewVersionIsRefused)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("too-new.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    EXPECT(!store.isResolved());
    EXPECT(!store.loadErrors().empty());
    EXPECT(!store.canSwitchProfile());

    MetaDataList mario {gameRecord()};
    mario.set("favorite", "true");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), mario);
    const ProfileState::FlushResult result {store.flush()};
    EXPECT(result.status == ProfileState::FlushStatus::failure);
    EXPECT(!result.reason.empty());
    EXPECT(!store.hasPendingWrites());
    EXPECT(readFile(stateFile) == testData("too-new.json"));
    fs::remove_all(directory);
}

TEST_CASE(corruptEmptyAndTruncatedFilesAreNotReplaced)
{
    const std::vector<std::string> broken {
        "not json at all\n",
        "",
        "{\n  \"schema\": \"nitropie.profile-state\",\n  \"version\": 1,\n  \"system\": "
        "\"snes\",\n  \"entries\": [\n",
    };

    for (const std::string& contents : broken) {
        const fs::path directory {makeTempDirectory()};
        const fs::path stateFile {directory / "profile-state" / "snes.json"};
        writeFile(stateFile, contents);

        ProfileState::JsonStore store {session(directory, "snes"),
                                       ProfileState::systemFileOperations()};
        EXPECT(!store.isResolved());
        EXPECT(!store.loadErrors().empty());

        const ProfileState::FlushResult result {store.flush()};
        EXPECT(result.status == ProfileState::FlushStatus::failure);
        EXPECT(readFile(stateFile) == contents);
        fs::remove_all(directory);
    }
}

TEST_CASE(renameFailureKeepsDirtyStateAndPreviousFile)
{
    const fs::path directory {makeTempDirectory()};
    FaultyFiles files;
    ProfileState::JsonStore store {session(directory, "snes"), files};

    MetaDataList first {gameRecord()};
    first.set("favorite", "true");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), first);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    const std::string previous {readFile(store.statePath())};

    MetaDataList second {gameRecord()};
    second.set("playcount", "7");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), second);
    files.failRename = true;

    const ProfileState::FlushResult result {store.flush()};
    EXPECT(result.status == ProfileState::FlushStatus::failure);
    EXPECT(result.reason.find("rename") != std::string::npos);
    EXPECT(store.hasPendingWrites());
    EXPECT(!store.canSwitchProfile());
    EXPECT(readFile(store.statePath()) == previous);

    files.failRename = false;
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    EXPECT(readFile(store.statePath()).find("\"playcount\": 7") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE(directorySyncFailureKeepsDirtyState)
{
    const fs::path directory {makeTempDirectory()};
    FaultyFiles files;
    ProfileState::JsonStore store {session(directory, "snes"), files};

    MetaDataList first {gameRecord()};
    first.set("favorite", "true");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), first);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    const std::string previous {readFile(store.statePath())};

    MetaDataList second {gameRecord()};
    second.set("playcount", "7");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), second);
    files.failDirectorySync = true;

    const ProfileState::FlushResult result {store.flush()};
    EXPECT(result.status == ProfileState::FlushStatus::failure);
    EXPECT(result.reason.find("sync") != std::string::npos);
    EXPECT(store.hasPendingWrites());
    EXPECT(!store.canSwitchProfile());
    // The last known good copy is still the previous state.
    EXPECT(readFile(store.statePath() + ".bak") == previous);

    files.failDirectorySync = false;
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    EXPECT(readFile(store.statePath()).find("\"playcount\": 7") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE(restartReloadsTheState)
{
    const fs::path directory {makeTempDirectory()};
    ProfileState::FileOperations& files {ProfileState::systemFileOperations()};

    {
        ProfileState::JsonStore store {session(directory, "snes"), files};
        MetaDataList mario {gameRecord()};
        mario.set("favorite", "true");
        mario.set("playcount", "5");
        mario.set("lastplayed", "20261115T191500");
        store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), mario);
        EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    }

    ProfileState::JsonStore reloaded {session(directory, "snes"), files};
    EXPECT(reloaded.isResolved());
    EXPECT(!reloaded.hasPendingWrites());

    MetaDataList mario {gameRecord()};
    reloaded.apply(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), mario);
    EXPECT(mario.get("favorite") == "true");
    EXPECT(mario.get("playcount") == "5");
    EXPECT(mario.get("lastplayed") == "20261115T191500");
    EXPECT(mario.get("completed") == "false");
    fs::remove_all(directory);
}

TEST_CASE(backupRecoveryRestoresTheLastGoodCopy)
{
    const fs::path directory {makeTempDirectory()};
    ProfileState::FileOperations& files {ProfileState::systemFileOperations()};
    ProfileState::JsonStore store {session(directory, "snes"), files};

    MetaDataList first {gameRecord()};
    first.set("favorite", "true");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), first);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    const std::string good {readFile(store.statePath())};

    MetaDataList second {gameRecord()};
    second.set("playcount", "7");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), second);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    EXPECT(readFile(store.statePath() + ".bak") == good);

    writeFile(store.statePath(), "corrupt\n");
    {
        ProfileState::JsonStore broken {session(directory, "snes"), files};
        EXPECT(!broken.isResolved());
        EXPECT(broken.recoverFromBackup());
        EXPECT(broken.isResolved());
        EXPECT(broken.hasPendingWrites());
        EXPECT(broken.flush().status == ProfileState::FlushStatus::success);
    }

    EXPECT(readFile(store.statePath()) == good);
    fs::remove_all(directory);
}

TEST_CASE(twoProfilesDoNotShareState)
{
    const fs::path aliceDirectory {makeTempDirectory()};
    const fs::path bobDirectory {makeTempDirectory()};
    ProfileState::FileOperations& files {ProfileState::systemFileOperations()};

    ProfileState::JsonStore alice {session(aliceDirectory, "snes"), files};
    ProfileState::JsonStore bob {session(bobDirectory, "snes"), files};

    MetaDataList aliceRecord {gameRecord()};
    aliceRecord.set("favorite", "true");
    aliceRecord.set("playcount", "11");
    alice.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), aliceRecord);
    EXPECT(alice.flush().status == ProfileState::FlushStatus::success);

    EXPECT(!bob.hasPendingWrites());
    MetaDataList bobRecord {gameRecord()};
    bob.apply(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), bobRecord);
    EXPECT(bobRecord.get("favorite") == "false");
    EXPECT(bobRecord.get("playcount") == "0");

    EXPECT(!fs::exists(bob.statePath()));
    EXPECT(readFile(alice.statePath()).find("\"playcount\": 11") != std::string::npos);
    fs::remove_all(aliceDirectory);
    fs::remove_all(bobDirectory);
}

TEST_CASE(serializedOrderingIsPinned)
{
    // Entries out of path order, a game and a folder at the same path in the reverse of the
    // expected order, and unknown keys at every level, all unsorted.
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("ordering-input.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    EXPECT(store.isResolved());

    // Force exactly one write of the unchanged document.
    store.setAlternativeEmulator("Temporary");
    store.setAlternativeEmulator("Snes9x - Current");
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);

    const std::string actual {readFile(stateFile)};
    if (actual != testData("ordering-golden.json"))
        std::printf("--- actual ---\n%s\n", actual.c_str());
    EXPECT(actual == testData("ordering-golden.json"));
    fs::remove_all(directory);
}

TEST_CASE(unchangedCaptureKeepsAWrongTypedRecord)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("wrong-type.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    EXPECT(store.isResolved());

    MetaDataList unchanged {gameRecord()};
    store.capture(key("snes", ProfileRecordType::game, "Broken (USA).sfc"), unchanged);

    EXPECT(!store.hasPendingWrites());
    EXPECT(store.flush().status == ProfileState::FlushStatus::nothingToDo);
    EXPECT(readFile(stateFile) == testData("wrong-type.json"));
    fs::remove_all(directory);
}

TEST_CASE(captureKeepsAWrongTypedFieldWhenAnotherFieldChanges)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("wrong-type.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    MetaDataList record {gameRecord()};
    record.set("favorite", "true");
    store.capture(key("snes", ProfileRecordType::game, "Broken (USA).sfc"), record);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);

    const std::string actual {readFile(stateFile)};
    if (actual != testData("wrong-type-favorite.json"))
        std::printf("--- actual ---\n%s\n", actual.c_str());
    EXPECT(actual == testData("wrong-type-favorite.json"));
    fs::remove_all(directory);
}

TEST_CASE(captureReplacesAWrongTypedFieldWithARealValue)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("wrong-type.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    MetaDataList record {gameRecord()};
    record.set("playcount", "9");
    store.capture(key("snes", ProfileRecordType::game, "Broken (USA).sfc"), record);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);

    const std::string actual {readFile(stateFile)};
    if (actual != testData("wrong-type-fixed.json"))
        std::printf("--- actual ---\n%s\n", actual.c_str());
    EXPECT(actual == testData("wrong-type-fixed.json"));
    fs::remove_all(directory);
}

TEST_CASE(flushWithoutStateDirectoryFailsClearly)
{
    const fs::path directory {makeTempDirectory()};

    ProfileState::Session missing;
    missing.profileId = "6f1c2a6e-0e8a-4c5b-9d0a-6f8f0e3b1c2d";
    missing.profileRoot = directory.string();
    missing.stateDirectory = (directory / "missing-state").string();
    missing.system = "snes";

    ProfileState::JsonStore store {missing, ProfileState::systemFileOperations()};
    EXPECT(store.isResolved());

    MetaDataList record {gameRecord()};
    record.set("favorite", "true");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), record);
    EXPECT(store.hasPendingWrites());

    const ProfileState::FlushResult result {store.flush()};
    EXPECT(result.status == ProfileState::FlushStatus::failure);
    EXPECT(!result.reason.empty());
    EXPECT(result.reason.find("could not write") != std::string::npos);
    EXPECT(store.hasPendingWrites());
    EXPECT(!store.canSwitchProfile());
    fs::remove_all(directory);
}

TEST_CASE(preferencesRoundTrip)
{
    const fs::path directory {makeTempDirectory()};
    ProfileState::FileOperations& files {ProfileState::systemFileOperations()};

    {
        ProfileState::JsonStore store {session(directory, "snes"), files};
        store.setAlternativeEmulator("Snes9x - Current");
        store.setLaunchOnOtherScreen(false);
        EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    }

    ProfileState::JsonStore reloaded {session(directory, "snes"), files};
    EXPECT(reloaded.hasAlternativeEmulator());
    EXPECT(reloaded.alternativeEmulator() == "Snes9x - Current");
    EXPECT(reloaded.hasLaunchOnOtherScreen());
    EXPECT(!reloaded.launchOnOtherScreen());
    fs::remove_all(directory);
}

TEST_CASE(capturingDefaultsDropsTheEntry)
{
    const fs::path directory {makeTempDirectory()};
    ProfileState::FileOperations& files {ProfileState::systemFileOperations()};
    ProfileState::JsonStore store {session(directory, "snes"), files};

    MetaDataList record {gameRecord()};
    record.set("favorite", "true");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), record);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    EXPECT(readFile(store.statePath()).find("Super Mario World") != std::string::npos);

    MetaDataList defaults {gameRecord()};
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), defaults);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    EXPECT(readFile(store.statePath()).find("Super Mario World") == std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE(folderAndGameRecordsAreSeparate)
{
    const fs::path directory {makeTempDirectory()};
    ProfileState::FileOperations& files {ProfileState::systemFileOperations()};
    ProfileState::JsonStore store {session(directory, "snes"), files};

    MetaDataList game {gameRecord()};
    game.set("favorite", "true");
    store.capture(key("snes", ProfileRecordType::game, "Hacks"), game);

    MetaDataList folder {FOLDER_METADATA};
    folder.set("hidden", "true");
    store.capture(key("snes", ProfileRecordType::folder, "Hacks"), folder);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);

    MetaDataList gameView {gameRecord()};
    store.apply(key("snes", ProfileRecordType::game, "Hacks"), gameView);
    EXPECT(gameView.get("favorite") == "true");
    EXPECT(gameView.get("hidden") == "false");

    MetaDataList folderView {FOLDER_METADATA};
    store.apply(key("snes", ProfileRecordType::folder, "Hacks"), folderView);
    EXPECT(folderView.get("favorite") == "false");
    EXPECT(folderView.get("hidden") == "true");
    fs::remove_all(directory);
}

TEST_CASE(wrongTypeSiblingFieldsAreApplied)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("wrong-type-sibling.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    EXPECT(store.isResolved());
    EXPECT(store.loadErrors().size() == 1);

    MetaDataList record {gameRecord()};
    store.apply(key("snes", ProfileRecordType::game, "Broken (USA).sfc"), record);
    EXPECT(record.get("favorite") == "true");
    EXPECT(record.get("playcount") == "0");
    fs::remove_all(directory);
}

TEST_CASE(defaultPreferencesAreOmitted)
{
    const fs::path directory {makeTempDirectory()};
    ProfileState::FileOperations& files {ProfileState::systemFileOperations()};
    ProfileState::JsonStore store {session(directory, "snes"), files};

    store.setAlternativeEmulator("");
    store.setLaunchOnOtherScreen(true);
    EXPECT(!store.hasPendingWrites());
    EXPECT(store.flush().status == ProfileState::FlushStatus::nothingToDo);

    MetaDataList record {gameRecord()};
    record.set("favorite", "true");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), record);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    EXPECT(readFile(store.statePath()).find("\"systemPreferences\": {}") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE(duplicateMemberNamesMakeTheDocumentInvalid)
{
    {
        const fs::path directory {makeTempDirectory()};
        const fs::path stateFile {directory / "profile-state" / "snes.json"};
        writeFile(stateFile, testData("duplicate-keys.json"));

        ProfileState::JsonStore store {session(directory, "snes"),
                                       ProfileState::systemFileOperations()};
        EXPECT(!store.isResolved());
        EXPECT(!store.loadErrors().empty());
        EXPECT(store.flush().status == ProfileState::FlushStatus::failure);
        EXPECT(readFile(stateFile) == testData("duplicate-keys.json"));
        fs::remove_all(directory);
    }

    {
        const fs::path directory {makeTempDirectory()};
        const fs::path stateFile {directory / "profile-state" / "snes.json"};
        writeFile(stateFile,
                  "{\n  \"schema\": \"nitropie.profile-state\",\n  \"version\": 1,\n"
                  "  \"system\": \"snes\",\n  \"entries\": [\n    {\n      \"type\": \"game\",\n"
                  "      \"path\": \"Game.sfc\",\n      \"fields\": {\n"
                  "        \"favorite\": true,\n        \"favorite\": false\n      }\n"
                  "    }\n  ]\n}\n");

        ProfileState::JsonStore store {session(directory, "snes"),
                                       ProfileState::systemFileOperations()};
        EXPECT(!store.isResolved());
        fs::remove_all(directory);
    }
}

TEST_CASE(duplicateEntriesMakeTheDocumentInvalid)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile,
              "{\n  \"schema\": \"nitropie.profile-state\",\n  \"version\": 1,\n"
              "  \"system\": \"snes\",\n  \"entries\": [\n    {\n      \"type\": \"game\",\n"
              "      \"path\": \"Game.sfc\",\n      \"fields\": {}\n    },\n    {\n"
              "      \"type\": \"game\",\n      \"path\": \"Game.sfc\",\n"
              "      \"fields\": {}\n    }\n  ]\n}\n");

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    EXPECT(!store.isResolved());
    fs::remove_all(directory);
}

namespace
{
    struct DuplicateKeyScanner
        : public rapidjson::BaseReaderHandler<rapidjson::UTF8<>, DuplicateKeyScanner> {
        std::vector<std::set<std::string>> scopes;
        std::string duplicate;

        bool StartObject()
        {
            scopes.emplace_back();
            return true;
        }

        bool EndObject(rapidjson::SizeType)
        {
            scopes.pop_back();
            return true;
        }

        bool StartArray() { return true; }
        bool EndArray(rapidjson::SizeType) { return true; }

        bool Key(const char* text, rapidjson::SizeType length, bool)
        {
            if (scopes.empty())
                return true;
            const std::string key {text, length};
            if (!scopes.back().insert(key).second) {
                duplicate = key;
                return false;
            }
            return true;
        }

        bool String(const char*, rapidjson::SizeType, bool) { return true; }
        bool Bool(bool) { return true; }
        bool Int(int) { return true; }
        bool Uint(unsigned int) { return true; }
        bool Int64(std::int64_t) { return true; }
        bool Uint64(std::uint64_t) { return true; }
        bool Double(double) { return true; }
        bool RawNumber(const char*, rapidjson::SizeType, bool) { return true; }
        bool Null() { return true; }
    };

    std::string duplicateKeyOf(const std::string& json)
    {
        DuplicateKeyScanner scanner;
        rapidjson::Reader reader;
        rapidjson::StringStream stream {json.c_str()};
        reader.Parse<rapidjson::kParseValidateEncodingFlag>(stream, scanner);
        return scanner.duplicate;
    }
} // namespace

TEST_CASE(serializedOutputHasNoDuplicateKeys)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("unknown-keys.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    store.setLaunchOnOtherScreen(false);
    MetaDataList record {gameRecord()};
    record.set("favorite", "true");
    record.set("playcount", "3");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), record);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);

    const std::string written {readFile(stateFile)};
    EXPECT(duplicateKeyOf(written).empty());
    fs::remove_all(directory);
}

TEST_CASE(invalidUtf8FileIsUnresolved)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("invalid-utf8.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    EXPECT(!store.isResolved());
    EXPECT(!store.loadErrors().empty());
    EXPECT(readFile(stateFile) == testData("invalid-utf8.json"));
    fs::remove_all(directory);
}

TEST_CASE(nonUtf8PathsAreRejected)
{
    EXPECT(!ProfileRecordKey::fromPath("snes", ProfileRecordType::game, "Caf\xe9 (USA).sfc")
                .has_value());
    EXPECT(
        ProfileRecordKey::fromPath("snes", ProfileRecordType::game, "Café (USA).sfc").has_value());
}

TEST_CASE(timeFieldsMustUseTheTimestampFormat)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("bad-time.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    EXPECT(store.isResolved());
    EXPECT(store.loadErrors().size() == 1);
    EXPECT(store.loadErrors().front().find("lastplayed") != std::string::npos);

    MetaDataList record {gameRecord()};
    store.apply(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), record);
    EXPECT(record.get("lastplayed") == "0");

    MetaDataList changed {gameRecord()};
    changed.set("favorite", "true");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), changed);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    EXPECT(readFile(stateFile).find("\"lastplayed\": \"2026-11-16\"") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE(negativeAndOversizedIntegersAreWrongTyped)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("bad-int.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    EXPECT(store.isResolved());
    EXPECT(store.loadErrors().size() == 2);

    MetaDataList negative {gameRecord()};
    store.apply(key("snes", ProfileRecordType::game, "Negative (USA).sfc"), negative);
    EXPECT(negative.get("playcount") == "0");

    MetaDataList huge {gameRecord()};
    store.apply(key("snes", ProfileRecordType::game, "Huge (USA).sfc"), huge);
    EXPECT(huge.get("playtime") == "0");
    fs::remove_all(directory);
}

TEST_CASE(backupTempWriteFailureLeavesBothFilesUntouched)
{
    const fs::path directory {makeTempDirectory()};
    FaultyFiles files;
    ProfileState::JsonStore store {session(directory, "snes"), files};

    MetaDataList first {gameRecord()};
    first.set("favorite", "true");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), first);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);

    MetaDataList second {gameRecord()};
    second.set("playcount", "7");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), second);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    const std::string mainBefore {readFile(store.statePath())};
    const std::string backupBefore {readFile(store.statePath() + ".bak")};

    MetaDataList third {gameRecord()};
    third.set("playcount", "8");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), third);
    files.failWriteSuffix = ".bak.tmp";

    const ProfileState::FlushResult result {store.flush()};
    EXPECT(result.status == ProfileState::FlushStatus::failure);
    EXPECT(result.reason.find(".bak.tmp") != std::string::npos);
    EXPECT(store.hasPendingWrites());
    EXPECT(readFile(store.statePath()) == mainBefore);
    EXPECT(readFile(store.statePath() + ".bak") == backupBefore);

    files.failWriteSuffix.clear();
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    EXPECT(readFile(store.statePath()).find("\"playcount\": 8") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE(missingMainFileWithBackupIsUnresolved)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    const fs::path backupFile {stateFile.string() + ".bak"};
    writeFile(backupFile, testData("snes.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    EXPECT(!store.isResolved());
    EXPECT(!store.loadErrors().empty());
    EXPECT(!store.canSwitchProfile());
    EXPECT(!fs::exists(stateFile));

    EXPECT(store.recoverFromBackup());
    EXPECT(store.isResolved());
    const std::string backup {readFile(backupFile)};
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    EXPECT(readFile(stateFile) == backup);
    EXPECT(readFile(backupFile) == backup);
    fs::remove_all(directory);
}

TEST_CASE(editErrorsSurviveLaterCaptures)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("snes.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};

    MetaDataList bad {gameRecord()};
    bad.set("playcount", "-3");
    store.capture(key("snes", ProfileRecordType::game, "Hacks/Kaizo Mario World.sfc"), bad);
    EXPECT(store.loadErrors().size() == 1);

    MetaDataList clean {gameRecord()};
    clean.set("playcount", "5");
    store.capture(key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc"), clean);

    EXPECT(store.loadErrors().size() == 1);
    EXPECT(store.loadErrors().front().find("Hacks/Kaizo Mario World.sfc") != std::string::npos);
    fs::remove_all(directory);
}

TEST_CASE(unencodableNewRecordReportsWithoutDirtying)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("snes.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};

    MetaDataList bad {gameRecord()};
    bad.set("playcount", "-1");
    store.capture(key("snes", ProfileRecordType::game, "New (USA).sfc"), bad);

    EXPECT(store.loadErrors().size() == 1);
    EXPECT(store.loadErrors().front().find("New (USA).sfc") != std::string::npos);
    EXPECT(!store.hasPendingWrites());
    EXPECT(store.flush().status == ProfileState::FlushStatus::nothingToDo);
    EXPECT(readFile(stateFile) == testData("snes.json"));
    fs::remove_all(directory);
}

TEST_CASE(recapturingClearsTheRecordEditError)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("snes.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};

    MetaDataList bad {gameRecord()};
    bad.set("playcount", "-3");
    store.capture(key("snes", ProfileRecordType::game, "Hacks/Kaizo Mario World.sfc"), bad);
    EXPECT(store.loadErrors().size() == 1);

    MetaDataList clean {gameRecord()};
    clean.set("playcount", "4");
    store.capture(key("snes", ProfileRecordType::game, "Hacks/Kaizo Mario World.sfc"), clean);
    EXPECT(store.loadErrors().empty());
    fs::remove_all(directory);
}

TEST_CASE(preferenceEditErrorsAreIndependent)
{
    const fs::path directory {makeTempDirectory()};
    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};

    store.setAlternativeEmulator("Bad\xe9");
    EXPECT(store.loadErrors().size() == 1);

    store.setLaunchOnOtherScreen(false);
    EXPECT(store.loadErrors().size() == 1);
    EXPECT(store.loadErrors().front().find("alternativeEmulator") != std::string::npos);
    EXPECT(store.hasPendingWrites());
    fs::remove_all(directory);
}

TEST_CASE(wrongTypedPreferencesSurviveDefaultWrites)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("bad-preferences.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    EXPECT(store.isResolved());
    EXPECT(store.loadErrors().size() == 2);

    store.setAlternativeEmulator("");
    store.setLaunchOnOtherScreen(true);
    EXPECT(!store.hasPendingWrites());
    EXPECT(store.flush().status == ProfileState::FlushStatus::nothingToDo);
    EXPECT(readFile(stateFile) == testData("bad-preferences.json"));

    store.setLaunchOnOtherScreen(false);
    EXPECT(store.hasPendingWrites());
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);

    const std::string actual {readFile(stateFile)};
    if (actual != testData("bad-preferences-fixed.json"))
        std::printf("--- actual ---\n%s\n", actual.c_str());
    EXPECT(actual == testData("bad-preferences-fixed.json"));
    fs::remove_all(directory);
}

TEST_CASE(captureKeepsValuesItCannotEncode)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("snes.json"));

    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};
    EXPECT(store.isResolved());

    MetaDataList record {gameRecord()};
    record.set("favorite", "true");
    record.set("playcount", "-3");
    record.set("lastplayed", "yesterday");
    record.set("controller", "Bad\xe9");
    store.capture(key("snes", ProfileRecordType::game, "Hacks/Kaizo Mario World.sfc"), record);

    // One error per record, naming every field that could not be encoded.
    EXPECT(store.loadErrors().size() == 1);
    EXPECT(store.loadErrors().front().find("playcount") != std::string::npos);
    EXPECT(store.loadErrors().front().find("lastplayed") != std::string::npos);
    EXPECT(store.loadErrors().front().find("controller") != std::string::npos);
    EXPECT(store.hasPendingWrites());
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);

    const std::string actual {readFile(stateFile)};
    if (actual != testData("bad-capture-expected.json"))
        std::printf("--- actual ---\n%s\n", actual.c_str());
    EXPECT(actual == testData("bad-capture-expected.json"));
    fs::remove_all(directory);
}

TEST_CASE(setAlternativeEmulatorRejectsNonUtf8)
{
    const fs::path directory {makeTempDirectory()};
    ProfileState::JsonStore store {session(directory, "snes"),
                                   ProfileState::systemFileOperations()};

    store.setAlternativeEmulator("Bad\xe9");
    EXPECT(!store.hasPendingWrites());
    EXPECT(!store.loadErrors().empty());
    EXPECT(store.flush().status == ProfileState::FlushStatus::nothingToDo);
    fs::remove_all(directory);
}

TEST_CASE(unreadableBackupWithMissingMainIsUnresolved)
{
    const fs::path directory {makeTempDirectory()};
    FaultyFiles files;
    files.failReadSuffix = ".bak";

    ProfileState::JsonStore store {session(directory, "snes"), files};
    EXPECT(!store.isResolved());
    EXPECT(!store.canSwitchProfile());
    EXPECT(!store.loadErrors().empty());
    fs::remove_all(directory);
}

TEST_CASE(recoveryMovesTheUnusableMainAside)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    const std::string tooNew {testData("too-new.json")};
    writeFile(stateFile, tooNew);
    writeFile(stateFile.string() + ".bak", testData("snes.json"));

    FixedClock clock;
    ProfileState::JsonStore store {session(directory, "snes"), ProfileState::systemFileOperations(),
                                   clock};
    EXPECT(!store.isResolved());
    EXPECT(store.recoverFromBackup());
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);

    const fs::path aside {stateFile.string() + ".unresolved-20261118T120000"};
    EXPECT(fs::exists(aside));
    EXPECT(readFile(aside) == tooNew);
    EXPECT(readFile(stateFile) == testData("snes.json"));
    EXPECT(readFile(stateFile.string() + ".bak") == testData("snes.json"));
    fs::remove_all(directory);
}

TEST_CASE(recoveryRefusesToOverwriteAnExistingAsideFile)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    const fs::path aside {stateFile.string() + ".unresolved-20261118T120000"};
    writeFile(stateFile, testData("too-new.json"));
    writeFile(stateFile.string() + ".bak", testData("snes.json"));
    writeFile(aside, "previous aside contents\n");

    FixedClock clock;
    ProfileState::JsonStore store {session(directory, "snes"), ProfileState::systemFileOperations(),
                                   clock};
    EXPECT(store.recoverFromBackup());
    const ProfileState::FlushResult result {store.flush()};
    EXPECT(result.status == ProfileState::FlushStatus::failure);
    EXPECT(result.reason.find("refusing to overwrite") != std::string::npos);
    EXPECT(store.hasPendingWrites());
    EXPECT(readFile(aside) == "previous aside contents\n");
    EXPECT(readFile(stateFile) == testData("too-new.json"));
    fs::remove_all(directory);
}

TEST_CASE(asideRenameFailureKeepsDirtyState)
{
    const fs::path directory {makeTempDirectory()};
    const fs::path stateFile {directory / "profile-state" / "snes.json"};
    writeFile(stateFile, testData("too-new.json"));
    writeFile(stateFile.string() + ".bak", testData("snes.json"));

    FaultyFiles files;
    files.failRenameContaining = ".unresolved-";
    FixedClock clock;
    ProfileState::JsonStore store {session(directory, "snes"), files, clock};
    EXPECT(store.recoverFromBackup());
    const ProfileState::FlushResult result {store.flush()};
    EXPECT(result.status == ProfileState::FlushStatus::failure);
    EXPECT(result.reason.find("aside") != std::string::npos);
    EXPECT(store.hasPendingWrites());
    EXPECT(readFile(stateFile) == testData("too-new.json"));
    EXPECT(!fs::exists(stateFile.string() + ".unresolved-20261118T120000"));
    fs::remove_all(directory);
}
