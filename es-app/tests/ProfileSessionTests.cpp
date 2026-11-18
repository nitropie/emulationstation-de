//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileSessionTests.cpp
//
//  Test cases for the immutable profile session context and the catalog reader.
//

#include "TestHarness.h"

#include "profiles/ProfileSession.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* sProfileA {"bbb35f7a-a579-4721-8510-2f777c27225a"};
    constexpr const char* sProfileB {"3f6c2a6e-0e8a-4c5b-9d0a-6f8f0e3b1c2d"};

    std::string testData(const std::string& name)
    {
        std::ifstream file {fs::path {ES_APP_TEST_DATA_DIR} / "profile" / name};
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    class EnvironmentGuard
    {
    public:
        EnvironmentGuard()
        {
            remember("NITROPIE_PROFILE_CATALOG");
            remember("NITROPIE_PROFILE_ID");
        }

        ~EnvironmentGuard()
        {
            for (std::size_t index {0}; index < mNames.size(); ++index) {
                if (mValues[index].has_value())
                    setenv(mNames[index].c_str(), mValues[index]->c_str(), 1);
                else
                    unsetenv(mNames[index].c_str());
            }
        }

        void set(const std::string& name, const std::string& value)
        {
            setenv(name.c_str(), value.c_str(), 1);
        }

    private:
        void remember(const std::string& name)
        {
            mNames.push_back(name);
            const char* value {getenv(name.c_str())};
            if (value != nullptr)
                mValues.push_back(std::string {value});
            else
                mValues.push_back(std::nullopt);
        }

        std::vector<std::string> mNames;
        std::vector<std::optional<std::string>> mValues;
    };
} // namespace

TEST_CASE(validCatalogResolvesTheProfile)
{
    using namespace ProfileSession;

    const Context context {fromCatalog(testData("catalog.json"), sProfileA)};
    EXPECT(context.active());
    EXPECT(context.error().empty());
    EXPECT(context.profileId() == sProfileA);
    EXPECT(context.defaultProfileId() == sProfileA);
    EXPECT(context.avatarManifestPath() ==
           "/nix/store/avatars/share/nitropie/avatars/avatars.json");
    EXPECT(context.metadataPath() ==
           "/var/lib/nitropie/profiles/bbb35f7a-a579-4721-8510-2f777c27225a/profile.json");
    EXPECT(context.entry().home ==
           "/var/lib/nitropie/profiles/bbb35f7a-a579-4721-8510-2f777c27225a/home");
    EXPECT(context.entry().appdata ==
           "/var/lib/nitropie/profiles/bbb35f7a-a579-4721-8510-2f777c27225a/home/ES-DE");
    EXPECT(context.entry().saves ==
           "/var/lib/nitropie/profiles/bbb35f7a-a579-4721-8510-2f777c27225a/home/saves");
    EXPECT(context.catalog().profiles.size() == 2);
    EXPECT(context.catalog().libraryRoot == "/var/lib/nitropie/library");

    const Context second {fromCatalog(testData("catalog.json"), sProfileB)};
    EXPECT(second.active());
    EXPECT(second.entry().metadata.find(sProfileB) != std::string::npos);
}

TEST_CASE(missingProfileEntryIsReported)
{
    using namespace ProfileSession;

    const Context context {
        fromCatalog(testData("catalog.json"), "11111111-1111-4111-8111-111111111111")};
    EXPECT(!context.active());
    EXPECT(context.error().find("not in the catalog") != std::string::npos);
}

TEST_CASE(malformedUuidIsReported)
{
    using namespace ProfileSession;

    EXPECT(isUuidV4(sProfileA));
    EXPECT(!isUuidV4("not-a-uuid"));
    EXPECT(!isUuidV4("BBB35F7A-A579-4721-8510-2F777C27225A"));
    EXPECT(!isUuidV4("bbb35f7a-a579-1721-8510-2f777c27225a"));

    const Context context {fromCatalog(testData("catalog.json"), "not-a-uuid")};
    EXPECT(!context.active());
    EXPECT(context.error().find("UUIDv4") != std::string::npos);
}

TEST_CASE(duplicateProfileIdsAreRejected)
{
    using namespace ProfileSession;

    const std::string catalog {
        R"({"version":1,"default":")" + std::string {sProfileA} +
        R"(","libraryRoot":"/l","avatarManifest":"/a.json","profiles":[)" + R"({"id":")" +
        sProfileA +
        R"(","metadata":"/m1","home":"/h1","appdata":"/e1","saves":"/s1",)"
        R"("xdgConfigHome":"/c1","xdgDataHome":"/d1","xdgStateHome":"/t1","xdgCacheHome":"/k1"},)" +
        R"({"id":")" + sProfileA +
        R"(","metadata":"/m2","home":"/h2","appdata":"/e2","saves":"/s2",)"
        R"("xdgConfigHome":"/c2","xdgDataHome":"/d2","xdgStateHome":"/t2","xdgCacheHome":"/k2"}]})"};

    const Context context {fromCatalog(catalog, sProfileA)};
    EXPECT(!context.active());
    EXPECT(context.error().find("duplicate profile id") != std::string::npos);
}

TEST_CASE(absentEnvironmentMeansLegacyMode)
{
    EnvironmentGuard guard;
    unsetenv("NITROPIE_PROFILE_CATALOG");
    unsetenv("NITROPIE_PROFILE_ID");

    const ProfileSession::Context context {
        ProfileSession::fromEnvironment(ProfileState::systemFileOperations())};
    EXPECT(!context.active());
    EXPECT(context.error().empty());
}

TEST_CASE(partialEnvironmentIsReported)
{
    EnvironmentGuard guard;
    unsetenv("NITROPIE_PROFILE_CATALOG");
    guard.set("NITROPIE_PROFILE_ID", sProfileA);

    const ProfileSession::Context context {
        ProfileSession::fromEnvironment(ProfileState::systemFileOperations())};
    EXPECT(!context.active());
    EXPECT(context.error().find("incomplete") != std::string::npos);
}

TEST_CASE(environmentResolvesTheProfile)
{
    EnvironmentGuard guard;
    const fs::path path {fs::temp_directory_path() / "es-de-profile-catalog-test.json"};
    {
        std::ofstream file {path};
        file << testData("catalog.json");
    }

    guard.set("NITROPIE_PROFILE_CATALOG", path.string());
    guard.set("NITROPIE_PROFILE_ID", sProfileA);

    const ProfileSession::Context context {
        ProfileSession::fromEnvironment(ProfileState::systemFileOperations())};
    EXPECT(context.active());
    EXPECT(context.profileId() == sProfileA);
    fs::remove(path);
}
