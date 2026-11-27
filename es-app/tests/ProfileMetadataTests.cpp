//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileMetadataTests.cpp
//
//  Test cases for the profile metadata file M: id matching, name validation, unknown avatars
//  and keys, atomic writes and unusable files.
//

#include "TestHarness.h"

#include "profiles/AvatarManifest.h"
#include "profiles/ProfileMetadata.h"
#include "profiles/ProfileStateFile.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* sProfileA {"bbb35f7a-a579-4721-8510-2f777c27225a"};

    std::string testData(const std::string& name)
    {
        std::ifstream file {fs::path {ES_APP_TEST_DATA_DIR} / "profile" / name};
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
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

    fs::path tempFile(const std::string& name)
    {
        const fs::path path {fs::temp_directory_path() / name};
        fs::remove(path);
        return path;
    }

    class FaultyFiles final : public ProfileState::FileOperations
    {
    public:
        bool failRename {false};

        ProfileState::ReadStatus readFile(const std::string& path, std::string& contents) override
        {
            return ProfileState::systemFileOperations().readFile(path, contents);
        }

        bool writeFile(const std::string& path, const std::string& contents) override
        {
            return ProfileState::systemFileOperations().writeFile(path, contents);
        }

        bool writeNewFile(const std::string& path, const std::string& contents) override
        {
            return ProfileState::systemFileOperations().writeNewFile(path, contents);
        }

        bool syncFile(const std::string& path) override
        {
            return ProfileState::systemFileOperations().syncFile(path);
        }

        bool renameFile(const std::string& from, const std::string& to) override
        {
            if (failRename)
                return false;
            return ProfileState::systemFileOperations().renameFile(from, to);
        }

        bool syncDirectory(const std::string& directory) override
        {
            return ProfileState::systemFileOperations().syncDirectory(directory);
        }

        bool removeFile(const std::string& path) override
        {
            return ProfileState::systemFileOperations().removeFile(path);
        }
    };

    std::string nameWithLength(const std::string& unit, const std::size_t count)
    {
        std::string name;
        for (std::size_t index {0}; index < count; ++index)
            name += unit;
        return name;
    }
} // namespace

TEST_CASE(unknownAvatarDisplaysDefaultAndIsKept)
{
    const fs::path path {tempFile("es-de-profile-metadata-unknown.json")};
    writeFile(path, testData("profile-unknown.json"));

    const fs::path manifestPath {tempFile("es-de-profile-avatars.json")};
    writeFile(manifestPath, testData("avatars.json"));
    std::string manifestError;
    const auto manifest {ProfileAvatars::Manifest::load(
        manifestPath.string(), ProfileState::systemFileOperations(), manifestError)};
    EXPECT(manifest.has_value());

    ProfileMetadata metadata {path.string(), sProfileA, ProfileState::systemFileOperations()};
    EXPECT(metadata.isResolved());
    EXPECT(metadata.loadErrors().empty());
    EXPECT(metadata.avatarId() == "future-avatar");
    EXPECT(metadata.displayedAvatar(*manifest) == "fox");

    std::string error;
    EXPECT(metadata.setDisplayName("David II", error));
    EXPECT(error.empty());

    const ProfileState::FlushResult result {metadata.flush()};
    EXPECT(result.status == ProfileState::FlushStatus::success);

    const std::string actual {readFile(path)};
    if (actual != testData("profile-unknown-golden.json"))
        std::printf("--- actual ---\n%s\n", actual.c_str());
    EXPECT(actual == testData("profile-unknown-golden.json"));
    EXPECT(actual.find("\"avatar\": \"future-avatar\"") != std::string::npos);
    EXPECT(actual.find("\"aaaExtra\"") < actual.find("\"zzzExtra\""));
    fs::remove(path);
    fs::remove(manifestPath);
}

TEST_CASE(invalidNamesAreRejected)
{
    const fs::path path {tempFile("es-de-profile-metadata-names.json")};
    writeFile(path, testData("profile.json"));

    ProfileMetadata metadata {path.string(), sProfileA, ProfileState::systemFileOperations()};
    EXPECT(metadata.isResolved());

    const std::string longName {nameWithLength("a", 33)};
    const std::string controlName {std::string {"bad"} + '\n' + "name"};

    const std::vector<std::string> rejected {"",       "   ",       "\t \n",
                                             longName, controlName, nameWithLength("\xC3\xA9", 33)};

    for (const std::string& name : rejected) {
        std::string error;
        EXPECT(!metadata.setDisplayName(name, error));
        EXPECT(!error.empty());
        EXPECT(!metadata.hasPendingWrites());
        EXPECT(metadata.displayName() == "David");
    }

    std::string error;
    EXPECT(metadata.setDisplayName(nameWithLength("\xC3\xA9", 32), error));
    EXPECT(error.empty());
    EXPECT(metadata.hasPendingWrites());
    fs::remove(path);
}

TEST_CASE(namesAreTrimmed)
{
    const fs::path path {tempFile("es-de-profile-metadata-trim.json")};
    writeFile(path, testData("profile.json"));

    ProfileMetadata metadata {path.string(), sProfileA, ProfileState::systemFileOperations()};
    std::string error;
    EXPECT(metadata.setDisplayName("  David  ", error));
    EXPECT(error.empty());
    EXPECT(metadata.displayName() == "David");
    EXPECT(!metadata.hasPendingWrites());
    EXPECT(metadata.flush().status == ProfileState::FlushStatus::nothingToDo);
    fs::remove(path);
}

TEST_CASE(mismatchedIdIsUnresolved)
{
    const fs::path path {tempFile("es-de-profile-metadata-id.json")};
    writeFile(path, testData("profile.json"));

    ProfileMetadata metadata {path.string(), "3f6c2a6e-0e8a-4c5b-9d0a-6f8f0e3b1c2d",
                              ProfileState::systemFileOperations()};
    EXPECT(!metadata.isResolved());
    EXPECT(!metadata.loadErrors().empty());
    if (!metadata.loadErrors().empty()) {
        EXPECT(metadata.loadErrors().front().find("does not match the profile directory") !=
               std::string::npos);
    }
    EXPECT(metadata.flush().status == ProfileState::FlushStatus::failure);
    fs::remove(path);
}

TEST_CASE(writeFailureKeepsOldFileAndDirtyState)
{
    const fs::path path {tempFile("es-de-profile-metadata-failure.json")};
    writeFile(path, testData("profile.json"));
    const std::string previous {readFile(path)};

    FaultyFiles files;
    ProfileMetadata metadata {path.string(), sProfileA, files};
    std::string error;
    EXPECT(metadata.setDisplayName("David II", error));
    files.failRename = true;

    const ProfileState::FlushResult result {metadata.flush()};
    EXPECT(result.status == ProfileState::FlushStatus::failure);
    EXPECT(result.reason.find("rename") != std::string::npos);
    EXPECT(metadata.hasPendingWrites());
    EXPECT(readFile(path) == previous);

    files.failRename = false;
    EXPECT(metadata.flush().status == ProfileState::FlushStatus::success);
    EXPECT(readFile(path).find("\"displayName\": \"David II\"") != std::string::npos);
    fs::remove(path);
}

TEST_CASE(missingCorruptAndTooNewMetadataAreUnresolved)
{
    const std::string tooNew {
        "{\n  \"schema\": \"nitropie.profile\",\n  \"version\": 9,\n  \"id\": \"" +
        std::string {sProfileA} + "\",\n  \"displayName\": \"David\",\n  \"avatar\": \"fox\"\n}\n"};

    const std::vector<std::string> cases {
        std::string {""},
        "not json\n",
        "{\n  \"schema\": \"nitropie.profile\",\n  \"version\": 1,\n  \"id\": \"" +
            std::string {sProfileA} + "\",\n  \"displayName\": \"David\",\n",
        tooNew,
    };

    for (const std::string& contents : cases) {
        const fs::path path {tempFile("es-de-profile-metadata-unusable.json")};
        if (!contents.empty())
            writeFile(path, contents);

        ProfileMetadata metadata {path.string(), sProfileA, ProfileState::systemFileOperations()};
        EXPECT(!metadata.isResolved());
        EXPECT(!metadata.loadErrors().empty());
        EXPECT(metadata.flush().status == ProfileState::FlushStatus::failure);
        if (!contents.empty())
            EXPECT(readFile(path) == contents);
        fs::remove(path);
    }
}

TEST_CASE(backupRecoveryRestoresTheLastGoodCopy)
{
    const fs::path path {tempFile("es-de-profile-metadata-backup.json")};
    writeFile(path, testData("profile.json"));

    {
        ProfileMetadata metadata {path.string(), sProfileA, ProfileState::systemFileOperations()};
        std::string error;
        EXPECT(metadata.setDisplayName("David II", error));
        EXPECT(metadata.flush().status == ProfileState::FlushStatus::success);
        EXPECT(metadata.setDisplayName("David III", error));
        EXPECT(metadata.flush().status == ProfileState::FlushStatus::success);
    }

    const std::string good {readFile(path.string() + ".bak")};
    writeFile(path, "corrupt\n");

    ProfileMetadata broken {path.string(), sProfileA, ProfileState::systemFileOperations()};
    EXPECT(!broken.isResolved());
    EXPECT(broken.recoverFromBackup());
    EXPECT(broken.isResolved());
    EXPECT(broken.hasPendingWrites());
    EXPECT(broken.flush().status == ProfileState::FlushStatus::success);
    EXPECT(readFile(path) == good);
    fs::remove(path);
    fs::remove(path.string() + ".bak");
}
