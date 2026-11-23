//  SPDX-License-Identifier: MIT
//
//  Tests for the 6B offline split: the fork's shared-only XML writer and its
//  ProfileState serializer, key derivation, wrong-typed values, duplicate keys
//  and reconstruction.
//

#include "TestHarness.h"

#include "MigrationSplit.h"
#include "Sha256.h"
#include "StagingIo.h"
#include "profiles/ProfileRecordKey.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    std::string testData(const std::string& name)
    {
        std::ifstream file {std::string {MIGRATION_TEST_DATA_DIR} + "/" + name,
                            std::ios::binary};
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // The relative-path helpers treat an existing directory as the system root
    // and a missing one as its parent, so the fixture root must exist.
    const std::string& romRoot()
    {
        static const std::string root {[] {
            const std::filesystem::path path {
                std::filesystem::temp_directory_path() / "es-migration-split" / "roms" /
                "snes"};
            std::filesystem::create_directories(path);
            return path.string();
        }()};
        return root;
    }

    MigrationSplit::SystemRequest requestFor(const std::string& source = "appdata")
    {
        MigrationSplit::SystemRequest request;
        request.system = "snes";
        request.profileId = "bbb35f7a-a579-4721-8510-2f777c27225a";
        request.romRoot = romRoot();
        request.source = source;
        request.gamelist = "inputs/snes/gamelist.xml";
        request.shared = "library/gamelists/snes/gamelist.xml";
        request.state = "profiles/bbb35f7a-a579-4721-8510-2f777c27225a/home/ES-DE/"
                        "profile-state/snes.json";
        return request;
    }

    void reportMismatch(const std::string& what,
                        const std::string& actual,
                        const std::string& expected)
    {
        if (actual != expected) {
            std::printf("--- %s actual ---\n%s\n--- %s expected ---\n%s\n", what.c_str(),
                        actual.c_str(), what.c_str(), expected.c_str());
        }
    }

    bool has(const std::string& text, const std::string& needle)
    {
        return text.find(needle) != std::string::npos;
    }
} // namespace

TEST_CASE(splitMatchesTheForkGoldenBytes)
{
    const std::string bytes {testData("gamelist-personal.xml")};
    MigrationSplit::SystemRequest request {requestFor()};
    std::string shared;
    std::string state;
    std::string error;
    MigrationSplit::SystemResult result;
    EXPECT(MigrationSplit::splitSystem(request, bytes, shared, state, result, error));

    const std::string expectedShared {testData("gamelist-personal-shared.xml")};
    const std::string expectedState {testData("gamelist-personal-state.json")};
    reportMismatch("shared", shared, expectedShared);
    reportMismatch("state", state, expectedState);
    EXPECT(shared == expectedShared);
    EXPECT(state == expectedState);

    EXPECT(result.records == 4);
    EXPECT(result.personalEntries == 4);
    EXPECT(result.source == "appdata");
    EXPECT(result.items.empty());
}

TEST_CASE(sharedDocumentCarriesNoPersonalFieldOrPreference)
{
    const std::string bytes {testData("gamelist-personal.xml")};
    MigrationSplit::SystemRequest request {requestFor()};
    std::string shared;
    std::string state;
    std::string error;
    MigrationSplit::SystemResult result;
    EXPECT(MigrationSplit::splitSystem(request, bytes, shared, state, result, error));

    for (const std::string key : {"<favorite>", "<completed>", "<hidden>", "<hidemetadata>",
                                  "<playcount>", "<playtime>", "<lastplayed>", "<altemulator>",
                                  "<controller>", "<screen>", "<alternativeEmulator>",
                                  "<launchOnOtherScreen>"}) {
        if (has(shared, key))
            std::printf("personal or preference element in the shared output: %s\n", key.c_str());
        EXPECT(!has(shared, key));
    }
    EXPECT(has(shared, "<mysteryTag>keep me</mysteryTag>"));
    EXPECT(has(shared, "<mysteryFolderTag>keep</mysteryFolderTag>"));
    EXPECT(has(shared, "<scraperNotes>"));
    EXPECT(has(shared, "<note>unknown child element</note>"));
}

TEST_CASE(preferencesMoveToTheStateFile)
{
    const std::string bytes {testData("gamelist-personal.xml")};
    MigrationSplit::SystemRequest request {requestFor()};
    std::string shared;
    std::string state;
    std::string error;
    MigrationSplit::SystemResult result;
    EXPECT(MigrationSplit::splitSystem(request, bytes, shared, state, result, error));
    EXPECT(has(state, "\"alternativeEmulator\": \"Snes9x - Current\""));
    EXPECT(has(state, "\"launchOnOtherScreen\": false"));
}

TEST_CASE(wrongTypedValuesAreKeptVerbatim)
{
    const std::string bytes {testData("gamelist-wrongtypes.xml")};
    MigrationSplit::SystemRequest request {requestFor()};
    std::string shared;
    std::string state;
    std::string error;
    MigrationSplit::SystemResult result;
    EXPECT(MigrationSplit::splitSystem(request, bytes, shared, state, result, error));
    EXPECT(has(state, "\"favorite\": \"yes\""));
    EXPECT(has(state, "\"playcount\": \"many\""));
    EXPECT(has(state, "\"lastplayed\": \"2024-01-02\""));
    EXPECT(result.items.size() == 3);
    for (const MigrationSplit::Item& item : result.items)
        EXPECT(!item.blocking);
}

TEST_CASE(duplicateKeysAreRefused)
{
    const std::string bytes {testData("gamelist-duplicate.xml")};
    MigrationSplit::SystemRequest request {requestFor()};
    std::string shared;
    std::string state;
    std::string error;
    MigrationSplit::SystemResult result;
    EXPECT(!MigrationSplit::splitSystem(request, bytes, shared, state, result, error));
    EXPECT(has(error, "duplicate record key"));
}

TEST_CASE(invalidUtf8PathCannotBeKeyed)
{
    std::string path {"bad"};
    path.push_back(static_cast<char>(0xff));
    path += ".sfc";
    EXPECT(!ProfileRecordKey::fromPath("snes", ProfileRecordType::game, path).has_value());
}

TEST_CASE(reconstructionDetectsTampering)
{
    const std::string bytes {testData("gamelist-personal.xml")};
    MigrationSplit::SystemRequest request {requestFor()};
    std::string shared;
    std::string state;
    std::string error;
    MigrationSplit::SystemResult result;
    EXPECT(MigrationSplit::splitSystem(request, bytes, shared, state, result, error));

    std::string tampered {shared};
    const std::string from {"<name>First Game</name>"};
    const std::string to {"<name>Renamed Game</name>"};
    const std::size_t position {tampered.find(from)};
    EXPECT(position != std::string::npos);
    if (position != std::string::npos)
        tampered.replace(position, from.size(), to);

    MigrationSplit::SystemResult checked;
    EXPECT(!MigrationSplit::verifySystem(request, bytes, tampered, state, checked, error));
    EXPECT(has(error, "shared field \"name\" differs"));
}

TEST_CASE(reconstructionDetectsUnknownChildTampering)
{
    const std::string bytes {testData("gamelist-personal.xml")};
    MigrationSplit::SystemRequest request {requestFor()};
    std::string shared;
    std::string state;
    std::string error;
    MigrationSplit::SystemResult result;
    EXPECT(MigrationSplit::splitSystem(request, bytes, shared, state, result, error));

    std::string tampered {shared};
    const std::string from {"<mysteryTag>keep me</mysteryTag>"};
    const std::string to {"<mysteryTag>changed</mysteryTag>"};
    const std::size_t position {tampered.find(from)};
    EXPECT(position != std::string::npos);
    if (position != std::string::npos)
        tampered.replace(position, from.size(), to);

    MigrationSplit::SystemResult checked;
    EXPECT(!MigrationSplit::verifySystem(request, bytes, tampered, state, checked, error));
    EXPECT(has(error, "unknown children differ"));
}


TEST_CASE(sha256MatchesStandardVectors)
{
    const std::pair<std::string, std::string> vectors[] {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {std::string(55, 'a'), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {std::string(56, 'a'), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {std::string(63, 'a'), "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {std::string(64, 'a'), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {std::string(65, 'a'), "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
        {std::string(119, 'a'), "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
        {std::string(1000000, 'a'),
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
    };
    for (const auto& vector : vectors) {
        MigrationSplit::Sha256 hash;
        hash.update(vector.first);
        const std::string actual {hash.hex()};
        if (actual != vector.second)
            std::printf("sha256(%zu bytes) = %s, expected %s\n", vector.first.size(),
                        actual.c_str(), vector.second.c_str());
        EXPECT(actual == vector.second);
    }
}

TEST_CASE(sha256HandlesUnevenChunks)
{
    MigrationSplit::Sha256 hash;
    hash.update("a");
    hash.update("b");
    hash.update("c");
    EXPECT(hash.hex() ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const std::string message(1000000, 'a');
    MigrationSplit::Sha256 chunked;
    std::size_t offset {0};
    const std::size_t sizes[] {1, 3, 7, 61, 63, 64, 65, 1000, 4096};
    std::size_t index {0};
    while (offset < message.size()) {
        const std::size_t size {std::min(sizes[index % 9], message.size() - offset)};
        chunked.update(reinterpret_cast<const unsigned char*>(message.data()) + offset, size);
        offset += size;
        ++index;
    }
    EXPECT(chunked.hex() ==
           "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE(stagingExistingIdenticalFileIsIdempotent)
{
    const std::filesystem::path directory {
        std::filesystem::temp_directory_path() / "es-migration-staging-idempotent"};
    std::filesystem::create_directories(directory);
    const int root {::open(directory.c_str(), O_RDONLY | O_DIRECTORY)};
    EXPECT(root >= 0);
    std::string error;
    EXPECT(MigrationSplit::writeNoClobber(root, "a/b.txt", "same", error));
    EXPECT(MigrationSplit::writeNoClobber(root, "a/b.txt", "same", error));
    EXPECT(error.empty());
    ::close(root);
}

TEST_CASE(stagingExistingDifferentFileIsRefusedUntouched)
{
    const std::filesystem::path directory {
        std::filesystem::temp_directory_path() / "es-migration-staging-different"};
    std::filesystem::create_directories(directory);
    const int root {::open(directory.c_str(), O_RDONLY | O_DIRECTORY)};
    EXPECT(root >= 0);
    std::string error;
    EXPECT(MigrationSplit::writeNoClobber(root, "a/b.txt", "short", error));
    EXPECT(!MigrationSplit::writeNoClobber(root, "a/b.txt", "longer", error));
    EXPECT(error.find("different bytes") != std::string::npos);
    std::string stored;
    std::string readError;
    EXPECT(MigrationSplit::readStagingFile(root, "a/b.txt", 1024, stored, readError));
    EXPECT(stored == "short");
    ::close(root);
}

TEST_CASE(stagingNamesRejectTraversal)
{
    EXPECT(MigrationSplit::validRelative("a/b.txt"));
    EXPECT(!MigrationSplit::validRelative("../escape"));
    EXPECT(!MigrationSplit::validRelative("a/../b"));
    EXPECT(!MigrationSplit::validRelative("/absolute"));
    EXPECT(!MigrationSplit::validRelative("a//b"));
    EXPECT(!MigrationSplit::validRelative(""));
}

TEST_CASE(nestedPortKeysStayRelative)
{
    const std::string bytes {testData("gamelist-ports.xml")};
    MigrationSplit::SystemRequest request {requestFor()};
    std::string shared;
    std::string state;
    std::string error;
    MigrationSplit::SystemResult result;
    EXPECT(MigrationSplit::splitSystem(request, bytes, shared, state, result, error));

    const std::string expectedShared {testData("gamelist-ports-shared.xml")};
    const std::string expectedState {testData("gamelist-ports-state.json")};
    reportMismatch("ports shared", shared, expectedShared);
    reportMismatch("ports state", state, expectedState);
    EXPECT(shared == expectedShared);
    EXPECT(state == expectedState);

    EXPECT(has(state, "\"path\": \"ports/quake2/baseq2/pak0.pak\""));
    EXPECT(has(state, "\"path\": \"ports/tyrquake/id1/pak0.pak\""));
    EXPECT(has(state, "\"path\": \"ports/quake2\""));
    EXPECT(!has(state, "\"path\": \"./"));
    EXPECT(!has(state, "\"path\": \"/"));
    EXPECT(has(state, "\"type\": \"folder\",\n      \"path\": \"ports/quake2\""));
    EXPECT(has(state, "\"type\": \"game\",\n      \"path\": \"ports/quake2/baseq2/pak0.pak\""));
}

TEST_CASE(legacySourceDoesNotChangeBytes)
{
    const std::string bytes {testData("gamelist-personal.xml")};
    std::string appdataShared;
    std::string appdataState;
    std::string legacyShared;
    std::string legacyState;
    std::string error;
    MigrationSplit::SystemResult appdataResult;
    MigrationSplit::SystemResult legacyResult;
    EXPECT(MigrationSplit::splitSystem(requestFor("appdata"), bytes, appdataShared, appdataState,
                                       appdataResult, error));
    EXPECT(MigrationSplit::splitSystem(requestFor("legacy"), bytes, legacyShared, legacyState,
                                       legacyResult, error));
    EXPECT(appdataShared == legacyShared);
    EXPECT(appdataState == legacyState);
    EXPECT(appdataResult.source == "appdata");
    EXPECT(legacyResult.source == "legacy");
}

TEST_CASE(pathOutsideTheSystemRootIsRefused)
{
    const std::string bytes {testData("gamelist-outside.xml")};
    MigrationSplit::SystemRequest request {requestFor()};
    std::string shared;
    std::string state;
    std::string error;
    MigrationSplit::SystemResult result;
    EXPECT(!MigrationSplit::splitSystem(request, bytes, shared, state, result, error));
    EXPECT(has(error, "outside the system root"));
}

TEST_CASE(legacySourceIsRecorded)
{
    const std::string bytes {testData("gamelist-personal.xml")};
    MigrationSplit::SystemRequest request {requestFor("legacy")};
    std::string shared;
    std::string state;
    std::string error;
    MigrationSplit::SystemResult result;
    EXPECT(MigrationSplit::splitSystem(request, bytes, shared, state, result, error));
    EXPECT(result.source == "legacy");
}
