//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  FakeWpaSupplicantServer.h
//
//  A wpa_supplicant D-Bus service for the Wi-Fi tests.
//
//  The fake implements the subset of fi.w1.wpa_supplicant1 that the manager talks to: interface,
//  BSS and Network objects, the async methods, the properties and the signals that follow them. It
//  can be reused by the native UI test server as well, so it has no dependency on the test
//  harness.
//

#ifndef ES_CORE_TESTS_FAKE_WPA_SUPPLICANT_SERVER_H
#define ES_CORE_TESTS_FAKE_WPA_SUPPLICANT_SERVER_H

#if defined(WIFI_MANAGER_WPASUPPLICANT)

#include "wifi/WifiMessageParser.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct sd_bus;
struct sd_event;
struct sd_event_source;

class FakeWpaSupplicantServer
{
public:
    struct Bss {
        std::string ssid;
        std::int16_t signal {-50};
        std::uint16_t frequency {2412};
        std::vector<std::string> keyMgmt {"WPA-PSK"};
        std::vector<std::uint8_t> bssid {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    };

    // Everything the manager asked the service to do, for the assertions.
    struct Calls {
        int scan {0};
        int addNetwork {0};
        int removeNetwork {0};
        int selectNetwork {0};
        int saveConfig {0};
        int removeCountAtSave {0};
        int enableCountAtSave {0};
        int disconnect {0};
        int reconnect {0};
        int propertySets {0};
        std::vector<std::string> addedSsids;
        std::vector<std::string> addedKeyMgmt;
        std::vector<std::string> selectedPaths;
        std::vector<std::string> removedPaths;
        std::vector<std::string> enabledPaths;
    };

    FakeWpaSupplicantServer();
    ~FakeWpaSupplicantServer();

    FakeWpaSupplicantServer(const FakeWpaSupplicantServer&) = delete;
    FakeWpaSupplicantServer& operator=(const FakeWpaSupplicantServer&) = delete;

    // Setup. Call these before start().
    void addInterface(const std::string& path, const std::string& ifname);
    void addBss(const std::string& path, const Bss& bss);
    void addSavedNetwork(const std::string& path,
                         const std::string& ssid,
                         const std::string& keyMgmt,
                         const bool enabled);
    // The passphrase that is accepted for an SSID. An SSID without an entry accepts any
    // passphrase.
    void setExpectedPassphrase(const std::string& ssid, const std::string& passphrase);
    // Where SaveConfig writes the simulated imperative.conf.
    void setConfigPath(const std::string& path);
    // A daemon that is already connected when the frontend starts. Call before start().
    void setConnectedState(const std::string& networkPath, const std::string& bssPath);
    // Emit the disconnect reason as a negative value, as the real daemon does.
    void setNegativeDisconnectReasons(const bool value);
    // Emit a stale completed state for the old network after a Disconnect call.
    void setStaleCompletedOnDisconnect(const bool value);
    // Delay the AddNetwork reply until releaseHeldAddNetwork() is called.
    void setHoldAddNetwork(const bool value);
    void releaseHeldAddNetwork();
    // Keep a selected network in the associating state for this many milliseconds before
    // publishing completed, so the UI can show CONNECTING TO.
    void setAssociationDelayMs(const int milliseconds);

    // Starts the service and acquires fi.w1.wpa_supplicant1 on the session bus.
    bool start();
    void stop();

    // Simulates the daemon leaving and returning to the bus.
    void releaseService();
    void acquireService();

    Calls calls() const;
    void resetCalls();

    static bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 5000);

private:
    struct Interface {
        std::string ifname;
    };

    struct Network {
        std::string ssid;
        std::string keyMgmt {"WPA-PSK"};
        std::string psk;
        bool enabled {true};
    };

    void run();
    void registerObjects();
    void registerNetworkObject(const std::string& path);
    void processCommands();
    void post(std::function<void()> command);

    void emitInterfaceProperties(const WifiProperties& changed);
    void emitScanDone();
    void emitBssAdded(const std::string& path);
    void emitNetworkAdded(const std::string& path);
    void emitNetworkRemoved(const std::string& path);

    void writeConfig();
    void selectNetwork(const std::string& path);
    static std::string pathForNetwork(const std::string& ssid);

    static int onRootProperty(sd_bus* bus,
                              const char* path,
                              const char* interfaceName,
                              const char* property,
                              sd_bus_message* reply,
                              void* userdata,
                              sd_bus_error* error);
    static int onInterfaceProperty(sd_bus* bus,
                                   const char* path,
                                   const char* interfaceName,
                                   const char* property,
                                   sd_bus_message* reply,
                                   void* userdata,
                                   sd_bus_error* error);
    static int onBssProperty(sd_bus* bus,
                             const char* path,
                             const char* interfaceName,
                             const char* property,
                             sd_bus_message* reply,
                             void* userdata,
                             sd_bus_error* error);
    static int onNetworkProperty(sd_bus* bus,
                                 const char* path,
                                 const char* interfaceName,
                                 const char* property,
                                 sd_bus_message* reply,
                                 void* userdata,
                                 sd_bus_error* error);
    static int onNetworkEnabled(sd_bus* bus,
                                const char* path,
                                const char* interfaceName,
                                const char* property,
                                sd_bus_message* reply,
                                void* userdata,
                                sd_bus_error* error);
    static int onNetworkEnabledSet(sd_bus* bus,
                                   const char* path,
                                   const char* interfaceName,
                                   const char* property,
                                   sd_bus_message* value,
                                   void* userdata,
                                   sd_bus_error* error);
    static int onScan(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int onAddNetwork(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int onRemoveNetwork(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int onSelectNetwork(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int onDisconnect(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int onReconnect(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int onSaveConfig(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int onWake(sd_event_source* source, int fd, uint32_t revents, void* userdata);

    static const sd_bus_vtable* rootVTable();
    static const sd_bus_vtable* interfaceVTable();
    static const sd_bus_vtable* bssVTable();
    static const sd_bus_vtable* networkVTable();

    sd_bus* mBus {nullptr};
    sd_event* mEvent {nullptr};
    sd_event_source* mWakeSource {nullptr};
    int mWakeFd {-1};
    std::thread mThread;

    // Guarded by the command mutex. The loop thread owns every other member.
    mutable std::mutex mCommandMutex;
    std::vector<std::function<void()>> mCommands;

    mutable std::mutex mCallsMutex;
    Calls mCalls;

    std::mutex mStartMutex;
    std::condition_variable mStartCondition;
    bool mStarted {false};
    bool mStartDone {false};
    std::atomic<bool> mRunning {false};

    std::string mInterfacePath;
    std::map<std::string, Interface> mInterfaces;
    std::map<std::string, Bss> mBsses;
    std::map<std::string, Network> mNetworks;
    std::map<std::string, std::string> mExpectedPassphrases;
    std::string mConfigPath;
    std::string mSelectedNetwork;
    std::string mCurrentNetwork;
    std::string mCurrentBss;
    std::string mState {"disconnected"};
    std::int32_t mDisconnectReason {0};
    bool mScanning {false};
    int mNextNetwork {1};
    bool mNegativeDisconnectReasons {false};
    bool mStaleCompletedOnDisconnect {false};
    bool mHoldAddNetwork {false};
    int mAssociationDelayMs {0};
    sd_event_source* mAssociationTimer {nullptr};
    std::string mAssociationPath;
    void completeSelection(const std::string& path);
    static int onAssociationTimeout(sd_event_source* source, uint64_t, void* userdata);

    struct HeldAddNetwork {
        sd_bus_message* message {nullptr};
        std::string path;
    };
    std::vector<HeldAddNetwork> mHeldAddNetwork;
};

#endif // WIFI_MANAGER_WPASUPPLICANT

#endif // ES_CORE_TESTS_FAKE_WPA_SUPPLICANT_SERVER_H
