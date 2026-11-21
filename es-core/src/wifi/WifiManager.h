//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  WifiManager.h
//
//  Wi-Fi network management interface.
//
//  The implementation is selected at build time and hides the platform details behind an
//  asynchronous action API: actions return immediately and the resulting state is published as an
//  immutable snapshot that the UI reads once per frame.
//

#ifndef ES_CORE_WIFI_MANAGER_H
#define ES_CORE_WIFI_MANAGER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The operation that is currently in flight for a network. The UI uses this to disable the
// conflicting actions and to show that something is happening.
enum class WifiOp {
    none,
    connecting,
    disconnecting,
    forgetting
};

// What a network asks of a client before it lets it join. Derived from the key management
// suites the access point advertises.
enum class WifiSecurity {
    open,
    personal,
    enterprise
};

// The key management to use when a network is added to wpa_supplicant.
enum class WifiKeyMgmt {
    none,
    wpaPsk,
    sae,
    wpaEap
};

struct WifiNetwork {
    // The SSID, which is the identity of the network. One entry is published per SSID even when
    // several access points advertise it.
    std::string ssid;
    WifiSecurity security {WifiSecurity::open};
    WifiKeyMgmt preferredKeyMgmt {WifiKeyMgmt::none};
    // True when an access point with this SSID is in range.
    bool visible {false};
    // True when wpa_supplicant has a Network object for this SSID, which is what makes it come
    // back after a reboot.
    bool saved {false};
    bool connected {false};
    // The strongest access point of this network in dBm, -100 when out of range.
    int rssi {-100};
    // 0..100 percentage derived from rssi, 0 when out of range.
    int signal {0};
    WifiOp pending {WifiOp::none};
};

struct WifiSnapshot {
    // True when a usable wireless interface was found. Everything else in the snapshot is
    // meaningless when this is false.
    bool available {false};
    bool powered {false};
    // True when the radio is blocked by a hardware or platform kill switch.
    bool hardBlocked {false};
    std::string interfacePath;
    std::string interfaceName;
    // The wpa_supplicant Interface.State property, for example "completed" or "disconnected".
    std::string state;
    bool scanning {false};
    bool connected {false};
    std::string activeSsid;
    // The IPv4 address of the interface, empty until a lease was acquired.
    std::string ipAddress;
    std::vector<WifiNetwork> networks;
    // The last failure, in a form that can be shown to the user. Empty when there is none.
    std::string lastError;
    // Increases every time a snapshot is published.
    std::uint64_t sequence {0};
};

class WifiManager
{
public:
    virtual ~WifiManager() = default;

    static WifiManager& getInstance();

    WifiManager(const WifiManager&) = delete;
    WifiManager& operator=(const WifiManager&) = delete;

    // True when the build has a real backend, which is a compile time property and not something
    // that changes: the null implementation never has one. A UI uses this to decide whether to
    // offer a feature at all, while isAvailable() reports whether the service can be reached now.
    static constexpr bool hasBackend()
    {
#if defined(WIFI_MANAGER_WPASUPPLICANT)
        return true;
#else
        return false;
#endif
    }

    // False in builds without a backend and, with a backend, while no wireless interface is
    // present.
    virtual bool isAvailable() const = 0;

    // The current state. The pointer stays valid when a newer snapshot is published, so the UI
    // can hold on to it while it draws a frame.
    virtual std::shared_ptr<const WifiSnapshot> getSnapshot() const = 0;

    // The sequence number of the current snapshot, a cheap way to detect that it changed.
    virtual std::uint64_t getSequence() const = 0;

    // The actions below return immediately. Their progress and outcome are reported through the
    // snapshot: WifiNetwork::pending on the network that is being worked on, and
    // WifiSnapshot::lastError for failures.
    virtual void setPowered(const bool powered) = 0;
    virtual void startScan() = 0;
    virtual void connect(const std::string& ssid) = 0;
    virtual void connectWithPassword(const std::string& ssid, const std::string& passphrase) = 0;
    virtual void disconnect() = 0;
    virtual void forget(const std::string& ssid) = 0;

protected:
    WifiManager() = default;
};

#endif // ES_CORE_WIFI_MANAGER_H
