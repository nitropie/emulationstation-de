//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileRecordKeyTests.cpp
//
//  Test cases for the personal-record key. Paths are compared byte for byte and anything that
//  is not a normalized path below the system root is rejected.
//

#include "TestHarness.h"

#include "profiles/ProfileRecordKey.h"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace
{
    ProfileRecordKey key(const std::string& system,
                         const ProfileRecordType type,
                         const std::string& path)
    {
        const std::optional<ProfileRecordKey> result {
            ProfileRecordKey::fromPath(system, type, path)};

        // Every caller in these tests uses a valid key; a null key would make the assertion below
        // fail with a readable location.
        EXPECT(result.has_value());
        return result.value_or(ProfileRecordKey::fromPath("invalid", type, "invalid").value());
    }

} // namespace

TEST_CASE(keysCarryTheirSystemTypeAndExactRelativePath)
{
    const ProfileRecordKey game {
        key("snes", ProfileRecordType::game, "Hacks/Kaizo Mario World.sfc")};

    EXPECT(game.system() == "snes");
    EXPECT(game.type() == ProfileRecordType::game);
    EXPECT(game.path() == "Hacks/Kaizo Mario World.sfc");
    EXPECT(game.describe() == "snes/game:Hacks/Kaizo Mario World.sfc");

    const ProfileRecordKey folder {key("snes", ProfileRecordType::folder, "Hacks")};
    EXPECT(folder.type() == ProfileRecordType::folder);
    EXPECT(folder.path() == "Hacks");
}

TEST_CASE(sameBasenameInDifferentFoldersAndSystemsStaysDistinct)
{
    const ProfileRecordKey rpg {key("snes", ProfileRecordType::game, "RPG/Game.sfc")};
    const ProfileRecordKey action {key("snes", ProfileRecordType::game, "Action/Game.sfc")};
    const ProfileRecordKey genesis {key("genesis", ProfileRecordType::game, "Game.sfc")};

    EXPECT(rpg != action);
    EXPECT(rpg != genesis);
    EXPECT(action != genesis);

    EXPECT(rpg.path() == "RPG/Game.sfc");
    EXPECT(action.path() == "Action/Game.sfc");
    EXPECT(genesis.path() == "Game.sfc");
}

TEST_CASE(gameAndFolderSharingAPathStayDistinct)
{
    const ProfileRecordKey game {key("snes", ProfileRecordType::game, "Hacks")};
    const ProfileRecordKey folder {key("snes", ProfileRecordType::folder, "Hacks")};

    EXPECT(game != folder);
    EXPECT(game.path() == folder.path());
    // The type is part of the identity, not something that can be inferred from the path.
    EXPECT(game < folder);
    EXPECT(!(folder < game));
}

TEST_CASE(collectionEntriesResolveToTheirSourceGame)
{
    const ProfileRecordKey source {
        key("snes", ProfileRecordType::game, "Hacks/Kaizo Mario World.sfc")};

    const ProfileRecordKey collection {ProfileRecordKey::fromCollectionEntry(source)};

    EXPECT(collection == source);
    EXPECT(collection.system() == "snes");
    EXPECT(collection.type() == ProfileRecordType::game);
    EXPECT(collection.path() == "Hacks/Kaizo Mario World.sfc");
}

TEST_CASE(rootKeysRequireADirectoryBoundary)
{
    const std::string root {"/roms/snes"};
    const auto fromRoot = [&root](const std::string& path) {
        return ProfileRecordKey::fromRootPath("snes", ProfileRecordType::game, root, path);
    };

    // The root itself has no record, with or without a trailing separator.
    EXPECT(!fromRoot("/roms/snes").has_value());
    EXPECT(!fromRoot("/roms/snes/").has_value());

    // A sibling that only shares the root's string prefix is outside, not "other/Game.sfc".
    EXPECT(!fromRoot("/roms/snes-other/Game.sfc").has_value());
    EXPECT(!fromRoot("/roms/snesother/Game.sfc").has_value());
    EXPECT(!fromRoot("/roms/other/Game.sfc").has_value());

    // Traversal and dot segments are rejected even when the text starts below the root.
    EXPECT(!fromRoot("/roms/snes/../snes-other/Game.sfc").has_value());
    EXPECT(!fromRoot("/roms/snes/Hacks/../Game.sfc").has_value());
    EXPECT(!fromRoot("/roms/snes/./Game.sfc").has_value());
    EXPECT(!fromRoot("/roms/snes/Hacks/./Game.sfc").has_value());

    const std::optional<ProfileRecordKey> top {fromRoot("/roms/snes/Game.sfc")};
    EXPECT(top.has_value());
    if (top.has_value())
        EXPECT(top->path() == "Game.sfc");

    const std::optional<ProfileRecordKey> nested {fromRoot("/roms/snes/Hacks/Game.sfc")};
    EXPECT(nested.has_value());
    if (nested.has_value())
        EXPECT(nested->path() == "Hacks/Game.sfc");

    // Separator normalization happens before the boundary test and the key rules.
    const std::optional<ProfileRecordKey> normalized {
        fromRoot("/roms/snes//Hacks//Game.sfc")};
    EXPECT(normalized.has_value());
    if (normalized.has_value())
        EXPECT(normalized->path() == "Hacks/Game.sfc");

    // A root of "/" keeps its leading separator out of the relative key.
    const std::optional<ProfileRecordKey> filesystemRoot {
        ProfileRecordKey::fromRootPath("snes", ProfileRecordType::game, "/", "/Game.sfc")};
    EXPECT(filesystemRoot.has_value());
    if (filesystemRoot.has_value())
        EXPECT(filesystemRoot->path() == "Game.sfc");
    EXPECT(!ProfileRecordKey::fromRootPath("snes", ProfileRecordType::game, "/", "/").has_value());
}

TEST_CASE(pathsThatEscapeTheSystemRootAreRejected)
{
    const std::vector<std::string> rejected {
        "",
        ".",
        "..",
        "../Game.sfc",
        "Hacks/../../Game.sfc",
        "/Game.sfc",
        "/mnt/cart0/roms/snes/Game.sfc",
        "Game.sfc/",
        "./Game.sfc",
        "Hacks/./Kaizo Mario World.sfc",
        "Hacks//Kaizo Mario World.sfc",
    };

    for (const std::string& path : rejected) {
        const std::optional<ProfileRecordKey> result {
            ProfileRecordKey::fromPath("snes", ProfileRecordType::game, path)};

        if (result.has_value())
            std::printf("accepted escaping path: \"%s\"\n", path.c_str());

        EXPECT(!result.has_value());
    }

    // The system is part of the identity and cannot be empty.
    EXPECT(!ProfileRecordKey::fromPath("", ProfileRecordType::game, "Game.sfc").has_value());
}

TEST_CASE(validPathsAreStoredWithoutNormalization)
{
    const ProfileRecordKey spaced {
        key("snes", ProfileRecordType::game, "Hacks/Kaizo Mario World (USA).sfc")};
    EXPECT(spaced.path() == "Hacks/Kaizo Mario World (USA).sfc");

    // Dot directories and backslashes are ordinary bytes, not segments or separators to rewrite.
    const ProfileRecordKey dotted {key("snes", ProfileRecordType::game, ".config/Game.sfc")};
    EXPECT(dotted.path() == ".config/Game.sfc");

    const ProfileRecordKey backslash {key("snes", ProfileRecordType::game, "Odd\\Name.sfc")};
    EXPECT(backslash.path() == "Odd\\Name.sfc");

    // Comparison is byte for byte; there is no case folding or Unicode normalization.
    const ProfileRecordKey upper {key("snes", ProfileRecordType::game, "Game.sfc")};
    const ProfileRecordKey lower {key("snes", ProfileRecordType::game, "game.sfc")};
    EXPECT(upper != lower);
}

TEST_CASE(keysSortBySystemTypeThenPath)
{
    const ProfileRecordKey genesis {key("genesis", ProfileRecordType::game, "Game.sfc")};
    const ProfileRecordKey snesFolder {key("snes", ProfileRecordType::folder, "Hacks")};
    const ProfileRecordKey snesGameA {key("snes", ProfileRecordType::game, "A.sfc")};
    const ProfileRecordKey snesGameB {key("snes", ProfileRecordType::game, "B.sfc")};

    EXPECT(genesis < snesGameA);
    EXPECT(snesGameA < snesGameB);
    EXPECT(snesGameA < snesFolder);
    EXPECT(!(snesGameB < snesGameA));
}
