//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  WifiState.h
//
//  Platform independent reduction of the wpa_supplicant D-Bus objects into a WifiSnapshot.
//
//  The wpa_supplicant backend feeds the BSS and Network objects it reads from D-Bus into
//  WifiState and publishes the snapshot it produces. Keeping the reduction here instead of in the
//  sd-bus code makes the SSID handling, the deduplication, the classification, the ordering and
//  the passphrase validation testable without a bus.
//

#ifndef ES_CORE_WIFI_STATE_H
#define ES_CORE_WIFI_STATE_H

#include "wifi/WifiManager.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// One fi.w1.wpa_supplicant1.BSS object, as the parser reduced it from D-Bus.
struct WifiBss {
    std::string path;
    std::string ssid;
    std::string bssid;
    // Signal strength in dBm, -100 when the access point does not report one.
    int signal {-100};
    std::uint32_t frequency {0};
    // The KeyMgmt suites of the WPA and RSN information elements, as advertised by the access
    // point. Empty for an open network that only reports Privacy.
    std::vector<std::string> keyMgmt;
};

// One fi.w1.wpa_supplicant1.Network object, which is what wpa_supplicant remembers about a
// network between connections.
struct WifiSavedNetwork {
    std::string path;
    std::string ssid;
    // The configured key management, for example "WPA-PSK" or "NONE". Used to classify a saved
    // network that is currently out of range, where no BSS is available.
    std::string keyMgmt;
    bool enabled {true};
};

class WifiState
{
public:
    // Replaces or adds one BSS object, by its D-Bus object path.
    void setBss(const std::string& path, const WifiBss& bss);
    void removeBss(const std::string& path);

    // Replaces or adds one Network object, by its D-Bus object path.
    void setNetwork(const std::string& path, const WifiSavedNetwork& network);
    void removeNetwork(const std::string& path);

    // The interface objects. available is true once a non-P2P wireless interface was selected.
    void setAvailable(const bool available);
    void setInterfacePath(const std::string& path);
    void setInterfaceName(const std::string& name);
    void setInterfaceState(const std::string& state);
    void setPowered(const bool powered);
    void setHardBlocked(const bool hardBlocked);
    void setScanning(const bool scanning);
    void setActiveSsid(const std::string& ssid);
    void setIpAddress(const std::string& address);

    // The operation that is in flight for a network and the last failure, both are merged into
    // the snapshot. The pending operation is keyed by SSID.
    void setPending(const std::string& ssid, const WifiOp op);
    void clearPending(const std::string& ssid);
    void setLastError(const std::string& error);

    // The object paths of the saved Network objects with this SSID. wpa_supplicant can hold more
    // than one after a failed connection attempt, which is what the connect path cleans up.
    std::vector<std::string> savedNetworkPaths(const std::string& ssid) const;

    // The object paths of every saved Network object.
    std::vector<std::string> allNetworkPaths() const;

    // The SSID behind a BSS or Network object path, empty when the path is unknown.
    std::string ssidForBssPath(const std::string& path) const;
    std::string ssidForNetworkPath(const std::string& path) const;

    // Forgets every object, used when wpa_supplicant leaves the bus.
    void clear();

    // Builds the snapshot. The deduplication, the classification and the ordering are applied
    // here, so that the published snapshot is already what the UI shows.
    WifiSnapshot toSnapshot(const std::uint64_t sequence) const;

    // Decodes the wpa_supplicant string form of an SSID: either a quoted ASCII string with
    // backslash escapes, or the hexadecimal form of the raw bytes. A value that is neither is
    // returned unchanged.
    static std::string decodeSsid(const std::string& encoded);

    // Encodes an SSID for D-Bus: a quoted ASCII string when every character is printable ASCII
    // and neither a quote nor a backslash, otherwise the lowercase hexadecimal form of the raw
    // bytes. An empty SSID stays empty.
    static std::string encodeSsid(const std::string& ssid);

    // The security of a network, from the key management suites its access point advertises.
    static WifiSecurity classifySecurity(const std::vector<std::string>& keyMgmt);

    // The key management to use when connecting. SAE is only chosen when the access point does
    // not also advertise WPA-PSK, as the transition mode of Broadcom brcmfmac cards stalls.
    static WifiKeyMgmt preferredKeyMgmt(const std::vector<std::string>& keyMgmt);

    // 0..100 percentage for an RSSI in dBm. Values at or above 0 mean that no signal was
    // reported, which is treated as 0.
    static int signalPercentage(const int rssi);

    // True for 8..63 printable ASCII characters (0x20..0x7e), or exactly 64 hexadecimal digits.
    static bool validatePassphrase(const std::string& passphrase);

    // The message for a rejected passphrase. It never contains the attempted value.
    static std::string passphraseError();

    // Turns an error name and message, normally a fi.w1.wpa_supplicant1.* name, into a message for
    // the user. A known name always maps to a fixed description that does not repeat the D-Bus
    // message, so a diagnostic string can never echo a credential.
    static std::string describeError(const std::string& errorName, const std::string& message);

private:
    std::map<std::string, WifiBss> mBss;
    std::map<std::string, WifiSavedNetwork> mNetworks;
    std::map<std::string, WifiOp> mPending;
    bool mAvailable {false};
    bool mPowered {false};
    bool mHardBlocked {false};
    bool mScanning {false};
    std::string mInterfacePath;
    std::string mInterfaceName;
    std::string mInterfaceState;
    std::string mActiveSsid;
    std::string mIpAddress;
    std::string mLastError;
    std::uint64_t mErrorSequence {0};
};

#endif // ES_CORE_WIFI_STATE_H
