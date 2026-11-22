//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  WifiManagerTests.cpp
//
//  Test cases for the Wi-Fi manager. The backend tests run the real wpa_supplicant worker on the
//  session bus against FakeWpaSupplicantServer, with a FIFO in place of /dev/rfkill.
//

#include "TestHarness.h"

#include "wifi/WifiManager.h"

#include <cstdlib>
#include <string>

#if !defined(WIFI_MANAGER_WPASUPPLICANT)

TEST_CASE(wifiManagerWithoutABackendIsNull)
{
    EXPECT(!WifiManager::hasBackend());

    WifiManager& manager {WifiManager::getInstance()};
    EXPECT(!manager.isAvailable());

    const std::shared_ptr<const WifiSnapshot> snapshot {manager.getSnapshot()};
    EXPECT(snapshot != nullptr);
    EXPECT(!snapshot->available);
    EXPECT(snapshot->networks.empty());

    // Every action is a safe no-op.
    manager.setPowered(true);
    manager.startScan();
    manager.connect("Cafe");
    manager.connectWithPassword("Cafe", "correct horse battery");
    manager.disconnect();
    manager.forget("Cafe");

    EXPECT(manager.getSequence() >= snapshot->sequence);
}

#else

#include "FakeWpaSupplicantServer.h"
#include "Log.h"
#include "utils/FileSystemUtil.h"
#include "wifi/WifiState.h"

#include <fcntl.h>
#include <linux/rfkill.h>
#include <poll.h>
#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

std::unique_ptr<WifiManager> createWpaSupplicantWifiManager();

namespace
{
    constexpr const char* sInterfacePath {"/fi/w1/wpa_supplicant1/Interfaces/1"};
    constexpr const char* sP2pInterfacePath {"/fi/w1/wpa_supplicant1/Interfaces/2"};
    constexpr const char* sCafeBssPath {"/fi/w1/wpa_supplicant1/Interfaces/1/BSSs/1"};
    constexpr const char* sHomeNetworkPath {"/fi/w1/wpa_supplicant1/Interfaces/1/Networks/0"};
    constexpr const char* sCafeNetworkPath {"/fi/w1/wpa_supplicant1/Interfaces/1/Networks/9"};
    constexpr const char* sCafeDuplicatePath {"/fi/w1/wpa_supplicant1/Interfaces/1/Networks/8"};
    constexpr const char* sOpenCafeBssPath {"/fi/w1/wpa_supplicant1/Interfaces/1/BSSs/2"};
    constexpr const char* sCorrectPassphrase {"correct horse battery"};
    constexpr const char* sWrongPassphrase {"wrong password"};

    std::string makeTempDirectory()
    {
        char pattern[] {"/tmp/es-de-wifi-XXXXXX"};
        char* directory {mkdtemp(pattern)};
        return directory == nullptr ? std::string {} : std::string {directory};
    }

    bool readRfkillEventAt(const int fd, struct rfkill_event& event, const off_t offset)
    {
        const auto deadline {std::chrono::steady_clock::now() + std::chrono::seconds {3}};
        while (std::chrono::steady_clock::now() < deadline) {
            if (pread(fd, &event, sizeof(event), offset) == static_cast<ssize_t>(sizeof(event)))
                return true;

            std::this_thread::sleep_for(std::chrono::milliseconds {5});
        }

        return false;
    }

    std::string readFile(const std::string& path)
    {
        std::ifstream file {path};
        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }

    bool contains(const std::string& text, const std::string& needle)
    {
        return text.find(needle) != std::string::npos;
    }

    bool containsNetwork(const std::shared_ptr<const WifiSnapshot>& snapshot,
                         const std::string& ssid)
    {
        if (snapshot == nullptr)
            return false;

        for (const WifiNetwork& network : snapshot->networks) {
            if (network.ssid == ssid)
                return true;
        }
        return false;
    }

    bool hasSavedNetwork(const std::shared_ptr<const WifiSnapshot>& snapshot,
                         const std::string& ssid)
    {
        if (snapshot == nullptr)
            return false;

        for (const WifiNetwork& network : snapshot->networks) {
            if (network.ssid == ssid && network.saved)
                return true;
        }
        return false;
    }

    bool hasEnabledPath(const FakeWpaSupplicantServer::Calls& calls, const std::string& path)
    {
        return std::find(calls.enabledPaths.begin(), calls.enabledPaths.end(), path) !=
               calls.enabledPaths.end();
    }

    // The direct wire-contract tests talk to the fake server without the manager, so that the
    // old payloads can be sent exactly as the buggy client sent them.
    sd_bus* openSessionBus()
    {
        sd_bus* bus {nullptr};
        if (sd_bus_open_user(&bus) < 0)
            return nullptr;

        return bus;
    }

    sd_bus_message* newInterfaceCall(sd_bus* bus, const char* member)
    {
        sd_bus_message* call {nullptr};
        if (sd_bus_message_new_method_call(bus, &call, "fi.w1.wpa_supplicant1", sInterfacePath,
                                           "fi.w1.wpa_supplicant1.Interface", member) < 0)
            return nullptr;

        if (sd_bus_message_open_container(call, SD_BUS_TYPE_ARRAY, "{sv}") < 0) {
            sd_bus_message_unref(call);
            return nullptr;
        }

        return call;
    }

    void appendStringEntry(sd_bus_message* message, const char* name, const char* value)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "s");
        sd_bus_message_append(message, "s", value);
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
    }

    void appendByteArrayEntry(sd_bus_message* message, const char* name, const std::string& text)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "ay");
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "y");
        for (const char character : text)
            sd_bus_message_append(message, "y", static_cast<unsigned char>(character));
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
    }

    std::string callErrorName(sd_bus* bus, sd_bus_message* call)
    {
        if (call == nullptr)
            return std::string {"<no-call>"};

        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message* reply {nullptr};
        const int result {sd_bus_call(bus, call, 0, &error, &reply)};

        std::string name;
        if (result < 0)
            name = error.name == nullptr ? std::string {} : std::string {error.name};

        sd_bus_error_free(&error);
        if (reply != nullptr)
            sd_bus_message_unref(reply);
        sd_bus_message_unref(call);
        return name;
    }

    struct WifiFixture {
        std::string directory;
        std::string rfkillPath;
        std::string configPath;
        FakeWpaSupplicantServer server;
        std::unique_ptr<WifiManager> manager;
        int rfkillFd {-1};

        explicit WifiFixture(const bool regularRfkillFile = false)
        {
            directory = makeTempDirectory();
            rfkillPath = directory + "/rfkill";
            configPath = directory + "/imperative.conf";

            if (regularRfkillFile) {
                const int create {open(rfkillPath.c_str(), O_CREAT | O_RDWR, 0600)};
                if (create >= 0)
                    close(create);
                rfkillFd = open(rfkillPath.c_str(), O_RDONLY);
            }
            else {
                mkfifo(rfkillPath.c_str(), 0600);
                rfkillFd = open(rfkillPath.c_str(), O_RDWR | O_NONBLOCK);
            }

            setenv("ES_DE_WIFI_DBUS_BUS", "session", 1);
            setenv("ES_DE_WIFI_RFKILL_PATH", rfkillPath.c_str(), 1);
            server.setConfigPath(configPath);
        }

        ~WifiFixture()
        {
            manager.reset();
            server.stop();

            if (rfkillFd >= 0)
                close(rfkillFd);

            unsetenv("ES_DE_WIFI_DBUS_BUS");
            unsetenv("ES_DE_WIFI_RFKILL_PATH");

            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }

        void addInterfaces()
        {
            // The P2P device is listed first on purpose: it must be skipped.
            server.addInterface(sP2pInterfacePath, "p2p-dev-wlan0");
            server.addInterface(sInterfacePath, "wlan0");
        }

        void addCafe()
        {
            server.addBss(sCafeBssPath, {"Cafe", -40, 2412, {"WPA-PSK"}});
            server.setExpectedPassphrase("Cafe", sCorrectPassphrase);
        }

        void addHome() { server.addSavedNetwork(sHomeNetworkPath, "Home", "WPA-PSK", false); }

        void addSavedCafe() { server.addSavedNetwork(sCafeNetworkPath, "Cafe", "WPA-PSK", true); }

        bool start()
        {
            if (!server.start())
                return false;

            manager = createWpaSupplicantWifiManager();
            return manager != nullptr;
        }

        std::shared_ptr<const WifiSnapshot> snapshot() const { return manager->getSnapshot(); }
    };

} // namespace

TEST_CASE(wifiInterfaceDiscoverySkipsP2pDevices)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.addCafe();
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    // The interface becomes available before its BSS objects are fetched, so the wait has to
    // include the network or the assertions race the first property reply.
    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        return fixture.snapshot() != nullptr && fixture.snapshot()->available &&
               containsNetwork(fixture.snapshot(), "Cafe");
    }));

    const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
    EXPECT(snapshot->interfaceName == "wlan0");
    EXPECT(snapshot->interfacePath == sInterfacePath);
    EXPECT(snapshot->networks.size() == 1);
    if (snapshot->networks.size() == 1) {
        EXPECT(snapshot->networks.front().ssid == "Cafe");
        EXPECT(snapshot->networks.front().rssi == -40);
        EXPECT(snapshot->networks.front().signal == 100);
        EXPECT(snapshot->networks.front().security == WifiSecurity::personal);
        EXPECT(snapshot->networks.front().preferredKeyMgmt == WifiKeyMgmt::wpaPsk);
    }
}

TEST_CASE(wifiRfkillPowerWritesEventsAndScans)
{
    // A regular file gives the test its own read offset, so the manager cannot consume the event
    // through its own watch descriptor.
    WifiFixture fixture {true};
    fixture.addInterfaces();
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.snapshot() != nullptr && fixture.snapshot()->available; }));
    EXPECT(fixture.rfkillFd >= 0);

    fixture.manager->setPowered(false);

    struct rfkill_event event {};
    EXPECT(readRfkillEventAt(fixture.rfkillFd, event, 0));
    EXPECT(event.type == RFKILL_TYPE_WLAN);
    EXPECT(event.op == RFKILL_OP_CHANGE_ALL);
    EXPECT(event.soft == 1);
    EXPECT(event.hard == 0);

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] { return !fixture.snapshot()->powered; }));

    fixture.manager->setPowered(true);

    EXPECT(readRfkillEventAt(fixture.rfkillFd, event, sizeof(event)));
    EXPECT(event.type == RFKILL_TYPE_WLAN);
    EXPECT(event.soft == 0);

    // Unblocking the radio triggers a scan once the interface is back.
    EXPECT(
        FakeWpaSupplicantServer::waitFor([&fixture] { return fixture.server.calls().scan >= 1; }));
}

TEST_CASE(wifiScanPublishesAccessPoints)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.addCafe();
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.snapshot() != nullptr && fixture.snapshot()->available; }));

    fixture.manager->startScan();

    EXPECT(
        FakeWpaSupplicantServer::waitFor([&fixture] { return fixture.server.calls().scan == 1; }));
    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return containsNetwork(fixture.snapshot(), "Cafe"); }));

    for (const WifiNetwork& network : fixture.snapshot()->networks) {
        if (network.ssid != "Cafe")
            continue;
        EXPECT(network.signal == 100);
        EXPECT(network.security == WifiSecurity::personal);
        EXPECT(!network.saved);
    }
}

TEST_CASE(wifiConnectWithPasswordSucceedsAndSaves)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.addCafe();
    fixture.addHome();
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        return fixture.snapshot() != nullptr && fixture.snapshot()->available &&
               containsNetwork(fixture.snapshot(), "Home");
    }));

    fixture.manager->connectWithPassword("Cafe", sCorrectPassphrase);

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && snapshot->connected && snapshot->activeSsid == "Cafe";
    }));

    const FakeWpaSupplicantServer::Calls calls {fixture.server.calls()};
    EXPECT(calls.addNetwork == 1);
    EXPECT(calls.selectNetwork == 1);
    EXPECT(calls.addedSsids.size() == 1);
    if (calls.addedSsids.size() == 1)
        EXPECT(calls.addedSsids.front() == "Cafe");

    // The other saved network is re-enabled and the configuration is saved.
    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return hasEnabledPath(fixture.server.calls(), sHomeNetworkPath); }));
    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.server.calls().saveConfig >= 1; }));

    struct stat status {};
    EXPECT(stat(fixture.configPath.c_str(), &status) == 0);
    EXPECT((status.st_mode & 0777) == 0600);

    const std::string config {readFile(fixture.configPath)};
    EXPECT(contains(config, "Cafe"));
    EXPECT(contains(config, sCorrectPassphrase));
}

TEST_CASE(wifiSavedNetworkSelectsWithoutMutation)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    // No expected passphrase here: selecting a saved block must not need one in the fake either.
    fixture.server.addBss(sCafeBssPath, {"Cafe", -40, 2412, {"WPA-PSK"}});
    fixture.addSavedCafe();
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        return fixture.snapshot() != nullptr && fixture.snapshot()->available &&
               hasSavedNetwork(fixture.snapshot(), "Cafe");
    }));

    // A saved or declarative network already has a Network object, so CONNECT only selects it.
    fixture.manager->connect("Cafe");

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && snapshot->connected && snapshot->activeSsid == "Cafe";
    }));

    const FakeWpaSupplicantServer::Calls calls {fixture.server.calls()};
    EXPECT(calls.addNetwork == 0);
    EXPECT(calls.removeNetwork == 0);
    EXPECT(calls.saveConfig == 0);
    EXPECT(calls.selectNetwork == 1);
    if (calls.selectedPaths.size() == 1)
        EXPECT(calls.selectedPaths.front() == sCafeNetworkPath);

    // Selecting an existing block must never create or rewrite imperative.conf.
    std::error_code error;
    if (std::filesystem::exists(fixture.configPath)) {
        EXPECT(std::filesystem::file_size(fixture.configPath, error) == 0);
    }
}

TEST_CASE(wifiSwitchingWhileConnectedIgnoresStaleCompleted)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.server.addBss(sCafeBssPath, {"Cafe", -40, 2412, {"WPA-PSK"}});
    fixture.addHome();
    fixture.addSavedCafe();
    fixture.server.setConnectedState(sHomeNetworkPath, "/");
    fixture.server.setStaleCompletedOnDisconnect(true);
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && snapshot->available && snapshot->connected &&
               snapshot->activeSsid == "Home";
    }));

    // The fake publishes the old completed state again after Disconnect, which used to complete
    // the new attempt with the wrong network.
    fixture.manager->connect("Cafe");

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && snapshot->connected && snapshot->activeSsid == "Cafe";
    }));

    const FakeWpaSupplicantServer::Calls calls {fixture.server.calls()};
    EXPECT(calls.disconnect >= 1);
    EXPECT(calls.addNetwork == 0);
    EXPECT(calls.saveConfig == 0);
    EXPECT(calls.selectNetwork == 1);
    if (calls.selectedPaths.size() == 1)
        EXPECT(calls.selectedPaths.front() == sCafeNetworkPath);
}

TEST_CASE(wifiFailedPasswordKeepsExistingSavedBlock)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.addCafe();
    fixture.addHome();
    fixture.server.setNegativeDisconnectReasons(true);
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        return fixture.snapshot() != nullptr && fixture.snapshot()->available &&
               hasSavedNetwork(fixture.snapshot(), "Home");
    }));

    fixture.manager->connectWithPassword("Cafe", sWrongPassphrase);

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && !snapshot->lastError.empty();
    }));

    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.server.calls().removeNetwork >= 1; }));

    const FakeWpaSupplicantServer::Calls calls {fixture.server.calls()};
    EXPECT(calls.addNetwork == 1);
    EXPECT(calls.selectNetwork == 1);
    EXPECT(calls.reconnect == 0);
    EXPECT(calls.saveConfig == 0);

    // Only the block this attempt created is removed.
    EXPECT(calls.removedPaths.size() == 1);
    if (calls.removedPaths.size() == 1)
        EXPECT(contains(calls.removedPaths.front(), "/Networks/"));

    // The pre-existing saved network is untouched and still saved; it is never re-enabled behind
    // the user's back because it was never disabled.
    EXPECT(!hasEnabledPath(calls, sHomeNetworkPath));
    const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
    EXPECT(hasSavedNetwork(snapshot, "Home"));
    EXPECT(snapshot->lastError == "The Wi-Fi password was not accepted");
    EXPECT(!contains(snapshot->lastError, sWrongPassphrase));

    std::error_code error;
    if (std::filesystem::exists(fixture.configPath)) {
        const std::string config {readFile(fixture.configPath)};
        EXPECT(!contains(config, "Cafe"));
    }
}

TEST_CASE(wifiWrongPasswordOnSavedNetworkPreservesSavedBlock)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.addCafe();
    fixture.addSavedCafe();
    fixture.server.setNegativeDisconnectReasons(true);
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        return fixture.snapshot() != nullptr && fixture.snapshot()->available &&
               hasSavedNetwork(fixture.snapshot(), "Cafe");
    }));

    // ENTER PASSWORD on a saved network must use the typed credential, not silently reuse the
    // existing block.
    fixture.manager->connectWithPassword("Cafe", sWrongPassphrase);

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && snapshot->lastError == "The Wi-Fi password was not accepted";
    }));

    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.server.calls().removeNetwork >= 1; }));

    const FakeWpaSupplicantServer::Calls calls {fixture.server.calls()};
    EXPECT(calls.addNetwork == 1);
    EXPECT(calls.selectNetwork == 1);
    EXPECT(calls.removeNetwork == 1);
    EXPECT(calls.saveConfig == 0);
    EXPECT(calls.reconnect == 0);

    // Only the newly created block is removed; the pre-existing saved block is untouched.
    EXPECT(calls.removedPaths.size() == 1);
    if (calls.removedPaths.size() == 1) {
        EXPECT(contains(calls.removedPaths.front(), "/Networks/"));
        EXPECT(calls.removedPaths.front() != sCafeNetworkPath);
    }
    EXPECT(hasSavedNetwork(fixture.snapshot(), "Cafe"));
}

TEST_CASE(wifiPasswordChangeReplacesDuplicatesAfterSuccess)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.server.addBss(sCafeBssPath, {"Cafe", -40, 2412, {"WPA-PSK"}});
    fixture.server.setExpectedPassphrase("Cafe", sCorrectPassphrase);
    fixture.addHome();
    fixture.addSavedCafe();
    fixture.server.addSavedNetwork(sCafeDuplicatePath, "Cafe", "WPA-PSK", true);
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        return fixture.snapshot() != nullptr && fixture.snapshot()->available &&
               hasSavedNetwork(fixture.snapshot(), "Cafe");
    }));

    fixture.manager->connectWithPassword("Cafe", sCorrectPassphrase);

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && snapshot->connected && snapshot->activeSsid == "Cafe";
    }));
    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.server.calls().saveConfig >= 1; }));

    const FakeWpaSupplicantServer::Calls calls {fixture.server.calls()};
    EXPECT(calls.addNetwork == 1);
    EXPECT(calls.selectNetwork == 1);

    // Both older Cafe blocks are removed only after the new block authenticated.
    EXPECT(calls.removeNetwork == 2);
    EXPECT(calls.removedPaths.size() == 2);
    if (calls.removedPaths.size() == 2) {
        EXPECT(std::find(calls.removedPaths.cbegin(), calls.removedPaths.cend(),
                         sCafeNetworkPath) != calls.removedPaths.cend());
        EXPECT(std::find(calls.removedPaths.cbegin(), calls.removedPaths.cend(),
                         sCafeDuplicatePath) != calls.removedPaths.cend());
    }

    // The other saved network is re-enabled, and both the removals and the enable are complete
    // before SaveConfig runs.
    EXPECT(hasEnabledPath(calls, sHomeNetworkPath));
    EXPECT(calls.removeCountAtSave == 2);
    EXPECT(calls.enableCountAtSave == static_cast<int>(calls.enabledPaths.size()));
    EXPECT(calls.saveConfig == 1);
    EXPECT(hasSavedNetwork(fixture.snapshot(), "Cafe"));
}

TEST_CASE(wifiOrphanAddNetworkReplyIsRemoved)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.addCafe();
    fixture.server.setHoldAddNetwork(true);
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        return fixture.snapshot() != nullptr && fixture.snapshot()->available &&
               containsNetwork(fixture.snapshot(), "Cafe");
    }));

    fixture.manager->connectWithPassword("Cafe", sCorrectPassphrase);
    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.server.calls().addNetwork == 1; }));

    // Abort the attempt while the AddNetwork reply is still in flight.
    fixture.manager->disconnect();
    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.server.calls().disconnect >= 1; }));

    fixture.server.setHoldAddNetwork(false);
    fixture.server.releaseHeldAddNetwork();

    // The late reply must still remove the object it created, so no orphan block is left behind.
    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.server.calls().removeNetwork >= 1; }));

    const FakeWpaSupplicantServer::Calls calls {fixture.server.calls()};
    EXPECT(calls.removedPaths.size() == 1);
    if (calls.removedPaths.size() == 1)
        EXPECT(contains(calls.removedPaths.front(), "/Networks/"));
    EXPECT(calls.saveConfig == 0);
}

TEST_CASE(wifiInvalidPassphraseFailsBeforeDbus)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.addCafe();
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.snapshot() != nullptr && fixture.snapshot()->available; }));

    fixture.manager->connectWithPassword("Cafe", "short");

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && snapshot->lastError == WifiState::passphraseError();
    }));

    EXPECT(fixture.server.calls().addNetwork == 0);
    EXPECT(fixture.server.calls().selectNetwork == 0);
}

TEST_CASE(wifiConnectToUnsavedOpenNetwork)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.server.addBss(sOpenCafeBssPath, {"OpenCafe", -45, 2412, {"NONE"}});
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        return fixture.snapshot() != nullptr && fixture.snapshot()->available &&
               containsNetwork(fixture.snapshot(), "OpenCafe");
    }));

    // An unsaved open network has no Network object, but connect() must still create one.
    fixture.manager->connect("OpenCafe");

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && snapshot->connected && snapshot->activeSsid == "OpenCafe";
    }));

    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.server.calls().saveConfig >= 1; }));

    const FakeWpaSupplicantServer::Calls calls {fixture.server.calls()};
    EXPECT(calls.addNetwork == 1);
    EXPECT(calls.selectNetwork == 1);
    EXPECT(calls.addedSsids.size() == 1);
    if (calls.addedSsids.size() == 1)
        EXPECT(calls.addedSsids.front() == "OpenCafe");
    EXPECT(calls.addedKeyMgmt.size() == 1);
    if (calls.addedKeyMgmt.size() == 1)
        EXPECT(calls.addedKeyMgmt.front() == "NONE");

    const std::string config {readFile(fixture.configPath)};
    EXPECT(contains(config, "OpenCafe"));
}

TEST_CASE(wifiPreConnectedStartupSyncsActiveSsid)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.addCafe();
    fixture.addSavedCafe();
    fixture.server.setConnectedState(sCafeNetworkPath, sCafeBssPath);
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && snapshot->available && snapshot->connected &&
               snapshot->activeSsid == "Cafe";
    }));

    const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
    for (const WifiNetwork& network : snapshot->networks) {
        if (network.ssid == "Cafe")
            EXPECT(network.connected);
    }

    fixture.manager->disconnect();

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> current {fixture.snapshot()};
        return current != nullptr && current->activeSsid.empty();
    }));
    EXPECT(!fixture.snapshot()->connected);
}

TEST_CASE(wifiRfkillEventsUpdateTheSnapshot)
{
    // The FIFO is read by the manager's watch descriptor. Only the test writes here, so no event
    // is lost to a competing reader.
    WifiFixture fixture;
    fixture.addInterfaces();
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.snapshot() != nullptr && fixture.snapshot()->available; }));
    EXPECT(fixture.rfkillFd >= 0);

    struct rfkill_event event {};
    event.op = RFKILL_OP_CHANGE_ALL;
    event.type = RFKILL_TYPE_WLAN;
    event.soft = 1;
    event.hard = 0;
    EXPECT(write(fixture.rfkillFd, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event)));

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] { return !fixture.snapshot()->powered; }));

    event.soft = 0;
    event.hard = 1;
    EXPECT(write(fixture.rfkillFd, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event)));

    EXPECT(
        FakeWpaSupplicantServer::waitFor([&fixture] { return fixture.snapshot()->hardBlocked; }));
    EXPECT(fixture.snapshot()->powered);

    event.soft = 0;
    event.hard = 0;
    EXPECT(write(fixture.rfkillFd, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event)));

    EXPECT(
        FakeWpaSupplicantServer::waitFor([&fixture] { return !fixture.snapshot()->hardBlocked; }));
}

TEST_CASE(wifiDisconnectAndForget)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.addCafe();
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.snapshot() != nullptr && fixture.snapshot()->available; }));

    fixture.manager->connectWithPassword("Cafe", sCorrectPassphrase);

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && snapshot->connected && snapshot->activeSsid == "Cafe";
    }));
    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.server.calls().saveConfig >= 1; }));

    // The configuration exists and contains the network before forget, so the check after it is
    // not vacuous.
    const std::string before {readFile(fixture.configPath)};
    EXPECT(contains(before, "Cafe"));

    const int savesBefore {fixture.server.calls().saveConfig};

    fixture.manager->disconnect();
    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && !snapshot->connected && snapshot->activeSsid.empty();
    }));

    fixture.manager->forget("Cafe");

    // The access point is still visible after forget, but it is no longer saved.
    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return !hasSavedNetwork(fixture.snapshot(), "Cafe"); }));
    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture, savesBefore] { return fixture.server.calls().saveConfig > savesBefore; }));

    const std::string after {readFile(fixture.configPath)};
    EXPECT(!contains(after, "Cafe"));
}

TEST_CASE(wifiUtf8SsidRoundTripsThroughTheWire)
{
    // A UTF-8 SSID with a quote in it is the case that the double quoting used to mangle.
    const std::string ssid {"Caf\xc3\xa9 \"Test\""};

    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.server.addBss(sCafeBssPath, {ssid, -42, 2412, {"WPA-PSK"}});
    fixture.server.setExpectedPassphrase(ssid, sCorrectPassphrase);
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture, &ssid] {
        return fixture.snapshot() != nullptr && fixture.snapshot()->available &&
               containsNetwork(fixture.snapshot(), ssid);
    }));

    fixture.manager->connectWithPassword(ssid, sCorrectPassphrase);

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture, &ssid] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && snapshot->connected && snapshot->activeSsid == ssid;
    }));

    const FakeWpaSupplicantServer::Calls calls {fixture.server.calls()};
    EXPECT(calls.addedSsids.size() == 1);
    if (calls.addedSsids.size() == 1)
        EXPECT(calls.addedSsids.front() == ssid);

    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.server.calls().saveConfig >= 1; }));
    EXPECT(contains(readFile(fixture.configPath), ssid));

    fixture.manager->disconnect();
    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && !snapshot->connected;
    }));

    const int savesBefore {fixture.server.calls().saveConfig};
    fixture.manager->forget(ssid);

    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture, &ssid] { return !hasSavedNetwork(fixture.snapshot(), ssid); }));
    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture, savesBefore] { return fixture.server.calls().saveConfig > savesBefore; }));
    EXPECT(!contains(readFile(fixture.configPath), ssid));
}

TEST_CASE(wifiRawHexPskConnectsAndSaves)
{
    const std::string rawKey {"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};

    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.addCafe();
    fixture.server.setExpectedPassphrase("Cafe", rawKey);
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        return fixture.snapshot() != nullptr && fixture.snapshot()->available &&
               containsNetwork(fixture.snapshot(), "Cafe");
    }));

    fixture.manager->connectWithPassword("Cafe", rawKey);

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && snapshot->connected && snapshot->activeSsid == "Cafe";
    }));

    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.server.calls().saveConfig >= 1; }));

    // A raw key is persisted as 64 hexadecimal digits, without quotes.
    EXPECT(contains(readFile(fixture.configPath), "psk=" + rawKey));
}

TEST_CASE(wifiFakeServerRejectsTheOldWirePayloads)
{
    FakeWpaSupplicantServer server;
    server.addInterface(sInterfacePath, "wlan0");
    server.addBss(sCafeBssPath, {"Cafe", -40, 2412, {"WPA-PSK"}});
    const bool started {server.start()};
    EXPECT(started);
    if (!started)
        return;

    sd_bus* bus {openSessionBus()};
    EXPECT(bus != nullptr);
    if (bus == nullptr) {
        server.stop();
        return;
    }

    // Scan without the Type entry, which is what the manager used to send.
    sd_bus_message* scan {newInterfaceCall(bus, "Scan")};
    sd_bus_message_close_container(scan);
    EXPECT(callErrorName(bus, scan) == "fi.w1.wpa_supplicant1.InvalidArgs");

    // An ssid that is already wrapped in quotes.
    sd_bus_message* quotedSsid {newInterfaceCall(bus, "AddNetwork")};
    appendStringEntry(quotedSsid, "ssid", "\"Cafe\"");
    sd_bus_message_close_container(quotedSsid);
    EXPECT(callErrorName(bus, quotedSsid) == "fi.w1.wpa_supplicant1.InvalidArgs");

    // A passphrase that is already wrapped in quotes.
    sd_bus_message* quotedPsk {newInterfaceCall(bus, "AddNetwork")};
    appendByteArrayEntry(quotedPsk, "ssid", "Cafe");
    appendStringEntry(quotedPsk, "psk", "\"hunter22\"");
    sd_bus_message_close_container(quotedPsk);
    EXPECT(callErrorName(bus, quotedPsk) == "fi.w1.wpa_supplicant1.InvalidArgs");

    // The raw key as a 64 character string instead of 32 raw bytes.
    sd_bus_message* hexPsk {newInterfaceCall(bus, "AddNetwork")};
    appendByteArrayEntry(hexPsk, "ssid", "Cafe");
    appendStringEntry(hexPsk, "psk",
                      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    sd_bus_message_close_container(hexPsk);
    EXPECT(callErrorName(bus, hexPsk) == "fi.w1.wpa_supplicant1.InvalidArgs");

    // A BSS has no Properties property; GetAll is the only way to enumerate it.
    sd_bus_message* getProperties {nullptr};
    EXPECT(sd_bus_message_new_method_call(bus, &getProperties, "fi.w1.wpa_supplicant1",
                                          sCafeBssPath, "org.freedesktop.DBus.Properties",
                                          "Get") >= 0);
    sd_bus_message_append(getProperties, "ss", "fi.w1.wpa_supplicant1.BSS", "Properties");
    EXPECT(callErrorName(bus, getProperties) == "org.freedesktop.DBus.Error.UnknownProperty");

    sd_bus_unref(bus);
    server.stop();
}

TEST_CASE(wifiDaemonRestartRecovers)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.addCafe();
    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.snapshot() != nullptr && fixture.snapshot()->available; }));

    fixture.server.releaseService();
    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] { return !fixture.snapshot()->available; }));

    fixture.server.acquireService();
    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && snapshot->available && snapshot->interfaceName == "wlan0";
    }));
}

TEST_CASE(wifiPassphraseNeverReachesTheLog)
{
    WifiFixture fixture;
    fixture.addInterfaces();
    fixture.addCafe();
    fixture.addHome();

    // getAppDataDirectory() may already be cached by an earlier test, so the log is read from
    // wherever it resolves to rather than from the fixture directory.
    const std::string logDirectory {Utils::FileSystem::getAppDataDirectory() + "/logs"};
    std::filesystem::create_directories(logDirectory);
    Log::init();
    Log::open();
    Log::setReportingLevel(LogDebug);

    EXPECT(fixture.start());
    if (fixture.manager == nullptr)
        return;

    EXPECT(FakeWpaSupplicantServer::waitFor(
        [&fixture] { return fixture.snapshot() != nullptr && fixture.snapshot()->available; }));

    fixture.manager->connectWithPassword("Cafe", sWrongPassphrase);

    EXPECT(FakeWpaSupplicantServer::waitFor([&fixture] {
        const std::shared_ptr<const WifiSnapshot> snapshot {fixture.snapshot()};
        return snapshot != nullptr && !snapshot->lastError.empty();
    }));

    Log::flush();
    Log::close();

    const std::string log {readFile(logDirectory + "/es_log.txt")};
    EXPECT(!log.empty());
    EXPECT(contains(log, "Cafe"));
    EXPECT(!contains(log, sWrongPassphrase));
    EXPECT(!contains(log, sCorrectPassphrase));
}

#endif // WIFI_MANAGER_WPASUPPLICANT
