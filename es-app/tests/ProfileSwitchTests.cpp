//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileSwitchTests.cpp
//
//  Test cases for the client side of the switch protocol: environment parsing, the readiness
//  acknowledgement, the idle and flush gates, the atomic request writer, the chooser request,
//  the exit-code mapping and the final-flush failure path.
//

#include "TestHarness.h"
#include "TestEnvironment.h"

#include "Settings.h"
#include "profiles/ProfileMode.h"
#include "profiles/ProfileSession.h"
#include "profiles/ProfileSwitch.h"
#include "utils/PlatformUtil.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#if !defined(_WIN64)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* sNonce {"aaaabbbbccccddddeeeeffff00001111"};
    constexpr const char* sProfileA {"11111111-1111-4111-8111-111111111111"};
    constexpr const char* sProfileB {"22222222-2222-4222-8222-222222222222"};
    constexpr const char* sRequestId {"00112233445566778899aabbccddeeff"};

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
        return readFile(fs::path {ES_APP_TEST_DATA_DIR} / name);
    }

    void setEnv(const char* name, const std::string& value)
    {
        setenv(name, value.c_str(), 1);
    }

    void clearEnv(const char* name)
    {
        unsetenv(name);
    }

    void clearSwitchEnvironment()
    {
        for (const char* name : {"NITROPIE_SESSION_MODE", "NITROPIE_SESSION_NONCE",
                                 "NITROPIE_READY_FD", "NITROPIE_SESSION_ROOT",
                                 "NITROPIE_SWITCH_REQUEST", "NITROPIE_SESSION_NOTICE",
                                 "NITROPIE_PROFILE_CATALOG", "NITROPIE_PROFILE_ID"})
            clearEnv(name);
        ProfileSwitch::resetForTesting();
        ProfileMode::initialize();
    }

    struct Pipe {
        int readFd {-1};
        int writeFd {-1};

        Pipe()
        {
#if !defined(_WIN64)
            int fds[2] {-1, -1};
            if (pipe(fds) == 0) {
                readFd = fds[0];
                writeFd = fds[1];
            }
#endif
        }

        ~Pipe()
        {
#if !defined(_WIN64)
            if (readFd >= 0)
                close(readFd);
            if (writeFd >= 0)
                close(writeFd);
#endif
        }

        std::string drain()
        {
            std::string out;
#if !defined(_WIN64)
            char buffer[256];
            const ssize_t count {read(readFd, buffer, sizeof(buffer))};
            if (count > 0)
                out.assign(buffer, static_cast<std::size_t>(count));
#endif
            return out;
        }
    };

    // File operations with per-operation failure injection. The seam is the same one the
    // personal-state store uses, so the switch writer is exercised exactly as in production.
    class SwitchFiles final : public ProfileState::FileOperations
    {
    public:
        bool failWriteNew {false};
        bool failSync {false};
        bool failRename {false};
        bool failDirectorySync {false};
        bool failRead {false};
        std::vector<std::string> created;
        std::vector<std::string> removed;
        std::vector<std::pair<std::string, std::string>> renamed;

        ProfileState::ReadStatus readFile(const std::string& path,
                                          std::string& contents) override
        {
            if (failRead)
                return ProfileState::ReadStatus::error;
            return ProfileState::systemFileOperations().readFile(path, contents);
        }

        bool writeFile(const std::string& path, const std::string& contents) override
        {
            return ProfileState::systemFileOperations().writeFile(path, contents);
        }

        bool writeNewFile(const std::string& path, const std::string& contents) override
        {
            if (failWriteNew)
                return false;
            created.push_back(path);
            return ProfileState::systemFileOperations().writeNewFile(path, contents);
        }

        bool syncFile(const std::string& path) override
        {
            if (failSync)
                return false;
            return ProfileState::systemFileOperations().syncFile(path);
        }

        bool renameFile(const std::string& from, const std::string& to) override
        {
            if (failRename)
                return false;
            renamed.emplace_back(from, to);
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
            removed.push_back(path);
            return ProfileState::systemFileOperations().removeFile(path);
        }
    };

    // A chooser-mode environment: mode, nonce, readiness pipe, request path and catalog.
    struct ChooserFixture {
        fs::path root;
        fs::path catalog;
        fs::path request;
        Pipe pipe;

        explicit ChooserFixture(const std::string& name)
            : root {fs::temp_directory_path() / ("es-de-switch-" + name)}
        {
            fs::remove_all(root);
            fs::create_directories(root);
            catalog = root / "catalog.json";
            request = root / "switch-request.json";

            const fs::path metadataA {root / "a/profile.json"};
            const fs::path metadataB {root / "b/profile.json"};
            const fs::path avatars {root / "avatars.json"};
            const fs::path homeA {root / "a/home"};
            const fs::path homeB {root / "b/home"};
            writeFile(metadataA, "{\"schema\":\"nitropie.profile\",\"version\":1,\"id\":\"" +
                                     std::string {sProfileA} +
                                     "\",\"displayName\":\"A\",\"avatar\":\"dog\"}\n");
            writeFile(metadataB, "{\"schema\":\"nitropie.profile\",\"version\":1,\"id\":\"" +
                                     std::string {sProfileB} +
                                     "\",\"displayName\":\"B\",\"avatar\":\"dog\"}\n");
            writeFile(avatars,
                      "{\"version\":1,\"default\":\"dog\",\"avatars\":[{\"id\":\"dog\","
                      "\"file\":\"dog.png\",\"label\":\"Dog\"}]}\n");
            writeFile(catalog,
                      "{\"version\":1,\"default\":\"" + std::string {sProfileA} +
                          "\",\"libraryRoot\":\"" + root.string() +
                          "/library\",\"avatarManifest\":\"" + avatars.string() +
                          "\",\"profiles\":[{\"id\":\"" + std::string {sProfileA} +
                          "\",\"metadata\":\"" + metadataA.string() + "\",\"home\":\"" +
                          homeA.string() + "\",\"appdata\":\"" + homeA.string() +
                          "/ES-DE\",\"saves\":\"" + homeA.string() +
                          "/saves\",\"xdgConfigHome\":\"" + homeA.string() +
                          "/.config\",\"xdgDataHome\":\"" + homeA.string() +
                          "/.local/share\",\"xdgStateHome\":\"" + homeA.string() +
                          "/.local/state\",\"xdgCacheHome\":\"" + homeA.string() +
                          "/.cache\"},{\"id\":\"" + std::string {sProfileB} +
                          "\",\"metadata\":\"" + metadataB.string() + "\",\"home\":\"" +
                          homeB.string() + "\",\"appdata\":\"" + homeB.string() +
                          "/ES-DE\",\"saves\":\"" + homeB.string() +
                          "/saves\",\"xdgConfigHome\":\"" + homeB.string() +
                          "/.config\",\"xdgDataHome\":\"" + homeB.string() +
                          "/.local/share\",\"xdgStateHome\":\"" + homeB.string() +
                          "/.local/state\",\"xdgCacheHome\":\"" + homeB.string() +
                          "/.cache\"}]}\n");
        }

        void enter()
        {
            setEnv("NITROPIE_SESSION_MODE", "chooser");
            setEnv("NITROPIE_SESSION_NONCE", sNonce);
            setEnv("NITROPIE_READY_FD", std::to_string(pipe.writeFd));
            setEnv("NITROPIE_SESSION_ROOT", root.string());
            setEnv("NITROPIE_SWITCH_REQUEST", request.string());
            setEnv("NITROPIE_PROFILE_CATALOG", catalog.string());
            clearEnv("NITROPIE_PROFILE_ID");
            ProfileMode::initialize();
            ProfileSwitch::resetForTesting();
            ProfileSwitch::initialize();
        }
    };

    // A resolved profile session pointing at the test application data directory.
    struct ProfileFixture {
        fs::path root;

        explicit ProfileFixture(const std::string& name)
            : root {fs::temp_directory_path() / ("es-de-switch-profile-" + name)}
        {
            fs::remove_all(root);
            fs::create_directories(root);
        }

        void enter()
        {
            const fs::path appdata {TestEnvironment::appDataDirectory()};
            const fs::path metadata {root / "profile.json"};
            const fs::path avatars {root / "avatars.json"};
            const fs::path catalog {root / "catalog.json"};
            const fs::path home {root / "home"};

            writeFile(metadata, "{\"schema\":\"nitropie.profile\",\"version\":1,\"id\":\"" +
                                    std::string {sProfileA} +
                                    "\",\"displayName\":\"Tester\",\"avatar\":\"dog\"}\n");
            writeFile(avatars,
                      "{\"version\":1,\"default\":\"dog\",\"avatars\":[{\"id\":\"dog\","
                      "\"file\":\"dog.png\",\"label\":\"Dog\"}]}\n");
            writeFile(catalog,
                      "{\"version\":1,\"default\":\"" + std::string {sProfileA} +
                          "\",\"libraryRoot\":\"" + appdata.string() +
                          "\",\"avatarManifest\":\"" + avatars.string() +
                          "\",\"profiles\":[{\"id\":\"" + std::string {sProfileA} +
                          "\",\"metadata\":\"" + metadata.string() + "\",\"home\":\"" +
                          home.string() + "\",\"appdata\":\"" + appdata.string() +
                          "\",\"saves\":\"" + home.string() +
                          "/saves\",\"xdgConfigHome\":\"" + home.string() +
                          "/.config\",\"xdgDataHome\":\"" + home.string() +
                          "/.local/share\",\"xdgStateHome\":\"" + home.string() +
                          "/.local/state\",\"xdgCacheHome\":\"" + home.string() +
                          "/.cache\"},{\"id\":\"" + std::string {sProfileB} +
                          "\",\"metadata\":\"" + (root / "b.json").string() +
                          "\",\"home\":\"" + (root / "bhome").string() +
                          "\",\"appdata\":\"" + (root / "bhome/ES-DE").string() +
                          "\",\"saves\":\"" + (root / "bhome/saves").string() +
                          "\",\"xdgConfigHome\":\"" + (root / "bhome/.config").string() +
                          "\",\"xdgDataHome\":\"" + (root / "bhome/.local/share").string() +
                          "\",\"xdgStateHome\":\"" + (root / "bhome/.local/state").string() +
                          "\",\"xdgCacheHome\":\"" + (root / "bhome/.cache").string() +
                          "\"}]}\n");
            writeFile(root / "b.json", "{\"schema\":\"nitropie.profile\",\"version\":1,"
                                       "\"id\":\"" +
                                           std::string {sProfileB} +
                                           "\",\"displayName\":\"B\",\"avatar\":\"dog\"}\n");

            setEnv("NITROPIE_PROFILE_CATALOG", catalog.string());
            setEnv("NITROPIE_PROFILE_ID", sProfileA);
            ProfileMode::initialize();
        }
    };
} // namespace

TEST_CASE(switchIsDisabledWithoutTheEnvironment)
{
    clearSwitchEnvironment();
    EXPECT(!ProfileSwitch::isEnabled());
    EXPECT(!ProfileSwitch::isChooser());
    EXPECT(!ProfileSwitch::disabledReason().empty());

    std::string error;
    EXPECT(!ProfileSwitch::requestSwitch(sProfileB, error));
    EXPECT(!error.empty());
    EXPECT(!ProfileSwitch::hasRequest());
}

TEST_CASE(switchRejectsAMalformedEnvironment)
{
    ChooserFixture fixture {"malformed"};

    // Wrong nonce length.
    fixture.enter();
    setEnv("NITROPIE_SESSION_NONCE", "abc");
    ProfileSwitch::resetForTesting();
    ProfileSwitch::initialize();
    EXPECT(!ProfileSwitch::isEnabled());

    // Unknown mode.
    clearSwitchEnvironment();
    fixture.enter();
    setEnv("NITROPIE_SESSION_MODE", "bogus");
    ProfileSwitch::resetForTesting();
    ProfileSwitch::initialize();
    EXPECT(!ProfileSwitch::isEnabled());

    // Relative request path.
    clearSwitchEnvironment();
    fixture.enter();
    setEnv("NITROPIE_SWITCH_REQUEST", "relative/switch-request.json");
    ProfileSwitch::resetForTesting();
    ProfileSwitch::initialize();
    EXPECT(!ProfileSwitch::isEnabled());

    clearSwitchEnvironment();
}

TEST_CASE(switchReadyLineIsExactAndSentOnce)
{
    ChooserFixture fixture {"ready"};
    fixture.enter();

#if !defined(_WIN64)
    // initialize() sets FD_CLOEXEC immediately.
    const int flags {fcntl(fixture.pipe.writeFd, F_GETFD)};
    EXPECT(flags >= 0 && (flags & FD_CLOEXEC) != 0);
#endif

    ProfileSwitch::acknowledgeReady();
    const std::string line {"READY " + std::string {sNonce} + " chooser\n"};
    EXPECT(fixture.pipe.drain() == line);

    // A second call writes nothing.
    ProfileSwitch::acknowledgeReady();
    EXPECT(fixture.pipe.drain().empty());

    clearSwitchEnvironment();
}

TEST_CASE(switchChooserRequestWritesTheGoldenBytes)
{
    ChooserFixture fixture {"chooser-golden"};
    fixture.enter();
    SwitchFiles files;
    ProfileSwitch::setFileOperationsForTesting(files);
    ProfileSwitch::setRequestIdForTesting(sRequestId);

    int quits {0};
    ProfileSwitch::setQuitHandlerForTesting([&quits]() { ++quits; });

    std::string error;
    EXPECT(ProfileSwitch::requestSelection(sProfileB, error));
    EXPECT(error.empty());
    EXPECT(quits == 1);
    EXPECT(readFile(fixture.request) == testData("switch-request.golden.json"));
    EXPECT(!fs::exists(fixture.request.string() + ".tmp"));

    // The publication sequence itself: a temp file, then one rename onto the request path.
    EXPECT(files.created.size() == 1 &&
           files.created[0] == fixture.request.string() + ".tmp");
    EXPECT(files.renamed.size() == 1 &&
           files.renamed[0].first == fixture.request.string() + ".tmp" &&
           files.renamed[0].second == fixture.request.string());

    clearSwitchEnvironment();
}

TEST_CASE(switchChooserRequestRefusals)
{
    ChooserFixture fixture {"chooser-refusals"};
    fixture.enter();
    ProfileSwitch::setRequestIdForTesting(sRequestId);
    int quits {0};
    ProfileSwitch::setQuitHandlerForTesting([&quits]() { ++quits; });

    std::string error;
    EXPECT(!ProfileSwitch::requestSelection("33333333-3333-4333-8333-333333333333", error));
    EXPECT(error.find("not in the profile catalog") != std::string::npos);
    EXPECT(!ProfileSwitch::requestSelection("not-a-uuid", error));
    EXPECT(!ProfileSwitch::requestSelection(std::string {sProfileB}.substr(0, 35) + "A", error));
    EXPECT(quits == 0);
    EXPECT(!ProfileSwitch::hasRequest());

    clearSwitchEnvironment();
}

TEST_CASE(switchRequestWriteIsAtomicUnderInjectedFailures)
{
    const struct {
        const char* name;
        bool SwitchFiles::*flag;
    } cases[] = {{"write", &SwitchFiles::failWriteNew},
                 {"sync", &SwitchFiles::failSync},
                 {"rename", &SwitchFiles::failRename},
                 {"directory-sync", &SwitchFiles::failDirectorySync}};

    for (const auto& item : cases) {
        ChooserFixture fixture {std::string {"atomic-"} + item.name};
        fixture.enter();
        ProfileSwitch::setRequestIdForTesting(sRequestId);
        SwitchFiles files;
        files.*(item.flag) = true;
        ProfileSwitch::setFileOperationsForTesting(files);

        int quits {0};
        ProfileSwitch::setQuitHandlerForTesting([&quits]() { ++quits; });

        std::string error;
        EXPECT(!ProfileSwitch::requestSelection(sProfileB, error));
        EXPECT(!error.empty());
        EXPECT(quits == 0);
        EXPECT(!ProfileSwitch::hasRequest());
        EXPECT(!fs::exists(fixture.request.string() + ".tmp"));
        EXPECT(files.created.size() == (item.flag == &SwitchFiles::failWriteNew ? 0U : 1U));

        clearSwitchEnvironment();
    }
}

TEST_CASE(switchExitCodeMappingAndNoShutdownCommand)
{
    std::vector<std::string> commands;
    Utils::Platform::setShutdownCommandRunnerForTesting(
        [&commands](const std::string& command) {
            commands.push_back(command);
            return 0;
        });

    Utils::Platform::quitES(Utils::Platform::QuitMode::SWITCH_PROFILE);
    Utils::Platform::processQuitMode();
    EXPECT(Utils::Platform::quitExitCode() == Utils::Platform::SWITCH_PROFILE_EXIT_CODE);
    EXPECT(Utils::Platform::SWITCH_PROFILE_EXIT_CODE == 42);
    EXPECT(commands.empty());

    Utils::Platform::quitES(Utils::Platform::QuitMode::QUIT);
    Utils::Platform::processQuitMode();
    EXPECT(Utils::Platform::quitExitCode() == 0);
    EXPECT(commands.empty());

    Utils::Platform::quitES(Utils::Platform::QuitMode::REBOOT);
    Utils::Platform::processQuitMode();
    EXPECT(Utils::Platform::quitExitCode() == 0);
    EXPECT(commands.size() == 1 && commands[0] == "reboot");

    Utils::Platform::quitES(Utils::Platform::QuitMode::POWEROFF);
    Utils::Platform::processQuitMode();
    EXPECT(Utils::Platform::quitExitCode() == 0);
    EXPECT(commands.size() == 2 && commands[1] == "poweroff");

    Utils::Platform::setShutdownCommandRunnerForTesting(nullptr);
    Utils::Platform::quitES(Utils::Platform::QuitMode::QUIT);
}

TEST_CASE(switchFinalFlushFailureRemovesTheRequestAndReturnsNonSwitchStatus)
{
    ChooserFixture fixture {"final-flush"};
    fixture.enter();
    ProfileSwitch::setRequestIdForTesting(sRequestId);
    ProfileSwitch::setQuitHandlerForTesting([]() {});

    std::string error;
    EXPECT(ProfileSwitch::requestSelection(sProfileB, error));
    EXPECT(ProfileSwitch::hasRequest());

    const int code {ProfileSwitch::finalizeExitCode(
        Utils::Platform::SWITCH_PROFILE_EXIT_CODE, false)};
    EXPECT(code != Utils::Platform::SWITCH_PROFILE_EXIT_CODE);
    EXPECT(code != 0);
    EXPECT(!ProfileSwitch::hasRequest());

    // A successful final flush keeps the switch status.
    EXPECT(ProfileSwitch::requestSelection(sProfileB, error));
    EXPECT(ProfileSwitch::finalizeExitCode(Utils::Platform::SWITCH_PROFILE_EXIT_CODE, true) ==
           Utils::Platform::SWITCH_PROFILE_EXIT_CODE);

    clearSwitchEnvironment();
}

TEST_CASE(switchSettingsFlushReportsFailure)
{
    // Make the settings directory unwritable, then run the flush group on its own.
    const fs::path settingsDirectory {TestEnvironment::appDataDirectory() / "settings"};
    fs::create_directories(settingsDirectory);
    const fs::path settingsFile {settingsDirectory / "es_settings.xml"};
    writeFile(settingsFile, "<settings/>\n");

    // Both the directory and the file must be read-only: truncating an existing writable file
    // needs no directory permission.
    std::error_code ec;
    fs::permissions(settingsFile, fs::perms::owner_read, fs::perm_options::replace, ec);
    fs::permissions(settingsDirectory, fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace, ec);
    if (!ec) {
        bool saved {true};
        if (geteuid() != 0)
            saved = Settings::getInstance()->saveFile();
        fs::permissions(settingsDirectory, fs::perms::owner_all, fs::perm_options::replace, ec);
        fs::permissions(settingsFile, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, ec);
        if (geteuid() != 0)
            EXPECT(!saved);
    }
    fs::permissions(settingsDirectory, fs::perms::owner_all, fs::perm_options::replace, ec);
    fs::permissions(settingsFile, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
}

TEST_CASE(switchProfileRequestRefusesNonIdleAndUnknownTargets)
{
    ProfileFixture fixture {"refusals"};
    fixture.enter();

    setEnv("NITROPIE_SESSION_MODE", "profile");
    setEnv("NITROPIE_SESSION_NONCE", sNonce);
    setEnv("NITROPIE_SESSION_ROOT", fixture.root.string());
    setEnv("NITROPIE_SWITCH_REQUEST", (fixture.root / "switch-request.json").string());
    Pipe pipe;
    setEnv("NITROPIE_READY_FD", std::to_string(pipe.writeFd));
    ProfileSwitch::resetForTesting();
    ProfileSwitch::initialize();

    EXPECT(ProfileSwitch::isEnabled());

    std::string error;
    // Not a canonical UUIDv4.
    EXPECT(!ProfileSwitch::requestSwitch("not-a-uuid", error));
    // Uppercase letters are not canonical.
    EXPECT(!ProfileSwitch::requestSwitch("ABCDEF01-2345-4678-9ABC-DEF012345678", error));
    // Unknown target.
    EXPECT(!ProfileSwitch::requestSwitch("33333333-3333-4333-8333-333333333333", error));
    // Target equals the active profile.
    EXPECT(!ProfileSwitch::requestSwitch(sProfileA, error));
    EXPECT(error.find("already the active profile") != std::string::npos);
    // A registered blocker refuses the switch.
    ProfileSwitch::registerBlocker("test-blocker", []() { return true; });
    EXPECT(!ProfileSwitch::requestSwitch(sProfileB, error));
    EXPECT(error.find("test-blocker") != std::string::npos);
    EXPECT(!ProfileSwitch::hasRequest());

    clearSwitchEnvironment();
}

TEST_CASE(switchRequestRefusesAFailedFlushGroup)
{
    ProfileFixture fixture {"flush-failure"};
    fixture.enter();

    setEnv("NITROPIE_SESSION_MODE", "profile");
    setEnv("NITROPIE_SESSION_NONCE", sNonce);
    setEnv("NITROPIE_SESSION_ROOT", fixture.root.string());
    setEnv("NITROPIE_SWITCH_REQUEST", (fixture.root / "switch-request.json").string());
    Pipe pipe;
    setEnv("NITROPIE_READY_FD", std::to_string(pipe.writeFd));
    ProfileSwitch::resetForTesting();
    ProfileSwitch::initialize();
    ProfileSwitch::setRequestIdForTesting(sRequestId);

    ProfileSwitch::FlushReport failed;
    failed.overlays = "failure";
    failed.metadata = "ok";
    failed.settings = "ok";
    failed.collections = "ok";
    failed.reason = "overlays: snes: disk full; ";
    ProfileSwitch::setFlushReportForTesting([failed]() { return failed; });

    int quits {0};
    ProfileSwitch::setQuitHandlerForTesting([&quits]() { ++quits; });

    std::string error;
    EXPECT(!ProfileSwitch::requestSwitch(sProfileB, error));
    EXPECT(error.find("flush failed") != std::string::npos);
    EXPECT(quits == 0);
    EXPECT(!ProfileSwitch::hasRequest());

    clearSwitchEnvironment();
}

TEST_CASE(switchProfileRequestWritesTheFlushReport)
{
    ProfileFixture fixture {"success"};
    fixture.enter();

    const fs::path request {fixture.root / "switch-request.json"};
    setEnv("NITROPIE_SESSION_MODE", "profile");
    setEnv("NITROPIE_SESSION_NONCE", sNonce);
    setEnv("NITROPIE_SESSION_ROOT", fixture.root.string());
    setEnv("NITROPIE_SWITCH_REQUEST", request.string());
    Pipe pipe;
    setEnv("NITROPIE_READY_FD", std::to_string(pipe.writeFd));
    ProfileSwitch::resetForTesting();
    ProfileSwitch::initialize();
    ProfileSwitch::setRequestIdForTesting(sRequestId);

    int quits {0};
    ProfileSwitch::setQuitHandlerForTesting([&quits]() { ++quits; });

    std::string error;
    EXPECT(ProfileSwitch::requestSwitch(sProfileB, error));
    EXPECT(error.empty());
    EXPECT(quits == 1);

    const std::string expected {"{\"flush\":{\"collections\":\"nothingToDo\","
                                "\"metadata\":\"nothingToDo\",\"overlays\":\"ok\","
                                "\"settings\":\"ok\"},\"idle\":true,\"requestId\":\"" +
                                std::string {sRequestId} +
                                "\",\"schema\":\"nitropie.profile-switch-request\","
                                "\"sessionNonce\":\"" +
                                std::string {sNonce} + "\",\"sourceProfileId\":\"" +
                                std::string {sProfileA} + "\",\"targetProfileId\":\"" +
                                std::string {sProfileB} + "\",\"version\":1}\n"};
    EXPECT(readFile(request) == expected);

    clearSwitchEnvironment();
}
