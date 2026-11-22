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

    // A nested RSN a{sv} with the KeyMgmt suites, which is what the parser reads.
    bool appendKeyMgmtProperty(sd_bus_message* message,
                               const char* name,
                               const std::vector<std::string>& suites)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "a{sv}");
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");

        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", "KeyMgmt");
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "as");
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "s");
        for (const std::string& suite : suites)
            sd_bus_message_append(message, "s", suite.c_str());
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);

        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
        return sd_bus_message_close_container(message) >= 0;
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
    appendInt16Property(signal, "Signal", bss->second.signal);
    appendUint16Property(signal, "Frequency", bss->second.frequency);
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

    sd_bus_message_append(signal, "o", path.c_str());
    sd_bus_message_open_container(signal, SD_BUS_TYPE_ARRAY, "{sv}");
    appendByteArrayProperty(signal, "ssid", byteValues(network->second.ssid));
    appendStringProperty(signal, "key_mgmt", network->second.keyMgmt.c_str());
    appendBooleanProperty(signal, "disabled", !network->second.enabled);
    sd_bus_message_close_container(signal);

    sd_bus_send(mBus, signal, nullptr);
    sd_bus_message_unref(signal);
}

void FakeWpaSupplicantServer::emitNetworkRemoved(const std::string& path)
{
    sd_bus_emit_signal(mBus, mInterfacePath.c_str(), sInterfaceInterface, "NetworkRemoved", "o",
                       path.c_str());
}

std::string FakeWpaSupplicantServer::unquote(const std::string& text)
{
    if (text.size() < 2 || text.front() != '"' || text.back() != '"')
        return text;

    std::string result;
    for (std::size_t index {1}; index + 1 < text.size(); ++index) {
        if (text[index] == '\\' && index + 2 < text.size())
            ++index;
        result.push_back(text[index]);
    }

    return result;
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
        SD_BUS_PROPERTY("DisconnectReason", "i", onInterfaceProperty, 0,
                        SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
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
    static const sd_bus_vtable vtable[] = {SD_BUS_VTABLE_START(0),
                                           SD_BUS_PROPERTY("Properties", "a{sv}", onBssProperty, 0,
                                                           SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
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
        if (!network.second.psk.empty())
            text += "\tpsk=\"" + network.second.psk + "\"\n";
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
    mState = "associating";
    mDisconnectReason = 0;

    WifiProperties associating;
    associating["State"] = std::string {"associating"};
    associating["CurrentNetwork"] = path;
    emitInterfaceProperties(associating);

    const auto expected {mExpectedPassphrases.find(network->second.ssid)};
    const bool accepted {expected == mExpectedPassphrases.end() ||
                         unquote(network->second.psk) == expected->second};

    if (!accepted) {
        mState = "disconnected";
        mDisconnectReason = 15;

        WifiProperties failed;
        failed["State"] = std::string {"disconnected"};
        failed["DisconnectReason"] = std::int64_t {15};
        emitInterfaceProperties(failed);
        return;
    }

    mState = "completed";

    std::string bssPath {sNonePath};
    for (const auto& bss : mBsses) {
        if (bss.second.ssid == network->second.ssid) {
            bssPath = bss.first;
            break;
        }
    }
    mCurrentNetwork = path;
    mCurrentBss = bssPath;

    WifiProperties completed;
    completed["State"] = std::string {"completed"};
    completed["CurrentNetwork"] = path;
    completed["CurrentBSS"] = bssPath;
    emitInterfaceProperties(completed);
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

    if (bss == server->mBsses.end() || std::strcmp(property, "Properties") != 0)
        return sd_bus_error_set(error, SD_BUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");

    sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{sv}");
    appendByteArrayProperty(reply, "SSID", byteValues(bss->second.ssid));
    appendInt16Property(reply, "Signal", bss->second.signal);
    appendUint16Property(reply, "Frequency", bss->second.frequency);
    appendKeyMgmtProperty(reply, "RSN", bss->second.keyMgmt);
    return sd_bus_message_close_container(reply);
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

    sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{sv}");
    appendByteArrayProperty(reply, "ssid", byteValues(network->second.ssid));
    appendStringProperty(reply, "key_mgmt", network->second.keyMgmt.c_str());
    appendBooleanProperty(reply, "disabled", !network->second.enabled);
    return sd_bus_message_close_container(reply);
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

int FakeWpaSupplicantServer::onScan(sd_bus_message* message, void* userdata, sd_bus_error*)
{
    auto* server {static_cast<FakeWpaSupplicantServer*>(userdata)};

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
        return sd_bus_error_set(error, "fi.w1.wpa_supplicant1.Error.InvalidArgs",
                                "Could not read the network properties");

    std::string ssid;
    const auto ssidProperty {properties.find("ssid")};
    if (ssidProperty != properties.end()) {
        if (const auto* bytes {std::get_if<std::vector<std::uint8_t>>(&ssidProperty->second)};
            bytes != nullptr)
            ssid.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        else if (const auto* text {std::get_if<std::string>(&ssidProperty->second)};
                 text != nullptr)
            ssid = WifiState::decodeSsid(*text);
    }

    if (ssid.empty())
        return sd_bus_error_set(error, "fi.w1.wpa_supplicant1.Error.InvalidArgs", "Missing SSID");

    Network network;
    network.ssid = ssid;

    const auto keyMgmt {properties.find("key_mgmt")};
    if (keyMgmt != properties.end()) {
        if (const auto* value {std::get_if<std::string>(&keyMgmt->second)};
            value != nullptr && !value->empty())
            network.keyMgmt = *value;
    }

    const auto psk {properties.find("psk")};
    if (psk != properties.end()) {
        if (const auto* value {std::get_if<std::string>(&psk->second)}; value != nullptr)
            network.psk = unquote(*value);
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
    server->mNetworks.erase(objectPath);

    {
        std::lock_guard<std::mutex> lock {server->mCallsMutex};
        ++server->mCalls.removeNetwork;
        server->mCalls.removedPaths.push_back(objectPath);
    }

    const int result {sd_bus_reply_method_return(message, nullptr)};
    server->emitNetworkRemoved(objectPath);
    return result;
}

int FakeWpaSupplicantServer::onSelectNetwork(sd_bus_message* message, void* userdata, sd_bus_error*)
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

    server->mState = "disconnected";
    server->mSelectedNetwork.clear();
    server->mCurrentNetwork.clear();
    server->mCurrentBss.clear();
    server->mDisconnectReason = 0;

    WifiProperties changed;
    changed["State"] = std::string {"disconnected"};
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
    }
    return sd_bus_reply_method_return(message, nullptr);
}

#endif // WIFI_MANAGER_WPASUPPLICANT
