//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileStateStoreTests.cpp
//
//  Test cases for the format-independent profile store contract. The in-memory store in this
//  file stands in for the JSON implementation of .8 and exercises the same interface: two
//  profiles stay isolated, omitted fields take their defaults instead of the shared XML tags,
//  and a failed flush keeps the staged changes pending.
//

#include "TestHarness.h"

#include "MetaData.h"
#include "profiles/ProfileRecordKey.h"
#include "profiles/ProfileStateStore.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr const char* sProfileAlice {"bbb35f7a-a579-4721-8510-2f777c27225a"};
    constexpr const char* sProfileBob {"3f6c2a6e-0e8a-4c5b-9d0a-6f8f0e3b1c2d"};

    ProfileState::Session session(const std::string& profileId, const std::string& system)
    {
        ProfileState::Session result;
        result.profileId = profileId;
        result.profileRoot = "/var/lib/nitropie/profiles/" + profileId;
        result.stateDirectory = result.profileRoot + "/home/ES-DE/profile-state";
        result.system = system;

        return result;
    }

    ProfileRecordKey key(const std::string& system,
                         const ProfileRecordType type,
                         const std::string& path)
    {
        const std::optional<ProfileRecordKey> result {
            ProfileRecordKey::fromPath(system, type, path)};
        EXPECT(result.has_value());

        return result.value_or(ProfileRecordKey::fromPath("invalid", type, "invalid").value());
    }

    // The .8 store is not part of this ticket. This double implements the same contract so the
    // isolation, default and dirty-state rules are testable now.
    class InMemoryStore final : public ProfileState::Store
    {
    public:
        explicit InMemoryStore(ProfileState::Session session)
            : mSession {std::move(session)}
        {
        }

        const ProfileState::Session& session() const override { return mSession; }

        void apply(const ProfileRecordKey& key, MetaDataList& record) const override
        {
            const auto stored {mRecords.find(key)};

            for (const MetaDataDecl& decl : record.getMDD()) {
                if (!isPersonalMetadata(decl))
                    continue;

                if (stored != mRecords.end()) {
                    const auto value {stored->second.find(decl.key)};
                    if (value != stored->second.end()) {
                        record.set(decl.key, value->second);
                        continue;
                    }
                }

                // Omitted means the declared default. The shared gamelist is never consulted.
                record.set(decl.key, decl.defaultValue);
            }
        }

        void capture(const ProfileRecordKey& key, const MetaDataList& record) override
        {
            std::map<std::string, std::string> fields;
            for (const MetaDataDecl& decl : record.getMDD()) {
                if (!isPersonalMetadata(decl))
                    continue;

                const std::string& value {record.get(decl.key)};
                if (value != decl.defaultValue)
                    fields[decl.key] = value;
            }

            const auto stored {mRecords.find(key)};
            const bool changed {stored == mRecords.end() ? !fields.empty() :
                                                           stored->second != fields};

            if (!changed)
                return;

            if (fields.empty())
                mRecords.erase(key);
            else
                mRecords[key] = std::move(fields);

            mDirty = true;
        }

        ProfileState::FlushResult flush() override
        {
            if (!mDirty)
                return {ProfileState::FlushStatus::nothingToDo, ""};

            if (mFailFlush)
                return {ProfileState::FlushStatus::failure, "injected failure"};

            mDirty = false;
            return {ProfileState::FlushStatus::success, ""};
        }

        bool hasPendingWrites() const override { return mDirty; }
        bool canSwitchProfile() const override { return !mDirty; }

        bool isResolved() const override { return true; }
        const std::vector<std::string>& loadErrors() const override { return mErrors; }
        bool recoverFromBackup() override { return false; }

        void failFlush(const bool fail) { mFailFlush = fail; }

        std::size_t recordCount() const { return mRecords.size(); }
        bool hasRecord(const ProfileRecordKey& key) const { return mRecords.count(key) == 1; }

    private:
        ProfileState::Session mSession;
        std::map<ProfileRecordKey, std::map<std::string, std::string>> mRecords;
        bool mDirty {false};
        bool mFailFlush {false};
        std::vector<std::string> mErrors;
    };

    MetaDataList sharedGameRecord()
    {
        MetaDataList record {GAME_METADATA};
        record.set("name", "Super Mario World");
        record.set("desc", "A launch game");
        // Stale personal tags from a shared gamelist, as migration and legacy libraries have them.
        record.set("favorite", "true");
        record.set("playcount", "7");
        record.set("lastplayed", "20260101T000000");

        return record;
    }

} // namespace

TEST_CASE(twoProfilesKeepSeparatePersonalState)
{
    const ProfileRecordKey game {
        key("snes", ProfileRecordType::game, "Super Mario World (USA).sfc")};

    InMemoryStore alice {session(sProfileAlice, "snes")};
    InMemoryStore bob {session(sProfileBob, "snes")};

    // The session identity is fixed at construction and carries the exact absolute paths.
    EXPECT(alice.session().profileId == "bbb35f7a-a579-4721-8510-2f777c27225a");
    EXPECT(alice.session().profileRoot ==
           "/var/lib/nitropie/profiles/bbb35f7a-a579-4721-8510-2f777c27225a");
    EXPECT(alice.session().stateDirectory ==
           "/var/lib/nitropie/profiles/bbb35f7a-a579-4721-8510-2f777c27225a"
           "/home/ES-DE/profile-state");
    EXPECT(alice.session().system == "snes");
    EXPECT(bob.session().profileId == "3f6c2a6e-0e8a-4c5b-9d0a-6f8f0e3b1c2d");

    MetaDataList aliceRecord {GAME_METADATA};
    aliceRecord.set("favorite", "true");
    aliceRecord.set("completed", "true");
    aliceRecord.set("playcount", "3");
    aliceRecord.set("playtime", "120");
    aliceRecord.set("lastplayed", "20261116T203000");
    alice.capture(game, aliceRecord);

    EXPECT(alice.hasPendingWrites());
    EXPECT(!alice.canSwitchProfile());

    // Alice sees her exact values; the shared fields are untouched.
    MetaDataList aliceView {sharedGameRecord()};
    alice.apply(game, aliceView);
    EXPECT(aliceView.get("favorite") == "true");
    EXPECT(aliceView.get("completed") == "true");
    EXPECT(aliceView.get("playcount") == "3");
    EXPECT(aliceView.get("playtime") == "120");
    EXPECT(aliceView.get("lastplayed") == "20261116T203000");
    EXPECT(aliceView.get("name") == "Super Mario World");
    EXPECT(aliceView.get("desc") == "A launch game");

    // Bob has no record; the stale tags in the shared XML never become his defaults.
    MetaDataList bobView {sharedGameRecord()};
    bob.apply(game, bobView);
    EXPECT(bobView.get("favorite") == "false");
    EXPECT(bobView.get("completed") == "false");
    EXPECT(bobView.get("playcount") == "0");
    EXPECT(bobView.get("playtime") == "0");
    EXPECT(bobView.get("lastplayed") == "0");
    EXPECT(bobView.get("name") == "Super Mario World");

    EXPECT(alice.flush().status == ProfileState::FlushStatus::success);
    EXPECT(bob.flush().status == ProfileState::FlushStatus::nothingToDo);
    EXPECT(!alice.hasPendingWrites());
    EXPECT(alice.canSwitchProfile());

    // No cross-profile writes: one record for Alice, none for Bob.
    EXPECT(alice.recordCount() == 1);
    EXPECT(bob.recordCount() == 0);

    MetaDataList aliceAfterFlush {sharedGameRecord()};
    alice.apply(game, aliceAfterFlush);
    EXPECT(aliceAfterFlush.get("favorite") == "true");
    EXPECT(aliceAfterFlush.get("playcount") == "3");
}

TEST_CASE(sameBasenameInDifferentFoldersAndSystemsStaysSeparate)
{
    const ProfileRecordKey rpg {key("snes", ProfileRecordType::game, "RPG/Game.sfc")};
    const ProfileRecordKey action {key("snes", ProfileRecordType::game, "Action/Game.sfc")};
    const ProfileRecordKey genesis {key("genesis", ProfileRecordType::game, "Game.sfc")};

    InMemoryStore snes {session(sProfileAlice, "snes")};
    InMemoryStore genesisStore {session(sProfileAlice, "genesis")};

    MetaDataList rpgRecord {GAME_METADATA};
    rpgRecord.set("favorite", "true");
    rpgRecord.set("lastplayed", "20261101T101010");
    snes.capture(rpg, rpgRecord);

    MetaDataList actionRecord {GAME_METADATA};
    actionRecord.set("completed", "true");
    actionRecord.set("playcount", "9");
    snes.capture(action, actionRecord);

    MetaDataList genesisRecord {GAME_METADATA};
    genesisRecord.set("playtime", "77");
    genesisStore.capture(genesis, genesisRecord);

    MetaDataList rpgView {sharedGameRecord()};
    snes.apply(rpg, rpgView);
    EXPECT(rpgView.get("favorite") == "true");
    EXPECT(rpgView.get("lastplayed") == "20261101T101010");
    EXPECT(rpgView.get("completed") == "false");
    EXPECT(rpgView.get("playcount") == "0");

    MetaDataList actionView {sharedGameRecord()};
    snes.apply(action, actionView);
    EXPECT(actionView.get("favorite") == "false");
    EXPECT(actionView.get("completed") == "true");
    EXPECT(actionView.get("playcount") == "9");

    MetaDataList genesisView {sharedGameRecord()};
    genesisStore.apply(genesis, genesisView);
    EXPECT(genesisView.get("playtime") == "77");
    EXPECT(genesisView.get("favorite") == "false");
    EXPECT(genesisView.get("playcount") == "0");

    EXPECT(snes.recordCount() == 2);
    EXPECT(genesisStore.recordCount() == 1);
}

TEST_CASE(gameAndFolderSharingAPathStaySeparate)
{
    const ProfileRecordKey game {key("snes", ProfileRecordType::game, "Hacks")};
    const ProfileRecordKey folder {key("snes", ProfileRecordType::folder, "Hacks")};

    InMemoryStore store {session(sProfileAlice, "snes")};

    MetaDataList gameRecord {GAME_METADATA};
    gameRecord.set("favorite", "true");
    store.capture(game, gameRecord);

    MetaDataList folderRecord {FOLDER_METADATA};
    folderRecord.set("hidden", "true");
    store.capture(folder, folderRecord);

    EXPECT(store.recordCount() == 2);

    MetaDataList gameView {GAME_METADATA};
    store.apply(game, gameView);
    EXPECT(gameView.get("favorite") == "true");
    EXPECT(gameView.get("hidden") == "false");

    MetaDataList folderView {FOLDER_METADATA};
    store.apply(folder, folderView);
    EXPECT(folderView.get("favorite") == "false");
    EXPECT(folderView.get("hidden") == "true");
}

TEST_CASE(failedFlushKeepsChangesPending)
{
    const ProfileRecordKey game {key("snes", ProfileRecordType::game, "Game.sfc")};
    InMemoryStore store {session(sProfileAlice, "snes")};

    MetaDataList record {GAME_METADATA};
    record.set("favorite", "true");
    store.capture(game, record);

    store.failFlush(true);
    EXPECT(store.flush().status == ProfileState::FlushStatus::failure);
    EXPECT(store.hasPendingWrites());
    EXPECT(!store.canSwitchProfile());

    // The staged value is still readable after the failed write.
    MetaDataList view {GAME_METADATA};
    store.apply(game, view);
    EXPECT(view.get("favorite") == "true");

    store.failFlush(false);
    EXPECT(store.flush().status == ProfileState::FlushStatus::success);
    EXPECT(!store.hasPendingWrites());
    EXPECT(store.canSwitchProfile());
}

TEST_CASE(capturingSharedFieldsCreatesNoPersonalRecord)
{
    const ProfileRecordKey game {key("snes", ProfileRecordType::game, "Game.sfc")};
    InMemoryStore store {session(sProfileAlice, "snes")};

    // A shared library edit, for example a scrape result, is never personal state.
    MetaDataList record {GAME_METADATA};
    record.set("name", "Scraped Name");
    record.set("rating", "0.80");
    record.set("developer", "Scraped Developer");
    store.capture(game, record);

    EXPECT(!store.hasRecord(game));
    EXPECT(!store.hasPendingWrites());
    EXPECT(store.canSwitchProfile());
    EXPECT(store.flush().status == ProfileState::FlushStatus::nothingToDo);
}

TEST_CASE(capturingDefaultsRemovesTheRecord)
{
    const ProfileRecordKey game {key("snes", ProfileRecordType::game, "Game.sfc")};
    InMemoryStore store {session(sProfileAlice, "snes")};

    MetaDataList record {GAME_METADATA};
    record.set("favorite", "true");
    store.capture(game, record);
    EXPECT(store.hasRecord(game));

    MetaDataList reset {GAME_METADATA};
    store.capture(game, reset);
    EXPECT(!store.hasRecord(game));
    EXPECT(store.hasPendingWrites());
}
