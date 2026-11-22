//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  FakeWpaSupplicantServer.cpp
//
//  A wpa_supplicant D-Bus service for the Wi-Fi tests.
//

#include "FakeWpaSupplicantServer.h"

#if defined(WIFI_MANAGER_WPASUPPLICANT)

#include "wifi/WifiState.h"

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <fcntl.h>
#include <linux/rfkill.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace
{
    constexpr const char* sService {"fi.w1.wpa_supplicant1"};
    constexpr const char* sServicePath {"/fi/w1/wpa_supplicant1"};
    constexpr const char* sServiceInterface {"fi.w1.wpa_supplicant1"};
    constexpr const char* sInterfaceInterface {"fi.w1.wpa_supplicant1.Interface"};
    constexpr const char* sBssInterface {"fi.w1.wpa_supplicant1.BSS"};
    constexpr const char* sNetworkInterface {"fi.w1.wpa_supplicant1.Network"};
    constexpr const char* sPropertiesInterface {"org.freedesktop.DBus.Properties"};
    constexpr const char* sNonePath {"/"};

    template <typename Append>
    int replyVariant(sd_bus* bus, sd_bus_message* call, const char* signature, Append append)
    {
        sd_bus_message* reply {nullptr};
        if (sd_bus_message_new_method_return(call, &reply) < 0)
            return -ENOMEM;

        sd_bus_message_open_container(reply, SD_BUS_TYPE_VARIANT, signature);
        append(reply);
        sd_bus_message_close_container(reply);
        sd_bus_send(bus, reply, nullptr);
        sd_bus_message_unref(reply);
        return 1;
    }

    bool appendStringProperty(sd_bus_message* message, const char* name, const char* value)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "s");
        sd_bus_message_append(message, "s", value);
        sd_bus_message_close_container(message);
        return sd_bus_message_close_container(message) >= 0;
    }

    bool appendBooleanProperty(sd_bus_message* message, const char* name, const bool value)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "b");
        sd_bus_message_append(message, "b", value ? 1 : 0);
        sd_bus_message_close_container(message);
        return sd_bus_message_close_container(message) >= 0;
    }

    bool appendInt16Property(sd_bus_message* message, const char* name, const std::int16_t value)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "n");
        sd_bus_message_append(message, "n", value);
        sd_bus_message_close_container(message);
        return sd_bus_message_close_container(message) >= 0;
    }

    bool appendUint16Property(sd_bus_message* message, const char* name, const std::uint16_t value)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "q");
        sd_bus_message_append(message, "q", value);
        sd_bus_message_close_container(message);
        return sd_bus_message_close_container(message) >= 0;
    }

    bool appendInt32Property(sd_bus_message* message, const char* name, const std::int32_t value)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "i");
        sd_bus_message_append(message, "i", value);
        sd_bus_message_close_container(message);
        return sd_bus_message_close_container(message) >= 0;
    }

    bool appendByteArrayProperty(sd_bus_message* message,
                                 const char* name,
                                 const std::vector<std::uint8_t>& bytes)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "ay");
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "y");
        for (const std::uint8_t byte : bytes)
            sd_bus_message_append(message, "y", byte);
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
        return sd_bus_message_close_container(message) >= 0;
    }

    // The contents of a nested WPA or RSN a{sv}: the KeyMgmt suites, which is what the parser
    // reads.
    bool appendKeyMgmtValue(sd_bus_message* message, const std::vector<std::string>& suites)
    {
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}") < 0)
            return false;
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv") < 0)
            return false;
        if (sd_bus_message_append(message, "s", "KeyMgmt") < 0)
            return false;
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "as") < 0)
            return false;
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "s") < 0)
            return false;
        for (const std::string& suite : suites) {
            if (sd_bus_message_append(message, "s", suite.c_str()) < 0)
                return false;
        }
        if (sd_bus_message_close_container(message) < 0)
            return false;
        if (sd_bus_message_close_container(message) < 0)
            return false;
        if (sd_bus_message_close_container(message) < 0)
            return false;
        return sd_bus_message_close_container(message) >= 0;
    }

    // A nested WPA or RSN a{sv} dictionary entry.
    bool appendKeyMgmtProperty(sd_bus_message* message,
                               const char* name,
                               const std::vector<std::string>& suites)
    {
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv") < 0)
            return false;
        if (sd_bus_message_append(message, "s", name) < 0)
            return false;
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "a{sv}") < 0)
            return false;
        if (!appendKeyMgmtValue(message, suites))
            return false;
        if (sd_bus_message_close_container(message) < 0)
            return false;
        return sd_bus_message_close_container(message) >= 0;
    }

    // The raw value of an "ay" property, which is what a BSS SSID and BSSID are.
    int appendByteArrayValue(sd_bus_message* message, const std::vector<std::uint8_t>& bytes)
    {
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "y") < 0)
            return -ENOMEM;

        for (const std::uint8_t byte : bytes) {
            if (sd_bus_message_append(message, "y", byte) < 0)
                return -ENOMEM;
        }

        return sd_bus_message_close_container(message);
    }

    std::string hexKey(const std::vector<std::uint8_t>& bytes)
    {
        static constexpr char digits[] {"0123456789abcdef"};

        std::string key;
        key.reserve(bytes.size() * 2);
        for (const std::uint8_t byte : bytes) {
            key.push_back(digits[byte >> 4]);
            key.push_back(digits[byte & 0x0f]);
        }
        return key;
    }

    bool isPrivateBss(const std::vector<std::string>& keyMgmt)
    {
        return !keyMgmt.empty() && keyMgmt.front() != "NONE";
    }

    // The Properties dictionary of a Network: wpa_config_get_all emits every value as a string.
    void appendNetworkProperties(sd_bus_message* message,
                                 const std::string& ssid,
                                 const std::string& keyMgmt,
                                 const bool enabled)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
        const std::string encodedSsid {WifiState::encodeSsid(ssid)};
        appendStringProperty(message, "ssid", encodedSsid.c_str());
        appendStringProperty(message, "key_mgmt", keyMgmt.c_str());
        appendStringProperty(message, "disabled", enabled ? "0" : "1");
        sd_bus_message_close_container(message);
    }

    std::vector<std::uint8_t> byteValues(const std::string& text)
    {
        return std::vector<std::uint8_t> {text.begin(), text.end()};
    }

    void appendProperties(sd_bus_message* message, const WifiProperties& properties)
    {
        for (const auto& property : properties) {
            if (const auto* text {std::get_if<std::string>(&property.second)}; text != nullptr) {
                appendStringProperty(message, property.first.c_str(), text->c_str());
            }
            else if (const auto* boolean {std::get_if<bool>(&property.second)};
                     boolean != nullptr) {
                appendBooleanProperty(message, property.first.c_str(), *boolean);
            }
            else if (const auto* number {std::get_if<std::int64_t>(&property.second)};
                     number != nullptr) {
                appendInt32Property(message, property.first.c_str(),
                                    static_cast<std::int32_t>(*number));
            }
        }
    }

} // namespace

FakeWpaSupplicantServer::FakeWpaSupplicantServer() = default;

FakeWpaSupplicantServer::~FakeWpaSupplicantServer() { stop(); }

void FakeWpaSupplicantServer::addInterface(const std::string& path, const std::string& ifname)
{
    Interface interface;
    interface.ifname = ifname;
    mInterfaces[path] = interface;
    if (mInterfacePath.empty() && ifname.rfind("p2p-", 0) != 0)
        mInterfacePath = path;
}

void FakeWpaSupplicantServer::addBss(const std::string& path, const Bss& bss)
{
    mBsses[path] = bss;
}

void FakeWpaSupplicantServer::addSavedNetwork(const std::string& path,
                                              const std::string& ssid,
                                              const std::string& keyMgmt,
                                              const bool enabled)
{
    Network network;
    network.ssid = ssid;
    network.keyMgmt = keyMgmt;
    network.enabled = enabled;
    mNetworks[path] = network;
}

void FakeWpaSupplicantServer::setExpectedPassphrase(const std::string& ssid,
                                                    const std::string& passphrase)
{
    mExpectedPassphrases[ssid] = passphrase;
}

void FakeWpaSupplicantServer::setConfigPath(const std::string& path) { mConfigPath = path; }

void FakeWpaSupplicantServer::setConnectedState(const std::string& networkPath,
                                                const std::string& bssPath)
{
    mState = "completed";
    mCurrentNetwork = networkPath;
    mCurrentBss = bssPath;
    mSelectedNetwork = networkPath;
}

void FakeWpaSupplicantServer::setNegativeDisconnectReasons(const bool value)
{
    mNegativeDisconnectReasons = value;
}

void FakeWpaSupplicantServer::setStaleCompletedOnDisconnect(const bool value)
{
    mStaleCompletedOnDisconnect = value;
}

void FakeWpaSupplicantServer::setHoldAddNetwork(const bool value) { mHoldAddNetwork = value; }

void FakeWpaSupplicantServer::setAssociationDelayMs(const int milliseconds)
{
    mAssociationDelayMs = milliseconds;
}

void FakeWpaSupplicantServer::setAuthFailureDelayMs(const int milliseconds)
{
    mAuthFailureDelayMs = milliseconds;
}

void FakeWpaSupplicantServer::setScanFailure(const std::string& errorName)
{
    mScanFailure = errorName;
}

void FakeWpaSupplicantServer::setSelectFailure(const std::string& errorName)
{
    mSelectFailure = errorName;
}

void FakeWpaSupplicantServer::setDropAfterAssociate(const bool value)
{
    mDropAfterAssociate = value;
}

void FakeWpaSupplicantServer::setSuppressNetworkSelected(const bool value)
{
    mSuppressNetworkSelected = value;
}

void FakeWpaSupplicantServer::setInterfaceState(const std::string& state)
{
    post([this, state] { applyInterfaceState(state); });
}

void FakeWpaSupplicantServer::setRfkillPolling(const bool enabled, const int transitionDelayMs)
{
    mRfkillPolling = enabled;
    mRfkillTransitionDelayMs = transitionDelayMs;
}

void FakeWpaSupplicantServer::applyInterfaceState(const std::string& state)
{
    mState = state;

    WifiProperties changed;
    changed["State"] = state;
    changed["Scanning"] = false;
    emitInterfaceProperties(changed);
}

void FakeWpaSupplicantServer::pollRfkillOnce()
{
    if (mRfkillFd < 0)
        return;

    // The frontend appends one rfkill_event per write, so read the last complete event instead
    // of streaming from an offset the writer never shares.
    struct stat status {};
    if (fstat(mRfkillFd, &status) < 0 || status.st_size < static_cast<off_t>(sizeof(rfkill_event)))
        return;

    struct rfkill_event event {};
    if (pread(mRfkillFd, &event, sizeof(event),
              status.st_size - static_cast<off_t>(sizeof(event))) !=
        static_cast<ssize_t>(sizeof(event)))
        return;
    if (event.type != RFKILL_TYPE_WLAN || event.soft == mLastSoft)
        return;

    mLastSoft = event.soft;
    mPendingSoft = event.soft;
    mPendingSince = std::chrono::steady_clock::now();
    if (event.soft == 0) {
        // Until the daemon has finished re-enabling the interface, a Scan is rejected with
        // InterfaceDisabled. This window is what the premature power-on scan raced.
        mRejectScanUntil =
            std::chrono::steady_clock::now() + std::chrono::milliseconds {mRfkillTransitionDelayMs};
    }
}

int FakeWpaSupplicantServer::onRfkillPoll(sd_event_source* source, uint64_t, void* userdata)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};

    server->pollRfkillOnce();

    if (server->mPendingSoft >= 0 &&
        std::chrono::steady_clock::now() - server->mPendingSince >=
            std::chrono::milliseconds {server->mRfkillTransitionDelayMs}) {
        const bool blocked {server->mPendingSoft != 0};
        server->mPendingSoft = -1;
        // A soft block disables the interface; unblocking is followed by a disconnected state,
        // which is what makes the frontend start its automatic scan.
        server->applyInterfaceState(blocked ? "interface_disabled" : "disconnected");
    }

    uint64_t now {0};
    sd_event_now(server->mEvent, CLOCK_MONOTONIC, &now);
    if (sd_event_add_time(server->mEvent, &server->mRfkillTimer, CLOCK_MONOTONIC, now + 20000, 0,
                          onRfkillPoll, server) < 0)
        server->mRfkillTimer = nullptr;

    return 0;
}

void FakeWpaSupplicantServer::releaseHeldAddNetwork()
{
    post([this] {
        for (HeldAddNetwork& held : mHeldAddNetwork) {
            if (held.message == nullptr)
                continue;

            sd_bus_reply_method_return(held.message, "o", held.path.c_str());
            emitNetworkAdded(held.path);
            sd_bus_message_unref(held.message);
            held.message = nullptr;
        }
        mHeldAddNetwork.clear();
    });
}

bool FakeWpaSupplicantServer::start()
{
    {
        std::lock_guard<std::mutex> lock {mStartMutex};
        mStartDone = false;
        mStarted = false;
    }

    mThread = std::thread {&FakeWpaSupplicantServer::run, this};

    std::unique_lock<std::mutex> lock {mStartMutex};
    mStartCondition.wait(lock, [this] { return mStartDone; });
    return mStarted;
}

void FakeWpaSupplicantServer::stop()
{
    if (!mThread.joinable())
        return;

    // sd_event_exit() does not reliably wake a loop that is blocked in another thread on this
    // libsystemd, so the exit runs on the loop thread through the same command queue the test
    // control commands use.
    if (mRunning) {
        post([this] {
            if (mEvent != nullptr)
                sd_event_exit(mEvent, 0);
        });
    }

    mThread.join();
}

void FakeWpaSupplicantServer::releaseService()
{
    post([this] { sd_bus_release_name(mBus, sService); });
}

void FakeWpaSupplicantServer::acquireService()
{
    post([this] { sd_bus_request_name(mBus, sService, 0); });
}

FakeWpaSupplicantServer::Calls FakeWpaSupplicantServer::calls() const
{
    std::lock_guard<std::mutex> lock {mCallsMutex};
    return mCalls;
}

void FakeWpaSupplicantServer::resetCalls()
{
    std::lock_guard<std::mutex> lock {mCallsMutex};
    mCalls = Calls {};
}

bool FakeWpaSupplicantServer::waitFor(const std::function<bool()>& predicate, int timeoutMs)
{
    const auto deadline {std::chrono::steady_clock::now() + std::chrono::milliseconds {timeoutMs}};
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }

    return predicate();
}

void FakeWpaSupplicantServer::run()
{
    const auto finishStart = [this](const bool started) {
        {
            std::lock_guard<std::mutex> lock {mStartMutex};
            mStarted = started;
            mStartDone = true;
        }
        mStartCondition.notify_all();
    };

    if (sd_event_default(&mEvent) < 0) {
        mEvent = nullptr;
        finishStart(false);
        return;
    }

    mWakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (mWakeFd < 0 || sd_event_add_io(mEvent, &mWakeSource, mWakeFd, EPOLLIN, onWake, this) < 0) {
        finishStart(false);
        return;
    }

    if (sd_bus_open_user(&mBus) < 0) {
        mBus = nullptr;
        finishStart(false);
        return;
    }

    sd_bus_attach_event(mBus, mEvent, SD_EVENT_PRIORITY_NORMAL);

    if (mRfkillPolling) {
        if (const char* path {std::getenv("ES_DE_WIFI_RFKILL_PATH")}; path != nullptr)
            mRfkillFd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

        uint64_t now {0};
        sd_event_now(mEvent, CLOCK_MONOTONIC, &now);
        sd_event_add_time(mEvent, &mRfkillTimer, CLOCK_MONOTONIC, now + 20000, 0, onRfkillPoll,
                          this);
    }

    registerObjects();

    const int requestResult {sd_bus_request_name(mBus, sService, 0)};
    if (requestResult < 0) {
        finishStart(false);
        return;
    }

    mRunning = true;
    finishStart(true);
    sd_event_loop(mEvent);
    mRunning = false;

    if (mBus != nullptr) {
        sd_bus_flush(mBus);
        mBus = sd_bus_unref(mBus);
    }
    if (mWakeSource != nullptr) {
        sd_event_source_disable_unref(mWakeSource);
        mWakeSource = nullptr;
    }
    mEvent = sd_event_unref(mEvent);
    if (mWakeFd >= 0) {
        close(mWakeFd);
        mWakeFd = -1;
    }
    if (mRfkillFd >= 0) {
        close(mRfkillFd);
        mRfkillFd = -1;
    }
}

void FakeWpaSupplicantServer::post(std::function<void()> command)
{
    {
        std::lock_guard<std::mutex> lock {mCommandMutex};
        mCommands.push_back(std::move(command));
    }

    const std::uint64_t counter {1};
    if (mWakeFd >= 0) {
        const ssize_t written {write(mWakeFd, &counter, sizeof(counter))};
        (void)written;
    }
}

int FakeWpaSupplicantServer::onWake(sd_event_source*, int, uint32_t, void* userdata)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};

    std::uint64_t counter {0};
    if (server->mWakeFd >= 0) {
        const ssize_t received {read(server->mWakeFd, &counter, sizeof(counter))};
        (void)received;
    }

    server->processCommands();
    return 0;
}

void FakeWpaSupplicantServer::processCommands()
{
    std::vector<std::function<void()>> commands;
    {
        std::lock_guard<std::mutex> lock {mCommandMutex};
        commands.swap(mCommands);
    }

    for (const auto& command : commands)
        command();
}

void FakeWpaSupplicantServer::emitInterfaceProperties(const WifiProperties& changed)
{
    // Recorded so a test can prove that CurrentNetwork is never part of the signals, like the
    // real wpa_supplicant.
    {
        std::lock_guard<std::mutex> lock {mCallsMutex};
        for (const auto& entry : changed)
            mCalls.interfaceChangedProperties.push_back(entry.first);
    }

    sd_bus_message* signal {nullptr};
    if (sd_bus_message_new_signal(mBus, &signal, mInterfacePath.c_str(), sInterfaceInterface,
                                  "PropertiesChanged") < 0)
        return;

    sd_bus_message_open_container(signal, SD_BUS_TYPE_ARRAY, "{sv}");
    appendProperties(signal, changed);
    sd_bus_message_close_container(signal);

    sd_bus_send(mBus, signal, nullptr);
    sd_bus_message_unref(signal);
}

void FakeWpaSupplicantServer::emitNetworkEnabledChanged(const std::string& path)
{
    sd_bus_message* signal {nullptr};
    if (sd_bus_message_new_signal(mBus, &signal, path.c_str(), sNetworkInterface,
                                  "PropertiesChanged") < 0)
        return;

    sd_bus_message_open_container(signal, SD_BUS_TYPE_ARRAY, "{sv}");
    appendBooleanProperty(signal, "Enabled", true);
    sd_bus_message_close_container(signal);

    sd_bus_send(mBus, signal, nullptr);
    sd_bus_message_unref(signal);
}

void FakeWpaSupplicantServer::emitScanDone()
{
    sd_bus_emit_signal(mBus, mInterfacePath.c_str(), sInterfaceInterface, "ScanDone", "b", 1);
}

void FakeWpaSupplicantServer::emitBssAdded(const std::string& path)
{
    const auto bss {mBsses.find(path)};
    if (bss == mBsses.end())
        return;

    sd_bus_message* signal {nullptr};
    if (sd_bus_message_new_signal(mBus, &signal, mInterfacePath.c_str(), sInterfaceInterface,
                                  "BSSAdded") < 0)
        return;

    sd_bus_message_append(signal, "o", path.c_str());
    sd_bus_message_open_container(signal, SD_BUS_TYPE_ARRAY, "{sv}");
    appendByteArrayProperty(signal, "SSID", byteValues(bss->second.ssid));
    appendByteArrayProperty(signal, "BSSID", bss->second.bssid);
    appendBooleanProperty(signal, "Privacy", isPrivateBss(bss->second.keyMgmt));
    appendInt16Property(signal, "Signal", bss->second.signal);
    appendUint16Property(signal, "Frequency", bss->second.frequency);
    appendKeyMgmtProperty(signal, "WPA", bss->second.keyMgmt);
    appendKeyMgmtProperty(signal, "RSN", bss->second.keyMgmt);
    sd_bus_message_close_container(signal);

    sd_bus_send(mBus, signal, nullptr);
    sd_bus_message_unref(signal);
}

void FakeWpaSupplicantServer::emitNetworkAdded(const std::string& path)
{
    const auto network {mNetworks.find(path)};
    if (network == mNetworks.end())
        return;

    sd_bus_message* signal {nullptr};
    if (sd_bus_message_new_signal(mBus, &signal, mInterfacePath.c_str(), sInterfaceInterface,
                                  "NetworkAdded") < 0)
        return;

    // wpas_dbus_signal_network_added wraps the network configuration in a Properties dictionary
    // of strings and adds the Enabled boolean at the top level.
    sd_bus_message_append(signal, "o", path.c_str());
    sd_bus_message_open_container(signal, SD_BUS_TYPE_ARRAY, "{sv}");

    sd_bus_message_open_container(signal, SD_BUS_TYPE_DICT_ENTRY, "sv");
    sd_bus_message_append(signal, "s", "Properties");
    sd_bus_message_open_container(signal, SD_BUS_TYPE_VARIANT, "a{sv}");
    appendNetworkProperties(signal, network->second.ssid, network->second.keyMgmt,
                            network->second.enabled);
    sd_bus_message_close_container(signal);
    sd_bus_message_close_container(signal);

    appendBooleanProperty(signal, "Enabled", network->second.enabled);

    sd_bus_message_close_container(signal);

    sd_bus_send(mBus, signal, nullptr);
    sd_bus_message_unref(signal);
}

void FakeWpaSupplicantServer::emitNetworkRemoved(const std::string& path)
{
    sd_bus_emit_signal(mBus, mInterfacePath.c_str(), sInterfaceInterface, "NetworkRemoved", "o",
                       path.c_str());
}

const sd_bus_vtable* FakeWpaSupplicantServer::rootVTable()
{
    static const sd_bus_vtable vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("Interfaces", "ao", onRootProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_VTABLE_END};

    return vtable;
}

const sd_bus_vtable* FakeWpaSupplicantServer::interfaceVTable()
{
    static const sd_bus_vtable vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Scan", "a{sv}", "", onScan, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("AddNetwork", "a{sv}", "o", onAddNetwork, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("RemoveNetwork", "o", "", onRemoveNetwork, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("SelectNetwork", "o", "", onSelectNetwork, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Disconnect", "", "", onDisconnect, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Reconnect", "", "", onReconnect, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("SaveConfig", "", "", onSaveConfig, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_PROPERTY("Ifname", "s", onInterfaceProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("State", "s", onInterfaceProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Scanning", "b", onInterfaceProperty, 0,
                        SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Networks", "ao", onInterfaceProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("BSSs", "ao", onInterfaceProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CurrentBSS", "o", onInterfaceProperty, 0,
                        SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("CurrentNetwork", "o", onInterfaceProperty, 0,
                        SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("CurrentAuthMode", "s", onInterfaceProperty, 0,
                        SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("DisconnectReason", "i", onInterfaceProperty, 0,
                        SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_SIGNAL("NetworkSelected", "o", 0),
        SD_BUS_SIGNAL("PropertiesChanged", "a{sv}", 0),
        SD_BUS_SIGNAL("ScanDone", "b", 0),
        SD_BUS_SIGNAL("BSSAdded", "oa{sv}", 0),
        SD_BUS_SIGNAL("BSSRemoved", "o", 0),
        SD_BUS_SIGNAL("NetworkAdded", "oa{sv}", 0),
        SD_BUS_SIGNAL("NetworkRemoved", "o", 0),
        SD_BUS_VTABLE_END};

    return vtable;
}

const sd_bus_vtable* FakeWpaSupplicantServer::bssVTable()
{
    // A BSS exposes its properties individually and has no Properties property.
    static const sd_bus_vtable vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("SSID", "ay", onBssProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("BSSID", "ay", onBssProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Privacy", "b", onBssProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Signal", "n", onBssProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Frequency", "q", onBssProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("WPA", "a{sv}", onBssProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("RSN", "a{sv}", onBssProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_SIGNAL("PropertiesChanged", "a{sv}", 0),
        SD_BUS_VTABLE_END};

    return vtable;
}

const sd_bus_vtable* FakeWpaSupplicantServer::networkVTable()
{
    static const sd_bus_vtable vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("Properties", "a{sv}", onNetworkProperty, 0,
                        SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_WRITABLE_PROPERTY("Enabled", "b", onNetworkEnabled, onNetworkEnabledSet, 0, 0),
        SD_BUS_SIGNAL("PropertiesChanged", "a{sv}", 0), SD_BUS_VTABLE_END};

    return vtable;
}

void FakeWpaSupplicantServer::registerObjects()
{
    sd_bus_add_object_vtable(mBus, nullptr, sServicePath, sServiceInterface, rootVTable(), this);

    for (const auto& interface : mInterfaces) {
        sd_bus_add_object_vtable(mBus, nullptr, interface.first.c_str(), sInterfaceInterface,
                                 interfaceVTable(), this);
    }

    for (const auto& bss : mBsses) {
        sd_bus_add_object_vtable(mBus, nullptr, bss.first.c_str(), sBssInterface, bssVTable(),
                                 this);
    }

    for (const auto& network : mNetworks) {
        sd_bus_add_object_vtable(mBus, nullptr, network.first.c_str(), sNetworkInterface,
                                 networkVTable(), this);
    }
}

void FakeWpaSupplicantServer::registerNetworkObject(const std::string& path)
{
    sd_bus_add_object_vtable(mBus, nullptr, path.c_str(), sNetworkInterface, networkVTable(), this);
}

void FakeWpaSupplicantServer::writeConfig()
{
    if (mConfigPath.empty())
        return;

    const int fd {open(mConfigPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600)};
    if (fd < 0)
        return;

    fchmod(fd, 0600);

    std::string text {"# fake wpa_supplicant imperative configuration\n"};
    for (const auto& network : mNetworks) {
        text += "network={\n";
        text += "\tssid=\"" + network.second.ssid + "\"\n";
        if (!network.second.psk.empty()) {
            // A raw key is written as 64 hexadecimal digits without quotes, a passphrase as a
            // quoted string.
            if (network.second.psk.size() == 64)
                text += "\tpsk=" + network.second.psk + "\n";
            else
                text += "\tpsk=\"" + network.second.psk + "\"\n";
        }
        text += "}\n";
    }

    if (!text.empty()) {
        const ssize_t written {write(fd, text.data(), text.size())};
        (void)written;
    }
    close(fd);
}

void FakeWpaSupplicantServer::selectNetwork(const std::string& path)
{
    const auto network {mNetworks.find(path)};
    if (network == mNetworks.end())
        return;

    mSelectedNetwork = path;
    mDisconnectReason = 0;
    mCurrentAuthMode = network->second.keyMgmt == "NONE" ? "NONE" : "WPA2-PSK";

    // The live wpa_supplicant v2.12 capture: the selected network is reported with
    // NetworkSelected, and CurrentNetwork never appears in PropertiesChanged. The selected block
    // is enabled as part of the selection.
    if (!mSuppressNetworkSelected) {
        {
            std::lock_guard<std::mutex> lock {mCallsMutex};
            ++mCalls.networkSelected;
        }
        sd_bus_emit_signal(mBus, mInterfacePath.c_str(), sInterfaceInterface, "NetworkSelected",
                           "o", path.c_str());
    }
    emitNetworkEnabledChanged(path);

    // The scan that precedes the association, exactly as the daemon publishes it.
    mState = "scanning";
    mScanning = true;
    WifiProperties scanning;
    scanning["State"] = std::string {"scanning"};
    scanning["Scanning"] = true;
    emitInterfaceProperties(scanning);

    emitScanDone();
    mScanning = false;
    WifiProperties scanFinished;
    scanFinished["Scanning"] = false;
    emitInterfaceProperties(scanFinished);

    mState = "associating";
    WifiProperties associating;
    associating["State"] = std::string {"associating"};
    associating["CurrentAuthMode"] = mCurrentAuthMode;
    emitInterfaceProperties(associating);

    const auto expected {mExpectedPassphrases.find(network->second.ssid)};
    const bool accepted {expected == mExpectedPassphrases.end() ||
                         network->second.psk == expected->second};

    if (!accepted) {
        mDisconnectReason = mNegativeDisconnectReasons ? -15 : 15;

        if (mAuthFailureDelayMs > 0) {
            if (mAuthFailureTimer != nullptr) {
                sd_event_source_disable_unref(mAuthFailureTimer);
                mAuthFailureTimer = nullptr;
            }

            uint64_t now {0};
            sd_event_now(mEvent, CLOCK_MONOTONIC, &now);
            if (sd_event_add_time(mEvent, &mAuthFailureTimer, CLOCK_MONOTONIC,
                                  now + mAuthFailureDelayMs * 1000, 0, onAuthFailureTimeout,
                                  this) < 0)
                mAuthFailureTimer = nullptr;
            return;
        }

        completeAuthFailure();
        return;
    }

    if (mDropAfterAssociate) {
        mState = "disconnected";
        mDisconnectReason = 0;
        mCurrentNetwork.clear();
        mCurrentBss.clear();
        mCurrentAuthMode = "INACTIVE";

        WifiProperties failed;
        failed["State"] = std::string {"disconnected"};
        failed["DisconnectReason"] = std::int64_t {0};
        failed["CurrentBSS"] = std::string {"/"};
        failed["CurrentAuthMode"] = mCurrentAuthMode;
        emitInterfaceProperties(failed);
        return;
    }

    if (mAssociationDelayMs > 0) {
        mAssociationPath = path;
        if (mAssociationTimer != nullptr) {
            sd_event_source_disable_unref(mAssociationTimer);
            mAssociationTimer = nullptr;
        }

        uint64_t now {0};
        sd_event_now(mEvent, CLOCK_MONOTONIC, &now);
        if (sd_event_add_time(mEvent, &mAssociationTimer, CLOCK_MONOTONIC,
                              now + mAssociationDelayMs * 1000, 0, onAssociationTimeout, this) < 0)
            mAssociationTimer = nullptr;
        return;
    }

    completeSelection(path);
}

void FakeWpaSupplicantServer::completeAuthFailure()
{
    mState = "disconnected";
    mCurrentNetwork.clear();
    mCurrentBss.clear();
    mCurrentAuthMode = "INACTIVE";

    WifiProperties failed;
    failed["State"] = std::string {"disconnected"};
    failed["DisconnectReason"] = std::int64_t {mDisconnectReason};
    failed["CurrentBSS"] = std::string {"/"};
    failed["CurrentAuthMode"] = mCurrentAuthMode;
    emitInterfaceProperties(failed);
}

int FakeWpaSupplicantServer::onAuthFailureTimeout(sd_event_source* source, uint64_t, void* userdata)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};
    sd_event_source_disable_unref(source);
    server->mAuthFailureTimer = nullptr;
    server->completeAuthFailure();
    return 0;
}

void FakeWpaSupplicantServer::completeSelection(const std::string& path)
{
    const auto network {mNetworks.find(path)};
    if (network == mNetworks.end())
        return;

    std::string bssPath {sNonePath};
    for (const auto& bss : mBsses) {
        if (bss.second.ssid == network->second.ssid) {
            bssPath = bss.first;
            break;
        }
    }

    // The association completes before the four-way handshake, and only the property Get reports
    // the selected network.
    mState = "associated";
    mCurrentBss = bssPath;
    WifiProperties associated;
    associated["State"] = std::string {"associated"};
    associated["CurrentBSS"] = bssPath;
    associated["CurrentAuthMode"] = mCurrentAuthMode;
    emitInterfaceProperties(associated);

    mState = "completed";
    mCurrentNetwork = path;
    mCurrentBss = bssPath;
    WifiProperties completed;
    completed["State"] = std::string {"completed"};
    completed["CurrentAuthMode"] = mCurrentAuthMode;
    emitInterfaceProperties(completed);
}

int FakeWpaSupplicantServer::onAssociationTimeout(sd_event_source* source, uint64_t, void* userdata)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};
    sd_event_source_disable_unref(source);
    server->mAssociationTimer = nullptr;

    const std::string path {server->mAssociationPath};
    server->mAssociationPath.clear();
    server->completeSelection(path);
    return 0;
}

int FakeWpaSupplicantServer::onRootProperty(sd_bus*,
                                            const char*,
                                            const char*,
                                            const char* property,
                                            sd_bus_message* reply,
                                            void* userdata,
                                            sd_bus_error* error)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};

    if (std::strcmp(property, "Interfaces") != 0)
        return sd_bus_error_set(error, SD_BUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");

    sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "o");
    for (const auto& interface : server->mInterfaces)
        sd_bus_message_append(reply, "o", interface.first.c_str());

    return sd_bus_message_close_container(reply);
}

int FakeWpaSupplicantServer::onInterfaceProperty(sd_bus*,
                                                 const char* path,
                                                 const char*,
                                                 const char* property,
                                                 sd_bus_message* reply,
                                                 void* userdata,
                                                 sd_bus_error* error)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};
    const std::string objectPath {path == nullptr ? std::string {} : std::string {path}};
    const auto interface {server->mInterfaces.find(objectPath)};

    if (std::strcmp(property, "Ifname") == 0) {
        const std::string ifname {
            interface == server->mInterfaces.end() ? std::string {} : interface->second.ifname};
        return sd_bus_message_append(reply, "s", ifname.c_str());
    }

    if (std::strcmp(property, "State") == 0)
        return sd_bus_message_append(reply, "s", server->mState.c_str());

    if (std::strcmp(property, "Scanning") == 0)
        return sd_bus_message_append(reply, "b", server->mScanning ? 1 : 0);

    if (std::strcmp(property, "CurrentAuthMode") == 0)
        return sd_bus_message_append(reply, "s", server->mCurrentAuthMode.c_str());

    if (std::strcmp(property, "DisconnectReason") == 0)
        return sd_bus_message_append(reply, "i", server->mDisconnectReason);

    if (std::strcmp(property, "CurrentBSS") == 0) {
        const char* current {server->mCurrentBss.empty() ? sNonePath : server->mCurrentBss.c_str()};
        return sd_bus_message_append(reply, "o", current);
    }

    if (std::strcmp(property, "CurrentNetwork") == 0) {
        const char* current {server->mCurrentNetwork.empty() ? sNonePath :
                                                               server->mCurrentNetwork.c_str()};
        return sd_bus_message_append(reply, "o", current);
    }

    if (std::strcmp(property, "Networks") == 0) {
        sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "o");
        for (const auto& network : server->mNetworks)
            sd_bus_message_append(reply, "o", network.first.c_str());
        return sd_bus_message_close_container(reply);
    }

    if (std::strcmp(property, "BSSs") == 0) {
        sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "o");
        for (const auto& bss : server->mBsses)
            sd_bus_message_append(reply, "o", bss.first.c_str());
        return sd_bus_message_close_container(reply);
    }

    return sd_bus_error_set(error, SD_BUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");
}

int FakeWpaSupplicantServer::onBssProperty(sd_bus*,
                                           const char* path,
                                           const char*,
                                           const char* property,
                                           sd_bus_message* reply,
                                           void* userdata,
                                           sd_bus_error* error)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};
    const std::string objectPath {path == nullptr ? std::string {} : std::string {path}};
    const auto bss {server->mBsses.find(objectPath)};

    if (bss == server->mBsses.end())
        return sd_bus_error_set(error, SD_BUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");

    if (std::strcmp(property, "SSID") == 0)
        return appendByteArrayValue(reply, byteValues(bss->second.ssid));
    if (std::strcmp(property, "BSSID") == 0)
        return appendByteArrayValue(reply, bss->second.bssid);
    if (std::strcmp(property, "Privacy") == 0)
        return sd_bus_message_append(reply, "b", isPrivateBss(bss->second.keyMgmt) ? 1 : 0);
    if (std::strcmp(property, "Signal") == 0)
        return sd_bus_message_append(reply, "n", bss->second.signal);
    if (std::strcmp(property, "Frequency") == 0)
        return sd_bus_message_append(reply, "q", bss->second.frequency);
    if (std::strcmp(property, "WPA") == 0 || std::strcmp(property, "RSN") == 0) {
        if (!appendKeyMgmtValue(reply, bss->second.keyMgmt))
            return -ENOMEM;
        return 0;
    }

    return sd_bus_error_set(error, SD_BUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");
}

int FakeWpaSupplicantServer::onNetworkProperty(sd_bus*,
                                               const char* path,
                                               const char*,
                                               const char* property,
                                               sd_bus_message* reply,
                                               void* userdata,
                                               sd_bus_error* error)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};
    const std::string objectPath {path == nullptr ? std::string {} : std::string {path}};
    const auto network {server->mNetworks.find(objectPath)};

    if (network == server->mNetworks.end() || std::strcmp(property, "Properties") != 0)
        return sd_bus_error_set(error, SD_BUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");

    appendNetworkProperties(reply, network->second.ssid, network->second.keyMgmt,
                            network->second.enabled);
    return 0;
}

int FakeWpaSupplicantServer::onNetworkEnabled(sd_bus*,
                                              const char* path,
                                              const char*,
                                              const char* property,
                                              sd_bus_message* reply,
                                              void* userdata,
                                              sd_bus_error* error)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};
    const std::string objectPath {path == nullptr ? std::string {} : std::string {path}};
    const auto network {server->mNetworks.find(objectPath)};

    if (network == server->mNetworks.end() || std::strcmp(property, "Enabled") != 0)
        return sd_bus_error_set(error, SD_BUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");

    return sd_bus_message_append(reply, "b", network->second.enabled ? 1 : 0);
}

int FakeWpaSupplicantServer::onNetworkEnabledSet(sd_bus*,
                                                 const char* path,
                                                 const char*,
                                                 const char* property,
                                                 sd_bus_message* value,
                                                 void* userdata,
                                                 sd_bus_error* error)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};
    const std::string objectPath {path == nullptr ? std::string {} : std::string {path}};
    const auto network {server->mNetworks.find(objectPath)};

    if (network == server->mNetworks.end() || std::strcmp(property, "Enabled") != 0)
        return sd_bus_error_set(error, SD_BUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");

    int boolean {0};
    if (sd_bus_message_read(value, "b", &boolean) < 0)
        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Expected a boolean");

    network->second.enabled = boolean != 0;

    {
        std::lock_guard<std::mutex> lock {server->mCallsMutex};
        ++server->mCalls.propertySets;
        server->mCalls.enabledPaths.push_back(objectPath);
    }

    return 0;
}

int FakeWpaSupplicantServer::onScan(sd_bus_message* message, void* userdata, sd_bus_error* error)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};

    // A call that arrives before the daemon's rfkill event has reached the poll timer still has to
    // be judged against that event, so read it here first.
    server->pollRfkillOnce();

    if (std::chrono::steady_clock::now() < server->mRejectScanUntil)
        return sd_bus_error_set(error, "fi.w1.wpa_supplicant1.InterfaceDisabled",
                                "The interface is still disabled");

    if (server->mState == "interface_disabled")
        return sd_bus_error_set(error, "fi.w1.wpa_supplicant1.InterfaceDisabled",
                                "The interface is disabled");

    if (!server->mScanFailure.empty())
        return sd_bus_error_set(error, server->mScanFailure.c_str(), "Scan failed");

    // wpas_dbus_handler_scan reads the Type entry and rejects a call without one.
    WifiProperties properties;
    if (!WifiMessageParser::readProperties(message, properties))
        return sd_bus_error_set(error, "fi.w1.wpa_supplicant1.InvalidArgs",
                                "Could not read the scan properties");

    const auto typeProperty {properties.find("Type")};
    const auto* typeValue {typeProperty == properties.end() ?
                               nullptr :
                               std::get_if<std::string>(&typeProperty->second)};
    const std::string type {typeValue == nullptr ? std::string {} : *typeValue};

    if (type != "active" && type != "passive")
        return sd_bus_error_set(error, "fi.w1.wpa_supplicant1.InvalidArgs",
                                "Missing or unsupported scan type");

    {
        std::lock_guard<std::mutex> lock {server->mCallsMutex};
        ++server->mCalls.scan;
    }

    server->mScanning = false;
    sd_bus_reply_method_return(message, nullptr);

    server->emitScanDone();
    for (const auto& bss : server->mBsses)
        server->emitBssAdded(bss.first);

    return 1;
}

int FakeWpaSupplicantServer::onAddNetwork(sd_bus_message* message,
                                          void* userdata,
                                          sd_bus_error* error)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};

    WifiProperties properties;
    if (!WifiMessageParser::readProperties(message, properties))
        return sd_bus_error_set(error, "fi.w1.wpa_supplicant1.InvalidArgs",
                                "Could not read the network properties");

    // The SSID is a raw byte array. A string is accepted as the unquoted form only; upstream
    // wraps a string value in double quotes itself, so one that arrives already quoted is the
    // double quoting bug this server has to reject.
    std::string ssid;
    const auto ssidProperty {properties.find("ssid")};
    if (ssidProperty != properties.end()) {
        if (const auto* bytes {std::get_if<std::vector<std::uint8_t>>(&ssidProperty->second)};
            bytes != nullptr) {
            if (!bytes->empty() && bytes->size() <= 32)
                ssid.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        }
        else if (const auto* text {std::get_if<std::string>(&ssidProperty->second)};
                 text != nullptr) {
            if (!text->empty() && text->size() <= 32 && text->front() != '"' && text->back() != '"')
                ssid = *text;
        }
    }

    if (ssid.empty())
        return sd_bus_error_set(error, "fi.w1.wpa_supplicant1.InvalidArgs",
                                "Missing or invalid SSID");

    Network network;
    network.ssid = ssid;

    const auto keyMgmt {properties.find("key_mgmt")};
    if (keyMgmt != properties.end()) {
        if (const auto* value {std::get_if<std::string>(&keyMgmt->second)};
            value != nullptr && !value->empty())
            network.keyMgmt = *value;
    }

    // A passphrase is an unquoted 8..63 character string that upstream quotes itself, or a raw
    // key as 32 bytes. A pre-quoted string and a 64 character string are both rejected.
    const auto psk {properties.find("psk")};
    if (psk != properties.end()) {
        if (const auto* value {std::get_if<std::string>(&psk->second)}; value != nullptr) {
            if (value->size() < 8 || value->size() > 63 ||
                (value->size() >= 2 && value->front() == '"' && value->back() == '"'))
                return sd_bus_error_set(error, "fi.w1.wpa_supplicant1.InvalidArgs",
                                        "Invalid passphrase");
            network.psk = *value;
        }
        else if (const auto* bytes {std::get_if<std::vector<std::uint8_t>>(&psk->second)};
                 bytes != nullptr) {
            if (bytes->size() != 32)
                return sd_bus_error_set(error, "fi.w1.wpa_supplicant1.InvalidArgs",
                                        "Invalid raw key");
            network.psk = hexKey(*bytes);
        }
        else {
            return sd_bus_error_set(error, "fi.w1.wpa_supplicant1.InvalidArgs",
                                    "Invalid passphrase type");
        }
    }

    std::string path {server->mInterfacePath + "/Networks/" +
                      std::to_string(server->mNextNetwork++)};
    server->mNetworks[path] = network;

    // A new object needs its own Properties interface before the manager can read it.
    server->registerNetworkObject(path);

    {
        std::lock_guard<std::mutex> lock {server->mCallsMutex};
        ++server->mCalls.addNetwork;
        server->mCalls.addedSsids.push_back(ssid);
        server->mCalls.addedKeyMgmt.push_back(network.keyMgmt);
    }

    if (server->mHoldAddNetwork) {
        // The reply is deferred until releaseHeldAddNetwork(). The object already exists, which
        // is what a slow daemon looks like to a client that gives up meanwhile.
        server->mHeldAddNetwork.push_back({sd_bus_message_ref(message), path});
        return 1;
    }

    const int result {sd_bus_reply_method_return(message, "o", path.c_str())};
    server->emitNetworkAdded(path);
    return result;
}

int FakeWpaSupplicantServer::onRemoveNetwork(sd_bus_message* message, void* userdata, sd_bus_error*)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};

    const char* path {nullptr};
    if (sd_bus_message_read(message, "o", &path) < 0)
        return -EINVAL;

    const std::string objectPath {path};
    const bool wasCurrent {objectPath == server->mCurrentNetwork};
    server->mNetworks.erase(objectPath);

    {
        std::lock_guard<std::mutex> lock {server->mCallsMutex};
        ++server->mCalls.removeNetwork;
        server->mCalls.removedPaths.push_back(objectPath);
    }

    const int result {sd_bus_reply_method_return(message, nullptr)};
    server->emitNetworkRemoved(objectPath);

    // Removing the active network disconnects, exactly like wpa_supplicant does when a selected
    // network disappears.
    if (wasCurrent) {
        server->mState = "disconnected";
        server->mSelectedNetwork.clear();
        server->mCurrentNetwork.clear();
        server->mCurrentBss.clear();
        server->mCurrentAuthMode = "INACTIVE";

        WifiProperties changed;
        changed["State"] = std::string {"disconnected"};
        changed["CurrentBSS"] = std::string {"/"};
        changed["CurrentAuthMode"] = server->mCurrentAuthMode;
        server->emitInterfaceProperties(changed);
    }

    return result;
}

int FakeWpaSupplicantServer::onSelectNetwork(sd_bus_message* message,
                                             void* userdata,
                                             sd_bus_error* error)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};

    const char* path {nullptr};
    if (sd_bus_message_read(message, "o", &path) < 0)
        return -EINVAL;

    {
        std::lock_guard<std::mutex> lock {server->mCallsMutex};
        ++server->mCalls.selectNetwork;
        server->mCalls.selectedPaths.push_back(path);
    }

    if (!server->mSelectFailure.empty())
        return sd_bus_error_set(error, server->mSelectFailure.c_str(), "Select failed");

    const int result {sd_bus_reply_method_return(message, nullptr)};
    server->selectNetwork(path);
    return result;
}

int FakeWpaSupplicantServer::onDisconnect(sd_bus_message* message, void* userdata, sd_bus_error*)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};

    {
        std::lock_guard<std::mutex> lock {server->mCallsMutex};
        ++server->mCalls.disconnect;
    }

    // A real daemon can still publish the old completed state after Disconnect was sent. The
    // manager must ignore it while it is tearing the old network down.
    if (server->mStaleCompletedOnDisconnect && !server->mCurrentNetwork.empty()) {
        WifiProperties stale;
        stale["State"] = std::string {"completed"};
        stale["CurrentNetwork"] = server->mCurrentNetwork;
        server->emitInterfaceProperties(stale);
    }

    server->mState = "disconnected";
    server->mSelectedNetwork.clear();
    server->mCurrentNetwork.clear();
    server->mCurrentBss.clear();
    server->mCurrentAuthMode = "INACTIVE";
    server->mDisconnectReason = -3;

    WifiProperties changed;
    changed["State"] = std::string {"disconnected"};
    changed["DisconnectReason"] = std::int64_t {-3};
    changed["CurrentBSS"] = std::string {"/"};
    changed["CurrentAuthMode"] = std::string {"INACTIVE"};
    server->emitInterfaceProperties(changed);

    return sd_bus_reply_method_return(message, nullptr);
}

int FakeWpaSupplicantServer::onReconnect(sd_bus_message* message, void* userdata, sd_bus_error*)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};

    {
        std::lock_guard<std::mutex> lock {server->mCallsMutex};
        ++server->mCalls.reconnect;
    }

    return sd_bus_reply_method_return(message, nullptr);
}

int FakeWpaSupplicantServer::onSaveConfig(sd_bus_message* message, void* userdata, sd_bus_error*)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};

    server->writeConfig();

    {
        std::lock_guard<std::mutex> lock {server->mCallsMutex};
        ++server->mCalls.saveConfig;
        server->mCalls.removeCountAtSave = server->mCalls.removeNetwork;
        server->mCalls.enableCountAtSave = static_cast<int>(server->mCalls.enabledPaths.size());
    }
    return sd_bus_reply_method_return(message, nullptr);
}

#endif // WIFI_MANAGER_WPASUPPLICANT
