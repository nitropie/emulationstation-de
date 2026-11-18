//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  AvatarManifestTests.cpp
//
//  Test cases for the avatar manifest reader.
//

#include "TestHarness.h"

#include "profiles/AvatarManifest.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    std::string testData(const std::string& name)
    {
        std::ifstream file {fs::path {ES_APP_TEST_DATA_DIR} / "profile" / name};
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    fs::path writeTemp(const std::string& name, const std::string& contents)
    {
        const fs::path path {fs::temp_directory_path() / name};
        std::ofstream file {path};
        file << contents;
        return path;
    }
} // namespace

TEST_CASE(validManifestListsAvatars)
{
    const fs::path path {writeTemp("es-de-avatars-valid.json", testData("avatars.json"))};
    std::string error;

    const std::optional<ProfileAvatars::Manifest> manifest {
        ProfileAvatars::Manifest::load(path.string(), ProfileState::systemFileOperations(), error)};
    EXPECT(manifest.has_value());
    EXPECT(error.empty());
    EXPECT(manifest->defaultId() == "fox");
    EXPECT(manifest->avatars().size() == 3);
    EXPECT(manifest->avatars()[0].id == "cat");
    EXPECT(manifest->avatars()[0].file == "cat.png");
    EXPECT(manifest->avatars()[0].label == "Cat");
    EXPECT(manifest->pathFor(*manifest->find("owl")) == path.parent_path().string() + "/owl.png");
    EXPECT(manifest->displayFor("owl").id == "owl");
    EXPECT(manifest->displayFor("future-avatar").id == "fox");
    EXPECT(manifest->find("future-avatar") == nullptr);
    fs::remove(path);
}

TEST_CASE(duplicateAvatarIdIsRejected)
{
    const fs::path path {writeTemp("es-de-avatars-duplicate.json",
                                   R"({"version":1,"default":"fox","avatars":[)"
                                   R"({"id":"fox","file":"a.png","label":"A"},)"
                                   R"({"id":"fox","file":"b.png","label":"B"}]})")};
    std::string error;
    const auto manifest {
        ProfileAvatars::Manifest::load(path.string(), ProfileState::systemFileOperations(), error)};
    EXPECT(!manifest.has_value());
    EXPECT(error.find("duplicate avatar id") != std::string::npos);
    fs::remove(path);
}

TEST_CASE(missingDefaultIsRejected)
{
    const fs::path path {writeTemp("es-de-avatars-default.json",
                                   R"({"version":1,"default":"fox","avatars":[)"
                                   R"({"id":"cat","file":"cat.png","label":"Cat"}]})")};
    std::string error;
    const auto manifest {
        ProfileAvatars::Manifest::load(path.string(), ProfileState::systemFileOperations(), error)};
    EXPECT(!manifest.has_value());
    EXPECT(error.find("not in the list") != std::string::npos);
    fs::remove(path);
}

TEST_CASE(malformedAvatarIdIsRejected)
{
    const fs::path path {writeTemp("es-de-avatars-id.json",
                                   R"({"version":1,"default":"fox","avatars":[)"
                                   R"({"id":"fox","file":"fox.png","label":"Fox"},)"
                                   R"({"id":"Fox","file":"other.png","label":"Other"}]})")};
    std::string error;
    const auto manifest {
        ProfileAvatars::Manifest::load(path.string(), ProfileState::systemFileOperations(), error)};
    EXPECT(!manifest.has_value());
    EXPECT(error.find("malformed") != std::string::npos);
    fs::remove(path);
}

TEST_CASE(duplicateAvatarFileIsRejected)
{
    const fs::path path {writeTemp("es-de-avatars-file.json",
                                   R"({"version":1,"default":"fox","avatars":[)"
                                   R"({"id":"fox","file":"same.png","label":"Fox"},)"
                                   R"({"id":"cat","file":"same.png","label":"Cat"}]})")};
    std::string error;
    const auto manifest {
        ProfileAvatars::Manifest::load(path.string(), ProfileState::systemFileOperations(), error)};
    EXPECT(!manifest.has_value());
    EXPECT(error.find("twice") != std::string::npos);
    fs::remove(path);
}
