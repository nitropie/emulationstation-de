//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  WifiManagerWpaSupplicant.cpp
//
//  wpa_supplicant implementation of WifiManager.
//
//  One sd_event loop on its own thread owns one bus connection and one rfkill file descriptor. The
//  loop is the only thread that touches the connection: nothing calls sd_bus_call, every method
//  call is asynchronous and every action enters through a command queue that an eventfd wakes up.
//  The UI thread only reads the published snapshot.
//

#include "wifi/WifiManager.h"

#if defined(WIFI_MANAGER_WPASUPPLICANT)

#include "Log.h"
#include "SystemStatus.h"
#include "wifi/WifiMessageParser.h"
#include "wifi/WifiState.h"

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/rfkill.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr const char* sService {"fi.w1.wpa_supplicant1"};
    constexpr const char* sServicePath {"/fi/w1/wpa_supplicant1"};
    constexpr const char* sServiceInterface {"fi.w1.wpa_supplicant1"};
    constexpr const char* sInterfaceInterface {"fi.w1.wpa_supplicant1.Interface"};
    constexpr const char* sBssInterface {"fi.w1.wpa_supplicant1.BSS"};
    constexpr const char* sNetworkInterface {"fi.w1.wpa_supplicant1.Network"};
    constexpr const char* sPropertiesInterface {"org.freedesktop.DBus.Properties"};
    constexpr const char* sRfkillPath {"/dev/rfkill"};
    constexpr const char* sConnectTimeoutMessage {"The connection attempt timed out"};
    constexpr const char* sAuthFailureMessage {"The Wi-Fi password was not accepted"};

    constexpr std::uint64_t sConnectTimeoutUsec {25 * 1000 * 1000};
    constexpr std::uint64_t sIpPollIntervalUsec {1000 * 1000};
    constexpr int sIpPollAttempts {15};

    bool isAuthenticationFailure(const std::int64_t reason)
    {
        return reason == 2 || reason == 14 || reason == 15 || reason == 23;
    }

    bool isInProgressState(const std::string& state)
    {
        return state == "authenticating" || state == "associating" || state == "4way_handshake" ||
               state == "group_handshake";
    }

    bool isDisconnectedState(const std::string& state)
    {
        return state == "disconnected" || state == "inactive";
    }

    bool startsWith(const std::string& text, const char* prefix)
    {
        return text.rfind(prefix, 0) == 0;
    }

    bool isHexadecimal(const std::string& text)
    {
        if (text.size() != 64)
            return false;

        return std::all_of(text.begin(), text.end(), [](const char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
        });
    }

    // wpa_supplicant accepts a passphrase either as a quoted string, with a backslash before a
    // quote or another backslash, or as the 64 hexadecimal digits of the raw key.
    std::string encodePassphrase(const std::string& passphrase)
    {
        if (isHexadecimal(passphrase))
            return passphrase;

        std::string encoded {"\""};
        for (const char character : passphrase) {
            if (character == '"' || character == '\\')
                encoded.push_back('\\');
            encoded.push_back(character);
        }
        encoded.push_back('"');
        return encoded;
    }

    // The IPv4 address of an interface, empty when it does not have one yet.
    std::string findIpv4Address(const std::string& interfaceName)
    {
        if (interfaceName.empty())
            return std::string {};

        struct ifaddrs* addresses {nullptr};
        if (getifaddrs(&addresses) < 0)
            return std::string {};

        std::string result;
        for (struct ifaddrs* address = addresses; address != nullptr; address = address->ifa_next) {
            if (address->ifa_addr == nullptr || address->ifa_name == nullptr)
                continue;
            if (interfaceName != address->ifa_name)
                continue;
            if (address->ifa_addr->sa_family != AF_INET)
                continue;

            char host[NI_MAXHOST] {};
            const auto* socketAddress {
                reinterpret_cast<const struct sockaddr_in*>(address->ifa_addr)};
            if (inet_ntop(AF_INET, &socketAddress->sin_addr, host, sizeof(host)) != nullptr) {
                result = host;
                break;
            }
        }

        freeifaddrs(addresses);
        return result;
    }

    const WifiPropertyValue* findProperty(const WifiProperties& properties, const char* name)
    {
        const auto property {properties.find(name)};
        return property == properties.end() ? nullptr : &property->second;
    }

    bool getString(const WifiProperties& properties, const char* name, std::string& result)
    {
        const WifiPropertyValue* value {findProperty(properties, name)};
        if (value == nullptr)
            return false;

        const auto text {std::get_if<std::string>(value)};
        if (text == nullptr)
            return false;

        result = *text;
        return true;
    }

    bool getBoolean(const WifiProperties& properties, const char* name, bool& result)
    {
        const WifiPropertyValue* value {findProperty(properties, name)};
        if (value == nullptr)
            return false;

        const auto boolean {std::get_if<bool>(value)};
        if (boolean == nullptr)
            return false;

        result = *boolean;
        return true;
    }

    bool getSigned(const WifiProperties& properties, const char* name, std::int64_t& result)
    {
        const WifiPropertyValue* value {findProperty(properties, name)};
        if (value == nullptr)
            return false;

        if (const auto number {std::get_if<std::int64_t>(value)}; number != nullptr) {
            result = *number;
            return true;
        }
        if (const auto number {std::get_if<std::uint64_t>(value)}; number != nullptr) {
            result = static_cast<std::int64_t>(*number);
            return true;
        }

        return false;
    }

    bool getStringList(const WifiProperties& properties,
                       const char* name,
                       std::vector<std::string>& result)
    {
        const WifiPropertyValue* value {findProperty(properties, name)};
        if (value == nullptr)
            return false;

        const auto list {std::get_if<std::vector<std::string>>(value)};
        if (list == nullptr)
            return false;

        result = *list;
        return true;
    }

    // Enters the variant that carries a Properties.Get reply.
    bool enterVariant(sd_bus_message* message)
    {
        char type {0};
        const char* contents {nullptr};
        if (sd_bus_message_peek_type(message, &type, &contents) < 0)
            return false;
        if (type != SD_BUS_TYPE_VARIANT)
            return false;

        return sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, contents) >= 0;
    }

    bool readVariantString(sd_bus_message* message, std::string& result)
    {
        if (!enterVariant(message))
            return false;

        char type {0};
        const char* contents {nullptr};
        if (sd_bus_message_peek_type(message, &type, &contents) < 0)
            return false;

        const char* text {nullptr};
        if (type == SD_BUS_TYPE_OBJECT_PATH) {
            if (sd_bus_message_read(message, "o", &text) < 0)
                return false;
        }
        else {
            if (sd_bus_message_read(message, "s", &text) < 0)
                return false;
        }

        result = text == nullptr ? std::string {} : std::string {text};
        return sd_bus_message_exit_container(message) >= 0;
    }

    bool readVariantObjectPaths(sd_bus_message* message, std::vector<std::string>& paths)
    {
        if (!enterVariant(message))
            return false;

        if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "o") < 0)
            return false;

        const char* path {nullptr};
        int result {0};
        while ((result = sd_bus_message_read(message, "o", &path)) > 0)
            paths.emplace_back(path);

        if (result < 0)
            return false;
        if (sd_bus_message_exit_container(message) < 0)
            return false;

        return sd_bus_message_exit_container(message) >= 0;
    }

    bool readVariantProperties(sd_bus_message* message, WifiProperties& properties)
    {
        if (!enterVariant(message))
            return false;

        if (!WifiMessageParser::readProperties(message, properties))
            return false;

        return sd_bus_message_exit_container(message) >= 0;
    }

    bool appendStringProperty(sd_bus_message* message, const char* name, const char* value)
    {
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv") < 0)
            return false;
        if (sd_bus_message_append(message, "s", name) < 0)
            return false;
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "s") < 0)
            return false;
        if (sd_bus_message_append(message, "s", value) < 0)
            return false;
        if (sd_bus_message_close_container(message) < 0)
            return false;

        return sd_bus_message_close_container(message) >= 0;
    }

    bool appendUint32Property(sd_bus_message* message, const char* name, std::uint32_t value)
    {
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv") < 0)
            return false;
        if (sd_bus_message_append(message, "s", name) < 0)
            return false;
        if (sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "u") < 0)
            return false;
        if (sd_bus_message_append(message, "u", value) < 0)
            return false;
        if (sd_bus_message_close_container(message) < 0)
            return false;

        return sd_bus_message_close_container(message) >= 0;
    }

    class WpaSupplicantWifiManager : public WifiManager
    {
    private:
        enum class Step {
            interfaces,
            ifname,
            interfaceProperties,
            bssProperties,
            networkProperties,
            scan,
            addNetwork,
            selectNetwork,
            removeNetworkForConnect,
            removeNetworkForForget,
            saveConfigForForget,
            enableNetwork,
            saveConfig,
            reconnect,
            disconnect,
            quiet
        };

        // What a completion callback needs besides the reply itself. A reply is dropped when the
        // interface epoch it was sent in has passed, or when the connection attempt it belongs to
        // is over, so that a stale reply can never overwrite newer state.
        struct FollowUp {
            Step step {Step::interfaces};
            std::uint64_t epoch {0};
            std::uint64_t serial {0};
            std::string path;
            std::string ssid;
            WpaSupplicantWifiManager* manager {nullptr};
        };

        struct Command {
            enum class Type {
                stop,
                refresh,
                setPowered,
                startScan,
                connect,
                connectWithPassword,
                disconnect,
                forget
            };

            Type type {Type::refresh};
            std::string ssid;
            std::string passphrase;
            bool state {false};
        };

    public:
        WpaSupplicantWifiManager()
        {
            mWakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (mWakeFd < 0) {
                LOG(LogError) << "Wi-Fi: could not create the wake file descriptor: "
                              << std::strerror(errno);
                publish();
                return;
            }

            publish();
            mThread = std::thread {&WpaSupplicantWifiManager::run, this};
        }

        ~WpaSupplicantWifiManager() override { stop(); }

        bool isAvailable() const override
        {
            const std::shared_ptr<const WifiSnapshot> snapshot {getSnapshot()};
            return snapshot != nullptr && snapshot->available;
        }

        std::shared_ptr<const WifiSnapshot> getSnapshot() const override
        {
            std::lock_guard<std::mutex> lock {mSnapshotMutex};
            return mSnapshot;
        }

        std::uint64_t getSequence() const override { return mSequence; }

        void setPowered(const bool powered) override
        {
            postCommand({Command::Type::setPowered, std::string {}, std::string {}, powered});
        }

        void startScan() override
        {
            postCommand({Command::Type::startScan, std::string {}, std::string {}, false});
        }

        void connect(const std::string& ssid) override
        {
            postCommand({Command::Type::connect, ssid, std::string {}, false});
        }

        void connectWithPassword(const std::string& ssid, const std::string& passphrase) override
        {
            postCommand({Command::Type::connectWithPassword, ssid, passphrase, false});
        }

        void disconnect() override
        {
            postCommand({Command::Type::disconnect, std::string {}, std::string {}, false});
        }

        void forget(const std::string& ssid) override
        {
            postCommand({Command::Type::forget, ssid, std::string {}, false});
        }

    private:
        void run()
        {
            if (sd_event_default(&mEvent) < 0) {
                LOG(LogError) << "Wi-Fi: could not create the event loop";
                mEvent = nullptr;
                return;
            }

            if (sd_event_add_io(mEvent, nullptr, mWakeFd, EPOLLIN, onWake, this) < 0) {
                LOG(LogError) << "Wi-Fi: could not watch the wake file descriptor";
                mEvent = sd_event_unref(mEvent);
                return;
            }

            openBus();
            openRfkill();

            if (mBus != nullptr) {
                addMatches();
                requestInterfaces();
            }

            const int result {sd_event_loop(mEvent)};
            if (result < 0) {
                LOG(LogError) << "Wi-Fi: the event loop failed: " << std::strerror(-result);
            }

            cancelConnectTimer();
            cancelIpTimer();

            if (mBus != nullptr) {
                sd_bus_flush(mBus);
                mBus = sd_bus_unref(mBus);
            }
            mEvent = sd_event_unref(mEvent);

            for (FollowUp* followUp : mFollowUps)
                delete followUp;
            mFollowUps.clear();
        }

        void stop()
        {
            if (!mThread.joinable())
                return;

            {
                std::lock_guard<std::mutex> lock {mCommandMutex};
                mCommands.push_back({Command::Type::stop, std::string {}, std::string {}, false});
            }
            wake();
            mThread.join();

            if (mRfkillFd >= 0) {
                close(mRfkillFd);
                mRfkillFd = -1;
            }

            if (mWakeFd >= 0) {
                close(mWakeFd);
                mWakeFd = -1;
            }
        }

        void wake()
        {
            if (mWakeFd < 0)
                return;

            const std::uint64_t counter {1};
            if (write(mWakeFd, &counter, sizeof(counter)) < 0 && errno != EAGAIN) {
                LOG(LogWarning) << "Wi-Fi: could not wake the event loop: " << std::strerror(errno);
            }
        }

        void postCommand(Command command)
        {
            {
                std::lock_guard<std::mutex> lock {mCommandMutex};
                mCommands.push_back(std::move(command));
            }
            wake();
        }

        void openBus()
        {
            const char* busKind {std::getenv("ES_DE_WIFI_DBUS_BUS")};
            const bool useSession {busKind != nullptr && std::strcmp(busKind, "session") == 0};

            const int result {useSession ? sd_bus_open_user(&mBus) : sd_bus_open_system(&mBus)};
            if (result < 0) {
                LOG(LogWarning) << "Wi-Fi: could not connect to the bus: "
                                << std::strerror(-result);
                mBus = nullptr;
                mState.setLastError("The Wi-Fi service is not available");
                publish();
                return;
            }

            sd_bus_attach_event(mBus, mEvent, SD_EVENT_PRIORITY_NORMAL);
        }

        void openRfkill()
        {
            const char* path {std::getenv("ES_DE_WIFI_RFKILL_PATH")};
            const char* rfkillPath {path == nullptr ? sRfkillPath : path};

            mRfkillFd = open(rfkillPath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (mRfkillFd < 0) {
                LOG(LogWarning) << "Wi-Fi: could not open " << rfkillPath << ": "
                                << std::strerror(errno);
                return;
            }

            // A regular file is what the tests use to read the written events. Watching it with
            // epoll would spin on the always-ready EOF, and a write to it does not generate an
            // event anyway.
            struct stat status {};
            if (fstat(mRfkillFd, &status) == 0 && S_ISREG(status.st_mode))
                return;

            if (sd_event_add_io(mEvent, &mRfkillSource, mRfkillFd, EPOLLIN, onRfkill, this) < 0) {
                LOG(LogWarning) << "Wi-Fi: could not watch " << rfkillPath;
                close(mRfkillFd);
                mRfkillFd = -1;
                mRfkillSource = nullptr;
            }
        }

        void addMatches()
        {
            const char* matches[] = {
                "type='signal',sender='fi.w1.wpa_supplicant1',path='/fi/w1/wpa_supplicant1',"
                "interface='fi.w1.wpa_supplicant1',member='InterfaceAdded'",
                "type='signal',sender='fi.w1.wpa_supplicant1',path='/fi/w1/wpa_supplicant1',"
                "interface='fi.w1.wpa_supplicant1',member='InterfaceRemoved'",
                "type='signal',sender='fi.w1.wpa_supplicant1',"
                "interface='fi.w1.wpa_supplicant1.Interface'",
                "type='signal',sender='fi.w1.wpa_supplicant1',"
                "interface='fi.w1.wpa_supplicant1.BSS'",
                "type='signal',sender='fi.w1.wpa_supplicant1',"
                "interface='fi.w1.wpa_supplicant1.Network'",
                "type='signal',sender='fi.w1.wpa_supplicant1',"
                "interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
                "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                "arg0='fi.w1.wpa_supplicant1'",
            };

            for (const char* match : matches) {
                if (sd_bus_add_match_async(mBus, nullptr, match, onSignal, nullptr, this) < 0) {
                    LOG(LogWarning) << "Wi-Fi: could not subscribe to a signal: " << match;
                }
            }
        }

        //------------------------------------------------------------------------------------------
        // Interface discovery.

        void requestInterfaces()
        {
            if (mBus == nullptr)
                return;

            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus, &message, sService, sServicePath,
                                               sPropertiesInterface, "Get") < 0) {
                setError("The Wi-Fi service is not available");
                return;
            }

            sd_bus_message_append(message, "ss", sServiceInterface, "Interfaces");
            send(message, Step::interfaces, mEpoch, 0, std::string {}, std::string {});
        }

        void selectInterface(const std::string& path, const std::string& ifname)
        {
            mInterfacePath = path;
            mInterfaceName = ifname;
            ++mEpoch;

            mState.setAvailable(true);
            mState.setInterfacePath(path);
            mState.setInterfaceName(ifname);
            publish();

            requestInterfaceProperties();
        }

        void requestInterfaceProperties()
        {
            if (mInterfacePath.empty())
                return;

            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus, &message, sService, mInterfacePath.c_str(),
                                               sPropertiesInterface, "GetAll") < 0) {
                setError("The Wi-Fi service is not available");
                return;
            }

            sd_bus_message_append(message, "s", sInterfaceInterface);
            send(message, Step::interfaceProperties, mEpoch, 0, mInterfacePath, std::string {});
        }

        void requestObjectProperties(const std::string& path,
                                     const char* interfaceName,
                                     const Step step)
        {
            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus, &message, sService, path.c_str(),
                                               sPropertiesInterface, "Get") < 0)
                return;

            sd_bus_message_append(message, "ss", interfaceName, "Properties");
            send(message, step, mEpoch, 0, path, std::string {});
        }

        //------------------------------------------------------------------------------------------
        // Actions, everything below runs on the loop thread.

        void performSetPowered(const bool powered)
        {
            if (mRfkillFd < 0) {
                LOG(LogWarning) << "Wi-Fi: the radio state cannot be written without rfkill";
                setError("The Wi-Fi radio cannot be controlled");
                return;
            }

            struct rfkill_event event {};
            event.op = RFKILL_OP_CHANGE_ALL;
            event.type = RFKILL_TYPE_WLAN;
            event.soft = powered ? 0 : 1;
            event.hard = 0;

            const ssize_t written {write(mRfkillFd, &event, sizeof(event))};
            if (written != static_cast<ssize_t>(sizeof(event))) {
                LOG(LogWarning) << "Wi-Fi: could not write the rfkill event: "
                                << std::strerror(errno);
                setError("The Wi-Fi radio state could not be changed");
                return;
            }

            // The published state follows the accepted write, never the other way around.
            mRfkillKnown = true;
            mRadioPowered = powered;
            mState.setPowered(powered);
            publish();

            // A radio that just came back cannot be scanned by wpa_supplicant until it has
            // re-enabled its interface, so the scan is attempted here as well.
            if (powered && !mInterfacePath.empty())
                performStartScan();
        }

        void performStartScan()
        {
            if (!hasInterface())
                return;

            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus, &message, sService, mInterfacePath.c_str(),
                                               sInterfaceInterface, "Scan") < 0) {
                setError("The Wi-Fi service is not available");
                return;
            }

            sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
            sd_bus_message_close_container(message);
            send(message, Step::scan, mEpoch, 0, mInterfacePath, std::string {});
        }

        bool beginAttempt(const std::string& ssid,
                          std::string passphrase,
                          const bool createdNetwork)
        {
            if (mAttempt.active)
                return false;
            if (!hasInterface())
                return false;

            cancelConnectTimer();
            mAttempt.active = true;
            mAttempt.serial = ++mAttemptSerial;
            mAttempt.ssid = ssid;
            mAttempt.passphrase = std::move(passphrase);
            mAttempt.createdNetwork = createdNetwork;
            mAttempt.newNetworkPath.clear();

            mState.setPending(ssid, WifiOp::connecting);
            mState.setLastError(std::string {});
            publish();

            LOG(LogInfo) << "Wi-Fi: starting a connection attempt for '" << ssid << "'";
            return true;
        }

        void performConnect(const std::string& ssid)
        {
            if (!hasInterface())
                return;

            const std::vector<std::string> paths {mState.savedNetworkPaths(ssid)};
            if (!paths.empty()) {
                if (!beginAttempt(ssid, std::string {}, false))
                    return;

                selectNetwork(paths.front());
                return;
            }

            // An unsaved open network has no Network object yet, so it is added here the same way
            // a network with a password is, only without a passphrase and with key_mgmt NONE.
            const WifiNetwork* target {findNetwork(ssid)};

            if (target != nullptr && target->security == WifiSecurity::enterprise) {
                mState.setLastError("Enterprise networks cannot be configured here");
                mState.clearPending(ssid);
                publish();
                return;
            }

            if (target == nullptr || target->security != WifiSecurity::open) {
                mState.setLastError("The network is no longer known");
                mState.clearPending(ssid);
                publish();
                return;
            }

            if (!beginAttempt(ssid, std::string {}, true))
                return;

            mConnectRemovalQueue.clear();
            addNetworkForAttempt();
        }

        void performConnectWithPassword(const std::string& ssid, std::string& passphrase)
        {
            if (mAttempt.active || !hasInterface())
                return;

            const WifiNetwork* target {findNetwork(ssid)};

            if (target != nullptr && target->security == WifiSecurity::enterprise) {
                mState.setLastError("Enterprise networks cannot be configured here");
                mState.clearPending(ssid);
                publish();
                return;
            }

            const bool open {target != nullptr && target->security == WifiSecurity::open};
            if (!open && !WifiState::validatePassphrase(passphrase)) {
                mState.setLastError(WifiState::passphraseError());
                mState.clearPending(ssid);
                publish();
                return;
            }

            if (!beginAttempt(ssid, passphrase, true))
                return;

            // A failed earlier attempt can have left a Network object for this SSID behind, and a
            // new one with a new password must not race against it.
            mConnectRemovalQueue = mState.savedNetworkPaths(ssid);
            removeNextForConnect();
        }

        void removeNextForConnect()
        {
            if (!mAttempt.active)
                return;

            if (mConnectRemovalQueue.empty()) {
                addNetworkForAttempt();
                return;
            }

            const std::string path {mConnectRemovalQueue.front()};
            mConnectRemovalQueue.erase(mConnectRemovalQueue.begin());

            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus, &message, sService, mInterfacePath.c_str(),
                                               sInterfaceInterface, "RemoveNetwork") < 0) {
                removeNextForConnect();
                return;
            }

            sd_bus_message_append(message, "o", path.c_str());
            send(message, Step::removeNetworkForConnect, mEpoch, mAttempt.serial, path,
                 mAttempt.ssid);
        }

        void addNetworkForAttempt()
        {
            if (!mAttempt.active)
                return;

            const WifiNetwork* target {findNetwork(mAttempt.ssid)};
            const WifiSecurity security {target == nullptr ? WifiSecurity::personal :
                                                             target->security};
            const WifiKeyMgmt keyMgmt {target == nullptr ? WifiKeyMgmt::wpaPsk :
                                                           target->preferredKeyMgmt};

            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus, &message, sService, mInterfacePath.c_str(),
                                               sInterfaceInterface, "AddNetwork") < 0) {
                failAttempt("The Wi-Fi service is not available");
                return;
            }

            sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");

            const std::string encodedSsid {WifiState::encodeSsid(mAttempt.ssid)};
            appendStringProperty(message, "ssid", encodedSsid.c_str());

            if (security == WifiSecurity::open) {
                appendStringProperty(message, "key_mgmt", "NONE");
            }
            else if (keyMgmt == WifiKeyMgmt::sae) {
                appendStringProperty(message, "key_mgmt", "SAE");
                appendUint32Property(message, "ieee80211w", 2);
                std::string psk {encodePassphrase(mAttempt.passphrase)};
                appendStringProperty(message, "psk", psk.c_str());
                std::fill(psk.begin(), psk.end(), '\0');
            }
            else {
                appendStringProperty(message, "key_mgmt", "WPA-PSK");
                std::string psk {encodePassphrase(mAttempt.passphrase)};
                appendStringProperty(message, "psk", psk.c_str());
                std::fill(psk.begin(), psk.end(), '\0');
            }

            sd_bus_message_close_container(message);

            // The message owns its copy of the credential now. The transient buffers are zeroed
            // before the call is even sent.
            std::fill(mAttempt.passphrase.begin(), mAttempt.passphrase.end(), '\0');
            mAttempt.passphrase.clear();

            send(message, Step::addNetwork, mEpoch, mAttempt.serial, mInterfacePath, mAttempt.ssid);
        }

        void selectNetwork(const std::string& path)
        {
            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus, &message, sService, mInterfacePath.c_str(),
                                               sInterfaceInterface, "SelectNetwork") < 0) {
                failAttempt("The Wi-Fi service is not available");
                return;
            }

            sd_bus_message_append(message, "o", path.c_str());
            send(message, Step::selectNetwork, mEpoch, mAttempt.serial, path, mAttempt.ssid);
        }

        void performDisconnect()
        {
            if (!hasInterface())
                return;

            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus, &message, sService, mInterfacePath.c_str(),
                                               sInterfaceInterface, "Disconnect") < 0) {
                setError("The Wi-Fi service is not available");
                return;
            }

            send(message, Step::disconnect, mEpoch, 0, mInterfacePath, std::string {});
        }

        void performForget(const std::string& ssid)
        {
            const std::vector<std::string> paths {mState.savedNetworkPaths(ssid)};
            if (paths.empty())
                return;

            mForgetSsid = ssid;
            mForgetQueue = paths;
            mForgetError.clear();
            mState.setPending(ssid, WifiOp::forgetting);
            publish();

            removeNextForForget();
        }

        void removeNextForForget()
        {
            if (mForgetQueue.empty()) {
                saveConfigAfterForget();
                return;
            }

            const std::string path {mForgetQueue.front()};
            mForgetQueue.erase(mForgetQueue.begin());

            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus, &message, sService, mInterfacePath.c_str(),
                                               sInterfaceInterface, "RemoveNetwork") < 0) {
                mForgetError = "The Wi-Fi service is not available";
                finishForget();
                return;
            }

            sd_bus_message_append(message, "o", path.c_str());
            send(message, Step::removeNetworkForForget, mEpoch, 0, path, mForgetSsid);
        }

        void saveConfigAfterForget()
        {
            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus, &message, sService, mInterfacePath.c_str(),
                                               sInterfaceInterface, "SaveConfig") < 0) {
                mForgetError = "The Wi-Fi service is not available";
                finishForget();
                return;
            }

            send(message, Step::saveConfigForForget, mEpoch, 0, mInterfacePath, mForgetSsid);
        }

        void finishForget()
        {
            if (!mForgetSsid.empty())
                mState.clearPending(mForgetSsid);
            mForgetSsid.clear();
            mForgetQueue.clear();
            mState.setLastError(mForgetError);
            mForgetError.clear();
            publish();
        }

        //------------------------------------------------------------------------------------------
        // Connection completion and rollback.

        void startConnectTimer()
        {
            cancelConnectTimer();

            uint64_t now {0};
            sd_event_now(mEvent, CLOCK_MONOTONIC, &now);
            if (sd_event_add_time(mEvent, &mConnectTimerSource, CLOCK_MONOTONIC,
                                  now + sConnectTimeoutUsec, 0, onConnectTimeout, this) < 0) {
                LOG(LogWarning) << "Wi-Fi: could not start the connection timer";
                mConnectTimerSource = nullptr;
            }
        }

        void cancelConnectTimer()
        {
            if (mConnectTimerSource != nullptr) {
                sd_event_source_disable_unref(mConnectTimerSource);
                mConnectTimerSource = nullptr;
            }
        }

        void attemptSucceeded()
        {
            if (!mAttempt.active)
                return;

            const std::string ssid {mAttempt.ssid};
            mAttempt.active = false;
            ++mAttemptSerial;
            mConnectRemovalQueue.clear();
            cancelConnectTimer();

            mState.clearPending(ssid);
            mState.setLastError(std::string {});
            mState.setInterfaceState("completed");
            mState.setActiveSsid(ssid);
            publish();

            LOG(LogInfo) << "Wi-Fi: connected to '" << ssid << "'";

            reEnableSavedNetworks();
            callSaveConfig();

            SystemStatus::getInstance().setPollImmediately(true);
            mIpPollStarted = false;
            mIpPollCounter = 0;
            syncActiveConnection();
        }

        void failAttempt(const std::string& reason)
        {
            if (!mAttempt.active)
                return;

            const std::string ssid {mAttempt.ssid};
            const std::string createdPath {mAttempt.newNetworkPath};
            const bool created {mAttempt.createdNetwork};

            mAttempt.active = false;
            ++mAttemptSerial;
            mConnectRemovalQueue.clear();
            cancelConnectTimer();

            mState.clearPending(ssid);
            mState.setLastError(reason);
            publish();

            LOG(LogInfo) << "Wi-Fi: the connection attempt for '" << ssid << "' failed: " << reason;

            // A block that AddNetwork created is removed again, so the rejected credential is
            // never written to imperative.conf.
            if (created && !createdPath.empty())
                callRemoveNetworkQuiet(createdPath);

            reEnableSavedNetworks(createdPath);
            if (hasInterface())
                callMethodQuiet(mInterfacePath, sInterfaceInterface, "Reconnect");
        }

        void reEnableSavedNetworks(const std::string& excludedPath = std::string {})
        {
            if (!hasInterface())
                return;

            for (const std::string& path : mState.allNetworkPaths()) {
                if (!excludedPath.empty() && path == excludedPath)
                    continue;
                setNetworkEnabled(path, true);
            }
        }

        void setNetworkEnabled(const std::string& path, const bool enabled)
        {
            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus, &message, sService, path.c_str(),
                                               sPropertiesInterface, "Set") < 0)
                return;

            sd_bus_message_append(message, "ss", sNetworkInterface, "Enabled");
            sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "b");
            sd_bus_message_append(message, "b", enabled ? 1 : 0);
            sd_bus_message_close_container(message);

            send(message, Step::enableNetwork, mEpoch, 0, path, std::string {});
        }

        void callSaveConfig()
        {
            if (!hasInterface())
                return;

            callMethodQuiet(mInterfacePath, sInterfaceInterface, "SaveConfig");
        }

        void pollIpAddress()
        {
            const std::string address {findIpv4Address(mInterfaceName)};
            if (!address.empty()) {
                mState.setIpAddress(address);
                publish();
                cancelIpTimer();
                return;
            }

            if (mIpPollCounter >= sIpPollAttempts)
                return;

            ++mIpPollCounter;

            cancelIpTimer();
            uint64_t now {0};
            sd_event_now(mEvent, CLOCK_MONOTONIC, &now);
            if (sd_event_add_time(mEvent, &mIpTimerSource, CLOCK_MONOTONIC,
                                  now + sIpPollIntervalUsec, 0, onIpPoll, this) < 0) {
                mIpTimerSource = nullptr;
            }
        }

        void cancelIpTimer()
        {
            if (mIpTimerSource != nullptr) {
                sd_event_source_disable_unref(mIpTimerSource);
                mIpTimerSource = nullptr;
            }
        }

        //------------------------------------------------------------------------------------------
        // State changes coming from the daemon.

        void applyInterfaceProperties(const WifiProperties& properties)
        {
            std::string state;
            const bool hasState {getString(properties, "State", state)};
            const std::string previousState {mInterfaceState};

            if (hasState) {
                mInterfaceState = state;
                mState.setInterfaceState(state);

                // Until an rfkill event is seen, the interface state is the only evidence of the
                // radio state. A daemon that publishes an interface is not soft blocked.
                if (!mRfkillKnown) {
                    mRadioPowered = state != "interface_disabled";
                    mState.setPowered(mRadioPowered);
                }
            }

            bool scanning {false};
            if (getBoolean(properties, "Scanning", scanning))
                mState.setScanning(scanning);

            std::string currentBss;
            if (getString(properties, "CurrentBSS", currentBss))
                mCurrentBssPath = currentBss;

            std::string currentNetwork;
            if (getString(properties, "CurrentNetwork", currentNetwork))
                mCurrentNetworkPath = currentNetwork;

            if (mAttempt.active) {
                std::int64_t reason {0};
                if (getSigned(properties, "DisconnectReason", reason) &&
                    isAuthenticationFailure(reason)) {
                    failAttempt(sAuthFailureMessage);
                }
                else if (hasState && state == "completed") {
                    attemptSucceeded();
                }
                else if (hasState && isInProgressState(previousState) &&
                         isDisconnectedState(state)) {
                    failAttempt("The connection attempt failed");
                }
            }

            // The radio just came back from a soft block: wpa_supplicant re-enabled the interface
            // and a scan is the only way to fill the list again.
            if (hasState && previousState == "interface_disabled" &&
                state != "interface_disabled" && mRadioPowered) {
                performStartScan();
            }

            syncActiveConnection();
            publish();
        }

        // Keeps the published active SSID and IP address in step with the interface state,
        // including a frontend that starts while wpa_supplicant is already connected.
        void syncActiveConnection()
        {
            if (mInterfaceState != "completed") {
                mActiveSsid.clear();
                mState.setActiveSsid(std::string {});
                mState.setIpAddress(std::string {});
                cancelIpTimer();
                mIpPollCounter = 0;
                mIpPollStarted = false;
                return;
            }

            // The Network or BSS object may arrive after the State property, so an unresolved
            // current object keeps the SSID that is already published.
            std::string ssid {mState.ssidForNetworkPath(mCurrentNetworkPath)};
            if (ssid.empty())
                ssid = mState.ssidForBssPath(mCurrentBssPath);

            if (!ssid.empty()) {
                mActiveSsid = ssid;
                mState.setActiveSsid(ssid);
            }

            if (!mIpPollStarted && !mActiveSsid.empty()) {
                mIpPollStarted = true;
                mIpPollCounter = 0;
                pollIpAddress();
            }
        }

        void applyBss(const std::string& path, const WifiProperties& properties)
        {
            WifiBss bss;
            const auto existing {mBsses.find(path)};
            if (existing != mBsses.end())
                bss = existing->second;
            bss.path = path;

            WifiMessageParser::applyBssProperties(properties, bss);
            mBsses[path] = bss;
            mState.setBss(path, bss);
        }

        void applyNetwork(const std::string& path, const WifiProperties& properties)
        {
            WifiSavedNetwork network;
            const auto existing {mNetworks.find(path)};
            if (existing != mNetworks.end())
                network = existing->second;
            network.path = path;

            WifiMessageParser::applyNetworkProperties(properties, network);
            mNetworks[path] = network;
            mState.setNetwork(path, network);
        }

        void handleInterfacesReply(sd_bus_message* message)
        {
            std::vector<std::string> paths;
            if (!readVariantObjectPaths(message, paths) || paths.empty()) {
                mState.setAvailable(false);
                publish();
                return;
            }

            for (const std::string& path : paths) {
                sd_bus_message* call {nullptr};
                if (sd_bus_message_new_method_call(mBus, &call, sService, path.c_str(),
                                                   sPropertiesInterface, "Get") < 0)
                    continue;

                sd_bus_message_append(call, "ss", sInterfaceInterface, "Ifname");
                send(call, Step::ifname, mEpoch, 0, path, std::string {});
            }
        }

        void handleIfnameReply(sd_bus_message* message, const FollowUp& followUp)
        {
            std::string ifname;
            if (!readVariantString(message, ifname) || ifname.empty()) {
                return;
            }

            // A P2P device is not the station interface that wpa_supplicant manages for us.
            if (startsWith(ifname, "p2p-"))
                return;

            if (!mInterfacePath.empty())
                return;

            selectInterface(followUp.path, ifname);
        }

        void handleInterfaceProperties(sd_bus_message* message)
        {
            // Properties.GetAll returns the dictionary directly, not wrapped in a variant.
            WifiProperties properties;
            if (!WifiMessageParser::readProperties(message, properties))
                return;

            applyInterfaceProperties(properties);

            std::vector<std::string> bssPaths;
            if (getStringList(properties, "BSSs", bssPaths)) {
                for (const std::string& path : bssPaths) {
                    if (mBsses.find(path) == mBsses.end())
                        requestObjectProperties(path, sBssInterface, Step::bssProperties);
                }
            }

            std::vector<std::string> networkPaths;
            if (getStringList(properties, "Networks", networkPaths)) {
                for (const std::string& path : networkPaths) {
                    if (mNetworks.find(path) == mNetworks.end())
                        requestObjectProperties(path, sNetworkInterface, Step::networkProperties);
                }
            }
        }

        void handleBssProperties(sd_bus_message* message, const FollowUp& followUp)
        {
            WifiProperties properties;
            if (!readVariantProperties(message, properties))
                return;

            applyBss(followUp.path, properties);
            syncActiveConnection();
            publish();
        }

        void handleNetworkProperties(sd_bus_message* message, const FollowUp& followUp)
        {
            WifiProperties properties;
            if (!readVariantProperties(message, properties))
                return;

            applyNetwork(followUp.path, properties);
            syncActiveConnection();
            publish();
        }

        void handleBssAdded(sd_bus_message* message)
        {
            std::string path;
            WifiProperties properties;
            if (!WifiMessageParser::readObjectAdded(message, path, properties))
                return;

            applyBss(path, properties);
            syncActiveConnection();
            publish();
        }

        void handleNetworkAdded(sd_bus_message* message)
        {
            std::string path;
            WifiProperties properties;
            if (!WifiMessageParser::readObjectAdded(message, path, properties))
                return;

            applyNetwork(path, properties);
            syncActiveConnection();
            publish();
        }

        void handleBssRemoved(sd_bus_message* message)
        {
            const char* path {nullptr};
            if (sd_bus_message_read(message, "o", &path) < 0)
                return;

            mBsses.erase(path);
            mState.removeBss(path);
            publish();
        }

        void handleNetworkRemoved(sd_bus_message* message)
        {
            const char* path {nullptr};
            if (sd_bus_message_read(message, "o", &path) < 0)
                return;

            mNetworks.erase(path);
            mState.removeNetwork(path);
            publish();
        }

        // A PropertiesChanged on an Interface, BSS or Network object. Both the standard signal and
        // wpa_supplicant's own form end up here; the standard one starts with the interface name,
        // which the caller has already consumed.
        void handleObjectPropertiesChanged(const std::string& path, const WifiProperties& changed)
        {
            if (!mInterfacePath.empty() && path == mInterfacePath) {
                applyInterfaceProperties(changed);
                return;
            }

            if (mBsses.find(path) != mBsses.end()) {
                applyBss(path, changed);
                publish();
                return;
            }

            if (mNetworks.find(path) != mNetworks.end()) {
                applyNetwork(path, changed);
                publish();
            }
        }

        void handleInterfaceRemoved(sd_bus_message* message)
        {
            const char* path {nullptr};
            if (sd_bus_message_read(message, "o", &path) < 0)
                return;

            if (mInterfacePath == path) {
                ++mEpoch;
                mInterfacePath.clear();
                mInterfaceName.clear();
                mInterfaceState.clear();
                mCurrentBssPath.clear();
                mCurrentNetworkPath.clear();
                mBsses.clear();
                mNetworks.clear();
                mState.setAvailable(false);
                publish();
            }

            requestInterfaces();
        }

        void handleNameOwnerChanged(sd_bus_message* message)
        {
            const char* name {nullptr};
            const char* oldOwner {nullptr};
            const char* newOwner {nullptr};
            if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) < 0)
                return;

            if (name == nullptr || std::strcmp(name, sService) != 0)
                return;

            ++mEpoch;
            mAttempt.active = false;
            ++mAttemptSerial;
            cancelConnectTimer();
            cancelIpTimer();

            if (newOwner != nullptr && newOwner[0] != '\0') {
                requestInterfaces();
                return;
            }

            // The daemon left the bus, so every object it knew is gone.
            mInterfacePath.clear();
            mInterfaceName.clear();
            mInterfaceState.clear();
            mCurrentBssPath.clear();
            mCurrentNetworkPath.clear();
            mBsses.clear();
            mNetworks.clear();
            mConnectRemovalQueue.clear();
            mForgetQueue.clear();
            mState.clear();
            publish();
        }

        //------------------------------------------------------------------------------------------
        // Worker loop.

        void drainWake()
        {
            std::uint64_t counter {0};
            if (read(mWakeFd, &counter, sizeof(counter)) < 0 && errno != EAGAIN) {
                LOG(LogWarning) << "Wi-Fi: could not drain the wake file descriptor";
            }
        }

        void processCommands()
        {
            std::vector<Command> commands;
            {
                std::lock_guard<std::mutex> lock {mCommandMutex};
                commands.swap(mCommands);
            }

            for (Command& command : commands) {
                switch (command.type) {
                    case Command::Type::stop:
                        mAttempt.active = false;
                        ++mAttemptSerial;
                        cancelConnectTimer();
                        cancelIpTimer();
                        mState.clearPending(mForgetSsid);
                        sd_event_exit(mEvent, 0);
                        return;

                    case Command::Type::refresh:
                        requestInterfaces();
                        break;

                    case Command::Type::setPowered:
                        if (ensureBus())
                            performSetPowered(command.state);
                        break;

                    case Command::Type::startScan:
                        if (ensureBus())
                            performStartScan();
                        break;

                    case Command::Type::connect:
                        if (ensureBus())
                            performConnect(command.ssid);
                        break;

                    case Command::Type::connectWithPassword:
                        if (ensureBus())
                            performConnectWithPassword(command.ssid, command.passphrase);
                        std::fill(command.passphrase.begin(), command.passphrase.end(), '\0');
                        break;

                    case Command::Type::disconnect:
                        if (ensureBus())
                            performDisconnect();
                        break;

                    case Command::Type::forget:
                        if (ensureBus())
                            performForget(command.ssid);
                        break;
                }
            }
        }

        bool ensureBus()
        {
            if (mBus != nullptr && !mInterfacePath.empty())
                return true;

            setError(mBus == nullptr ? "The Wi-Fi service is not available" :
                                       "There is no Wi-Fi interface");
            return false;
        }

        bool hasInterface() const { return mBus != nullptr && !mInterfacePath.empty(); }

        const WifiNetwork* findNetwork(const std::string& ssid)
        {
            const WifiSnapshot snapshot {mState.toSnapshot(0)};
            const auto network {std::find_if(
                snapshot.networks.begin(), snapshot.networks.end(),
                [&ssid](const WifiNetwork& candidate) { return candidate.ssid == ssid; })};
            if (network == snapshot.networks.end())
                return nullptr;

            // The snapshot is a temporary, so the caller only uses the copy below.
            mLookupNetwork = *network;
            return &mLookupNetwork;
        }

        //------------------------------------------------------------------------------------------
        // Async plumbing.

        FollowUp* trackFollowUp(const Step step,
                                const std::uint64_t epoch,
                                const std::uint64_t serial,
                                const std::string& path,
                                const std::string& ssid)
        {
            auto* followUp {new FollowUp {step, epoch, serial, path, ssid, this}};
            mFollowUps.push_back(followUp);
            return followUp;
        }

        void freeFollowUp(FollowUp* followUp)
        {
            const auto found {std::find(mFollowUps.begin(), mFollowUps.end(), followUp)};
            if (found != mFollowUps.end())
                mFollowUps.erase(found);

            delete followUp;
        }

        void send(sd_bus_message* message,
                  const Step step,
                  const std::uint64_t epoch,
                  const std::uint64_t serial,
                  const std::string& path,
                  const std::string& ssid)
        {
            if (mBus == nullptr) {
                sd_bus_message_unref(message);
                return;
            }

            auto* followUp {trackFollowUp(step, epoch, serial, path, ssid)};
            const int result {sd_bus_call_async(mBus, nullptr, message, onReply, followUp, 0)};
            sd_bus_message_unref(message);

            if (result < 0) {
                freeFollowUp(followUp);
                if (mAttempt.active && serial != 0 && serial == mAttemptSerial)
                    failAttempt("The Wi-Fi service is not available");
                else
                    setError("The Wi-Fi service is not available");
            }
        }

        void callRemoveNetworkQuiet(const std::string& path)
        {
            if (mBus == nullptr || mInterfacePath.empty())
                return;

            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus, &message, sService, mInterfacePath.c_str(),
                                               sInterfaceInterface, "RemoveNetwork") < 0)
                return;

            sd_bus_message_append(message, "o", path.c_str());
            send(message, Step::quiet, mEpoch, 0, path, std::string {});
        }

        void callMethodQuiet(const std::string& path, const char* interfaceName, const char* member)
        {
            sd_bus_message* message {nullptr};
            if (sd_bus_message_new_method_call(mBus, &message, sService, path.c_str(),
                                               interfaceName, member) < 0)
                return;

            send(message, Step::quiet, mEpoch, 0, path, std::string {});
        }

        bool isStale(const FollowUp& followUp) const
        {
            if (followUp.epoch != 0 && followUp.epoch != mEpoch)
                return true;

            if (followUp.serial != 0 && (!mAttempt.active || followUp.serial != mAttemptSerial))
                return true;

            return false;
        }

        void handleReply(FollowUp& followUp,
                         sd_bus_message* message,
                         const bool failed,
                         const std::string& error)
        {
            if (isStale(followUp))
                return;

            switch (followUp.step) {
                case Step::interfaces:
                    if (failed)
                        setError(error);
                    else
                        handleInterfacesReply(message);
                    break;

                case Step::ifname:
                    if (!failed)
                        handleIfnameReply(message, followUp);
                    break;

                case Step::interfaceProperties:
                    if (!failed)
                        handleInterfaceProperties(message);
                    break;

                case Step::bssProperties:
                    if (!failed)
                        handleBssProperties(message, followUp);
                    break;

                case Step::networkProperties:
                    if (!failed)
                        handleNetworkProperties(message, followUp);
                    break;

                case Step::scan:
                    if (failed)
                        setError(error);
                    break;

                case Step::addNetwork:
                    if (failed) {
                        failAttempt(error);
                        break;
                    }
                    {
                        const char* path {nullptr};
                        if (sd_bus_message_read(message, "o", &path) < 0 || path == nullptr) {
                            failAttempt("The Wi-Fi service did not create the network");
                            break;
                        }
                        mAttempt.newNetworkPath = path;
                        selectNetwork(path);
                    }
                    break;

                case Step::selectNetwork:
                    if (failed)
                        failAttempt(error);
                    else
                        startConnectTimer();
                    break;

                case Step::removeNetworkForConnect:
                    // A stale block that could not be removed is not fatal, the new one is added
                    // under a fresh object path anyway.
                    removeNextForConnect();
                    break;

                case Step::removeNetworkForForget:
                    if (failed)
                        mForgetError = error;
                    removeNextForForget();
                    break;

                case Step::saveConfigForForget:
                    if (failed)
                        mForgetError = error;
                    finishForget();
                    break;

                case Step::enableNetwork:
                    if (failed) {
                        LOG(LogWarning) << "Wi-Fi: could not re-enable a saved network: " << error;
                    }
                    break;

                case Step::saveConfig:
                    if (failed) {
                        LOG(LogWarning)
                            << "Wi-Fi: could not save the Wi-Fi configuration: " << error;
                    }
                    break;

                case Step::reconnect:
                    if (failed) {
                        LOG(LogWarning) << "Wi-Fi: could not reconnect: " << error;
                    }
                    break;

                case Step::quiet:
                    if (failed) {
                        LOG(LogWarning) << "Wi-Fi: a cleanup call failed: " << error;
                    }
                    break;

                case Step::disconnect:
                    if (failed) {
                        setError(error);
                        break;
                    }
                    for (const auto& network : mNetworks)
                        mState.clearPending(network.second.ssid);
                    mState.setLastError(std::string {});
                    publish();
                    break;
            }
        }

        void handleRfkillReadable()
        {
            bool changed {false};

            while (true) {
                struct rfkill_event event {};
                const ssize_t size {read(mRfkillFd, &event, sizeof(event))};

                if (size == 0) {
                    // A pipe that was closed. There is nothing left to watch, and keeping the
                    // source would spin on the always-readable EOF.
                    LOG(LogWarning) << "Wi-Fi: the rfkill file reached its end";
                    if (mRfkillSource != nullptr) {
                        sd_event_source_disable_unref(mRfkillSource);
                        mRfkillSource = nullptr;
                    }
                    close(mRfkillFd);
                    mRfkillFd = -1;
                    break;
                }

                if (size < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        LOG(LogWarning)
                            << "Wi-Fi: could not read the rfkill event: " << std::strerror(errno);
                    }
                    break;
                }

                if (size != static_cast<ssize_t>(sizeof(event)) || event.type != RFKILL_TYPE_WLAN)
                    continue;

                mRfkillKnown = true;
                mRadioPowered = event.soft == 0;
                mState.setPowered(mRadioPowered);
                mState.setHardBlocked(event.hard != 0);
                changed = true;
            }

            if (changed)
                publish();
        }

        void setError(const std::string& error)
        {
            mState.setLastError(error);
            publish();
        }

        void publish()
        {
            const std::uint64_t sequence {++mSequence};
            std::shared_ptr<const WifiSnapshot> snapshot {
                std::make_shared<WifiSnapshot>(mState.toSnapshot(sequence))};

            std::lock_guard<std::mutex> lock {mSnapshotMutex};
            mSnapshot = std::move(snapshot);
        }

        static int onWake(sd_event_source*, int, uint32_t, void* userdata)
        {
            auto* manager {static_cast<WpaSupplicantWifiManager*>(userdata)};
            manager->drainWake();
            manager->processCommands();
            return 0;
        }

        static int onRfkill(sd_event_source*, int, uint32_t, void* userdata)
        {
            auto* manager {static_cast<WpaSupplicantWifiManager*>(userdata)};
            manager->handleRfkillReadable();
            return 0;
        }

        static int onConnectTimeout(sd_event_source* source, uint64_t, void* userdata)
        {
            auto* manager {static_cast<WpaSupplicantWifiManager*>(userdata)};
            sd_event_source_disable_unref(source);
            manager->mConnectTimerSource = nullptr;
            manager->failAttempt(sConnectTimeoutMessage);
            return 0;
        }

        static int onIpPoll(sd_event_source* source, uint64_t, void* userdata)
        {
            auto* manager {static_cast<WpaSupplicantWifiManager*>(userdata)};
            sd_event_source_disable_unref(source);
            manager->mIpTimerSource = nullptr;
            manager->pollIpAddress();
            return 0;
        }

        static int onSignal(sd_bus_message* message, void* userdata, sd_bus_error*)
        {
            auto* manager {static_cast<WpaSupplicantWifiManager*>(userdata)};

            const char* member {sd_bus_message_get_member(message)};
            const char* path {sd_bus_message_get_path(message)};
            const char* interfaceName {sd_bus_message_get_interface(message)};

            if (member == nullptr || path == nullptr || interfaceName == nullptr)
                return 0;

            const std::string name {member};
            const std::string iface {interfaceName};

            if (name == "NameOwnerChanged") {
                manager->handleNameOwnerChanged(message);
                return 0;
            }

            if (name == "InterfaceAdded") {
                manager->requestInterfaces();
                return 0;
            }

            if (name == "InterfaceRemoved") {
                manager->handleInterfaceRemoved(message);
                return 0;
            }

            if (name == "ScanDone") {
                manager->mState.setScanning(false);
                manager->publish();
                return 0;
            }

            if (name == "BSSAdded") {
                manager->handleBssAdded(message);
                return 0;
            }

            if (name == "BSSRemoved") {
                manager->handleBssRemoved(message);
                return 0;
            }

            if (name == "NetworkAdded") {
                manager->handleNetworkAdded(message);
                return 0;
            }

            if (name == "NetworkRemoved") {
                manager->handleNetworkRemoved(message);
                return 0;
            }

            if (name != "PropertiesChanged")
                return 0;

            WifiProperties changed;
            if (iface == sPropertiesInterface) {
                const char* objectInterface {nullptr};
                if (sd_bus_message_read(message, "s", &objectInterface) < 0)
                    return 0;
                std::vector<std::string> invalidated;
                if (!WifiMessageParser::readPropertiesChanged(message, changed, invalidated))
                    return 0;
                manager->handleObjectPropertiesChanged(path, changed);
                return 0;
            }

            std::vector<std::string> invalidated;
            if (!WifiMessageParser::readPropertiesChanged(message, changed, invalidated))
                return 0;
            manager->handleObjectPropertiesChanged(path, changed);
            return 0;
        }

        static int onReply(sd_bus_message* message, void* userdata, sd_bus_error*)
        {
            auto* followUp {static_cast<FollowUp*>(userdata)};
            WpaSupplicantWifiManager* manager {followUp->manager};

            const bool failed {sd_bus_message_is_method_error(message, nullptr) > 0};
            std::string error;

            if (failed) {
                const sd_bus_error* busError {sd_bus_message_get_error(message)};
                error = WifiState::describeError(
                    busError != nullptr && busError->name != nullptr ? busError->name :
                                                                       std::string {},
                    busError != nullptr && busError->message != nullptr ? busError->message :
                                                                          std::string {});
            }

            manager->handleReply(*followUp, message, failed, error);
            manager->freeFollowUp(followUp);
            return 0;
        }

        std::thread mThread;
        sd_event* mEvent {nullptr};
        sd_bus* mBus {nullptr};
        int mWakeFd {-1};
        int mRfkillFd {-1};
        sd_event_source* mRfkillSource {nullptr};

        std::atomic<std::uint64_t> mSequence {0};
        mutable std::mutex mSnapshotMutex;
        std::shared_ptr<const WifiSnapshot> mSnapshot;

        std::mutex mCommandMutex;
        std::vector<Command> mCommands;

        // Loop thread only.
        std::string mInterfacePath;
        std::string mInterfaceName;
        std::string mInterfaceState;
        std::string mCurrentBssPath;
        std::string mCurrentNetworkPath;
        std::map<std::string, WifiBss> mBsses;
        std::map<std::string, WifiSavedNetwork> mNetworks;
        WifiState mState;

        // Incremented when the interface or the daemon instance changes. Replies sent in a
        // previous epoch are dropped.
        std::uint64_t mEpoch {0};

        struct Attempt {
            bool active {false};
            std::uint64_t serial {0};
            std::string ssid;
            std::string passphrase;
            std::string newNetworkPath;
            bool createdNetwork {false};
        };

        Attempt mAttempt;
        std::uint64_t mAttemptSerial {0};
        std::vector<std::string> mConnectRemovalQueue;

        std::string mForgetSsid;
        std::string mForgetError;
        std::vector<std::string> mForgetQueue;

        std::string mActiveSsid;
        bool mRfkillKnown {false};
        bool mRadioPowered {true};
        bool mIpPollStarted {false};

        sd_event_source* mConnectTimerSource {nullptr};
        sd_event_source* mIpTimerSource {nullptr};
        int mIpPollCounter {0};

        WifiNetwork mLookupNetwork;

        std::vector<FollowUp*> mFollowUps;
    };

} // namespace

std::unique_ptr<WifiManager> createWpaSupplicantWifiManager()
{
    return std::make_unique<WpaSupplicantWifiManager>();
}

#endif // WIFI_MANAGER_WPASUPPLICANT
