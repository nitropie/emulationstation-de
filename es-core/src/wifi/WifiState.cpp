//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  WifiState.cpp
//
//  Platform independent reduction of the wpa_supplicant D-Bus objects into a WifiSnapshot.
//

#include "wifi/WifiState.h"

#include <algorithm>
#include <cctype>

namespace
{
    bool isHexDigit(const char character)
    {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F');
    }

    int hexValue(const char character)
    {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;

        return character - 'A' + 10;
    }

    bool isHexadecimal(const std::string& text)
    {
        if (text.empty() || text.size() % 2 != 0)
            return false;

        return std::all_of(text.begin(), text.end(), isHexDigit);
    }

    // wpa_supplicant spells the suites in lower case with hyphens on the D-Bus wire, while a
    // saved network keeps the upper case configuration form. Comparing a normalized form accepts
    // both spellings without listing every suite twice.
    std::string normalizeKeyMgmt(const std::string& text)
    {
        std::string normalized;
        normalized.reserve(text.size());
        for (const char character : text) {
            const auto byte {static_cast<unsigned char>(character)};
            normalized.push_back(character == '-' ? '_' : static_cast<char>(std::toupper(byte)));
        }
        return normalized;
    }

    bool hasKeyMgmt(const std::vector<std::string>& keyMgmt, const char* suite)
    {
        const std::string expected {normalizeKeyMgmt(suite)};
        return std::any_of(keyMgmt.begin(), keyMgmt.end(),
                           [&expected](const std::string& candidate) {
                               return normalizeKeyMgmt(candidate) == expected;
                           });
    }

    // An SSID is hidden when the access point did not publish one. wpa_supplicant reports that as
    // empty bytes, or as a string of NUL bytes for an all-zero information element.
    bool isHiddenSsid(const std::string& ssid)
    {
        return std::all_of(ssid.begin(), ssid.end(),
                           [](const char character) { return character == '\0'; });
    }

    // Connected networks come first, then the saved networks that are in range, then the
    // networks a scan turned up, then the saved networks that are out of range. A saved network
    // must not jump around while it is rescanned.
    int networkGroup(const WifiNetwork& network)
    {
        if (network.connected)
            return 0;
        if (network.saved && network.visible)
            return 1;
        if (network.visible)
            return 2;

        return 3;
    }

    bool sortNetworks(const WifiNetwork& left, const WifiNetwork& right)
    {
        const int leftGroup {networkGroup(left)};
        const int rightGroup {networkGroup(right)};

        if (leftGroup != rightGroup)
            return leftGroup < rightGroup;

        // Only the visible networks move with the signal. An out-of-range saved network has no
        // signal at all, so that group is ordered by name alone.
        if ((leftGroup == 1 || leftGroup == 2) && left.signal != right.signal)
            return left.signal > right.signal;

        if (left.ssid != right.ssid)
            return left.ssid < right.ssid;

        return false;
    }

} // namespace

void WifiState::setBss(const std::string& path, const WifiBss& bss) { mBss[path] = bss; }

void WifiState::removeBss(const std::string& path) { mBss.erase(path); }

void WifiState::setNetwork(const std::string& path, const WifiSavedNetwork& network)
{
    mNetworks[path] = network;
}

void WifiState::removeNetwork(const std::string& path) { mNetworks.erase(path); }

void WifiState::setAvailable(const bool available) { mAvailable = available; }

void WifiState::setInterfacePath(const std::string& path) { mInterfacePath = path; }

void WifiState::setInterfaceName(const std::string& name) { mInterfaceName = name; }

void WifiState::setInterfaceState(const std::string& state) { mInterfaceState = state; }

void WifiState::setPowered(const bool powered) { mPowered = powered; }

void WifiState::setHardBlocked(const bool hardBlocked) { mHardBlocked = hardBlocked; }

void WifiState::setScanning(const bool scanning) { mScanning = scanning; }

void WifiState::setActiveSsid(const std::string& ssid) { mActiveSsid = ssid; }

void WifiState::setIpAddress(const std::string& address) { mIpAddress = address; }

void WifiState::setPending(const std::string& ssid, const WifiOp op)
{
    if (op == WifiOp::none)
        mPending.erase(ssid);
    else
        mPending[ssid] = op;
}

void WifiState::clearPending(const std::string& ssid) { mPending.erase(ssid); }

void WifiState::setLastError(const std::string& error,
                             const std::string& failedSsid,
                             const bool needsPasswordRetry)
{
    mLastError = error;

    // The sequence only moves when a failure is published, so a screen can tell a new identical
    // failure from the one it has already shown.
    if (!error.empty()) {
        mFailedSsid = failedSsid;
        mLastErrorNeedsPasswordRetry = needsPasswordRetry && !failedSsid.empty();
        ++mErrorSequence;
    }
    else {
        mFailedSsid.clear();
        mLastErrorNeedsPasswordRetry = false;
    }
}

void WifiState::clear()
{
    mBss.clear();
    mNetworks.clear();
    mPending.clear();
    mAvailable = false;
    mPowered = false;
    mHardBlocked = false;
    mScanning = false;
    mInterfacePath.clear();
    mInterfaceName.clear();
    mInterfaceState.clear();
    mActiveSsid.clear();
    mIpAddress.clear();
    mLastError.clear();
    mFailedSsid.clear();
    mLastErrorNeedsPasswordRetry = false;
    mErrorSequence = 0;
}

std::vector<std::string> WifiState::savedNetworkPaths(const std::string& ssid) const
{
    std::vector<std::string> paths;
    for (const auto& network : mNetworks) {
        if (network.second.ssid == ssid)
            paths.push_back(network.first);
    }

    return paths;
}

std::vector<std::string> WifiState::allNetworkPaths() const
{
    std::vector<std::string> paths;
    for (const auto& network : mNetworks)
        paths.push_back(network.first);

    return paths;
}

std::string WifiState::ssidForBssPath(const std::string& path) const
{
    const auto bss {mBss.find(path)};
    return bss == mBss.end() ? std::string {} : bss->second.ssid;
}

std::string WifiState::ssidForNetworkPath(const std::string& path) const
{
    const auto network {mNetworks.find(path)};
    return network == mNetworks.end() ? std::string {} : network->second.ssid;
}

WifiSnapshot WifiState::toSnapshot(const std::uint64_t sequence) const
{
    WifiSnapshot snapshot;
    snapshot.sequence = sequence;
    snapshot.available = mAvailable;
    snapshot.powered = mPowered;
    snapshot.hardBlocked = mHardBlocked;
    snapshot.interfacePath = mInterfacePath;
    snapshot.interfaceName = mInterfaceName;
    snapshot.state = mInterfaceState;
    snapshot.scanning = mScanning;
    snapshot.connected = mInterfaceState == "completed";
    snapshot.activeSsid = mActiveSsid;
    snapshot.ipAddress = mIpAddress;
    snapshot.lastError = mLastError;
    snapshot.failedSsid = mFailedSsid;
    snapshot.lastErrorNeedsPasswordRetry = mLastErrorNeedsPasswordRetry;
    snapshot.errorSequence = mErrorSequence;

    // wpa_supplicant can hold several Network objects for one SSID after a failed connection
    // attempt. One entry per SSID is published, and the first object is the representative.
    std::map<std::string, const WifiSavedNetwork*> savedNetworks;
    for (const auto& network : mNetworks) {
        if (network.second.ssid.empty())
            continue;
        savedNetworks.emplace(network.second.ssid, &network.second);
    }

    // Every access point with the same SSID is reduced into one network, keeping the strongest
    // signal. A network whose SSID was not published is hidden and only shown when it is saved.
    std::map<std::string, const WifiBss*> strongestBss;
    for (const auto& bss : mBss) {
        if (isHiddenSsid(bss.second.ssid))
            continue;

        const auto existing {strongestBss.find(bss.second.ssid)};
        if (existing == strongestBss.end() || bss.second.signal > existing->second->signal)
            strongestBss[bss.second.ssid] = &bss.second;
    }

    for (const auto& visible : strongestBss) {
        WifiNetwork entry;
        entry.ssid = visible.first;
        entry.visible = true;
        entry.rssi = visible.second->signal;
        entry.signal = signalPercentage(visible.second->signal);
        entry.security = classifySecurity(visible.second->keyMgmt);
        entry.preferredKeyMgmt = preferredKeyMgmt(visible.second->keyMgmt);
        entry.saved = savedNetworks.find(visible.first) != savedNetworks.end();
        entry.connected =
            snapshot.connected && !mActiveSsid.empty() && visible.first == mActiveSsid;

        const auto pending {mPending.find(visible.first)};
        entry.pending = pending == mPending.end() ? WifiOp::none : pending->second;

        snapshot.networks.push_back(std::move(entry));
    }

    for (const auto& saved : savedNetworks) {
        if (strongestBss.find(saved.first) != strongestBss.end())
            continue;

        WifiNetwork entry;
        entry.ssid = saved.first;
        entry.visible = false;
        entry.rssi = -100;
        entry.signal = 0;
        entry.saved = true;
        entry.connected = snapshot.connected && !mActiveSsid.empty() && saved.first == mActiveSsid;

        if (!saved.second->keyMgmt.empty()) {
            entry.security = classifySecurity({saved.second->keyMgmt});
            entry.preferredKeyMgmt = preferredKeyMgmt({saved.second->keyMgmt});
        }

        const auto pending {mPending.find(saved.first)};
        entry.pending = pending == mPending.end() ? WifiOp::none : pending->second;

        snapshot.networks.push_back(std::move(entry));
    }

    std::sort(snapshot.networks.begin(), snapshot.networks.end(), sortNetworks);

    return snapshot;
}

std::string WifiState::decodeSsid(const std::string& encoded)
{
    if (encoded.empty())
        return std::string {};

    if (encoded.size() >= 2 && encoded.front() == '"' && encoded.back() == '"') {
        std::string decoded;
        decoded.reserve(encoded.size() - 2);

        const std::size_t closingQuote {encoded.size() - 1};
        for (std::size_t index {1}; index < closingQuote; ++index) {
            const char character {encoded[index]};
            if (character != '\\') {
                decoded.push_back(character);
                continue;
            }

            if (++index >= closingQuote)
                break;

            const char escaped {encoded[index]};

            // wpa_supplicant writes the readable control characters as escapes.
            if (escaped == 'n') {
                decoded.push_back('\n');
                continue;
            }
            if (escaped == 'r') {
                decoded.push_back('\r');
                continue;
            }
            if (escaped == 't') {
                decoded.push_back('\t');
                continue;
            }
            if (escaped == 'e') {
                decoded.push_back('\x1b');
                continue;
            }

            // wpa_supplicant writes a byte that has no readable form as \xNN.
            if (escaped == 'x' && index + 2 < closingQuote && isHexDigit(encoded[index + 1]) &&
                isHexDigit(encoded[index + 2])) {
                decoded.push_back(static_cast<char>(hexValue(encoded[index + 1]) * 16 +
                                                    hexValue(encoded[index + 2])));
                index += 2;
                continue;
            }

            // Any other escaped character stands for itself.
            decoded.push_back(escaped);
        }

        return decoded;
    }

    if (isHexadecimal(encoded)) {
        std::string decoded;
        decoded.reserve(encoded.size() / 2);
        for (std::size_t index {0}; index + 1 < encoded.size(); index += 2) {
            decoded.push_back(
                static_cast<char>(hexValue(encoded[index]) * 16 + hexValue(encoded[index + 1])));
        }
        return decoded;
    }

    return encoded;
}

std::string WifiState::encodeSsid(const std::string& ssid)
{
    if (ssid.empty())
        return std::string {};

    const bool quotable {std::all_of(ssid.begin(), ssid.end(), [](const char character) {
        const auto byte {static_cast<unsigned char>(character)};
        return byte >= 0x20 && byte <= 0x7e && byte != '"' && byte != '\\';
    })};

    if (quotable)
        return "\"" + ssid + "\"";

    static constexpr char digits[] {"0123456789abcdef"};

    std::string encoded;
    encoded.reserve(ssid.size() * 2);
    for (const char character : ssid) {
        const auto byte {static_cast<unsigned char>(character)};
        encoded.push_back(digits[byte >> 4]);
        encoded.push_back(digits[byte & 0x0f]);
    }

    return encoded;
}

WifiSecurity WifiState::classifySecurity(const std::vector<std::string>& keyMgmt)
{
    // The suite names are the ones that wpa_supplicant's dbus_new_handlers.c emits, plus the
    // upper case configuration form that a saved network stores.
    if (hasKeyMgmt(keyMgmt, "WPA-EAP") || hasKeyMgmt(keyMgmt, "WPA-EAP-SHA256") ||
        hasKeyMgmt(keyMgmt, "WPA-EAP-SHA384") || hasKeyMgmt(keyMgmt, "WPA-EAP-SUITE-B") ||
        hasKeyMgmt(keyMgmt, "WPA-EAP-SUITE-B-192") || hasKeyMgmt(keyMgmt, "WPA-FT-EAP") ||
        hasKeyMgmt(keyMgmt, "FT-EAP") || hasKeyMgmt(keyMgmt, "FT-EAP-SHA384") ||
        hasKeyMgmt(keyMgmt, "WPA-FILS-SHA256") || hasKeyMgmt(keyMgmt, "WPA-FILS-SHA384") ||
        hasKeyMgmt(keyMgmt, "WPA-FT-FILS-SHA256") || hasKeyMgmt(keyMgmt, "WPA-FT-FILS-SHA384") ||
        hasKeyMgmt(keyMgmt, "IEEE8021X"))
        return WifiSecurity::enterprise;

    if (hasKeyMgmt(keyMgmt, "WPA-PSK") || hasKeyMgmt(keyMgmt, "WPA-PSK-SHA256") ||
        hasKeyMgmt(keyMgmt, "WPA-FT-PSK") || hasKeyMgmt(keyMgmt, "FT-PSK") ||
        hasKeyMgmt(keyMgmt, "SAE") || hasKeyMgmt(keyMgmt, "SAE-EXT-KEY") ||
        hasKeyMgmt(keyMgmt, "FT-SAE") || hasKeyMgmt(keyMgmt, "FT-SAE-EXT-KEY"))
        return WifiSecurity::personal;

    // An empty key management list is what an access point without a WPA or RSN information
    // element reports. "NONE" and "WPA-NONE" are the stored forms, and OWE is an open network
    // with opportunistic encryption that the frontend does not configure.
    return WifiSecurity::open;
}

WifiKeyMgmt WifiState::preferredKeyMgmt(const std::vector<std::string>& keyMgmt)
{
    if (hasKeyMgmt(keyMgmt, "WPA-EAP") || hasKeyMgmt(keyMgmt, "WPA-EAP-SHA256") ||
        hasKeyMgmt(keyMgmt, "WPA-EAP-SHA384") || hasKeyMgmt(keyMgmt, "WPA-EAP-SUITE-B") ||
        hasKeyMgmt(keyMgmt, "WPA-EAP-SUITE-B-192") || hasKeyMgmt(keyMgmt, "WPA-FT-EAP") ||
        hasKeyMgmt(keyMgmt, "FT-EAP") || hasKeyMgmt(keyMgmt, "FT-EAP-SHA384") ||
        hasKeyMgmt(keyMgmt, "WPA-FILS-SHA256") || hasKeyMgmt(keyMgmt, "WPA-FILS-SHA384") ||
        hasKeyMgmt(keyMgmt, "WPA-FT-FILS-SHA256") || hasKeyMgmt(keyMgmt, "WPA-FT-FILS-SHA384") ||
        hasKeyMgmt(keyMgmt, "IEEE8021X"))
        return WifiKeyMgmt::wpaEap;

    // A PSK suite wins over SAE when an access point advertises both. That is the WPA2/WPA3
    // transition mode, and asking for SAE there stalls Broadcom brcmfmac cards.
    if (hasKeyMgmt(keyMgmt, "WPA-PSK") || hasKeyMgmt(keyMgmt, "WPA-PSK-SHA256") ||
        hasKeyMgmt(keyMgmt, "WPA-FT-PSK") || hasKeyMgmt(keyMgmt, "FT-PSK"))
        return WifiKeyMgmt::wpaPsk;

    if (hasKeyMgmt(keyMgmt, "SAE") || hasKeyMgmt(keyMgmt, "SAE-EXT-KEY") ||
        hasKeyMgmt(keyMgmt, "FT-SAE") || hasKeyMgmt(keyMgmt, "FT-SAE-EXT-KEY"))
        return WifiKeyMgmt::sae;

    // NONE, WPA-NONE and OWE need no key management of their own.
    return WifiKeyMgmt::none;
}

int WifiState::signalPercentage(const int rssi)
{
    // wpa_supplicant reports 0 when the signal is unknown, and a real RSSI is never positive.
    if (rssi >= 0 || rssi <= -100)
        return 0;

    return std::min(100, 2 * (rssi + 100));
}

bool WifiState::validatePassphrase(const std::string& passphrase)
{
    if (passphrase.size() == 64)
        return std::all_of(passphrase.begin(), passphrase.end(), isHexDigit);

    if (passphrase.size() < 8 || passphrase.size() > 63)
        return false;

    return std::all_of(passphrase.begin(), passphrase.end(), [](const char character) {
        const auto byte {static_cast<unsigned char>(character)};
        return byte >= 0x20 && byte <= 0x7e;
    });
}

std::string WifiState::passphraseError() { return "Password must be between 8 and 63 characters"; }

std::string WifiState::describeError(const std::string& errorName, const std::string& message)
{
    static const std::map<std::string, std::string> descriptions {
        {"fi.w1.wpa_supplicant1.InvalidArgs", "The network settings were not accepted"},
        {"fi.w1.wpa_supplicant1.UnknownError", "The Wi-Fi operation failed"},
        {"fi.w1.wpa_supplicant1.NetworkUnknown", "The network is no longer known"},
        {"fi.w1.wpa_supplicant1.NotConnected", "There is no active Wi-Fi connection"},
        {"fi.w1.wpa_supplicant1.InterfaceDisabled", "The Wi-Fi interface is disabled"},
        {"fi.w1.wpa_supplicant1.InterfaceUnknown", "The Wi-Fi interface is not available"},
        {"fi.w1.wpa_supplicant1.ConnectChannelUnavailable", "The Wi-Fi service is busy"},
        {"fi.w1.wpa_supplicant1.ConnectChannelUnsupported", "The operation is not supported"},
        {"fi.w1.wpa_supplicant1.ConnectUnspecifiedError", "The Wi-Fi operation failed"},
        {"fi.w1.wpa_supplicant1.Interface.ScanError", "The scan failed"},
        {"org.freedesktop.DBus.Error.ServiceUnknown", "The Wi-Fi service is not running"},
        {"org.freedesktop.DBus.Error.NoReply", "The Wi-Fi service did not reply"},
        {"org.freedesktop.DBus.Error.TimedOut", "The Wi-Fi service did not reply in time"},
        {"org.freedesktop.DBus.Error.AccessDenied", "Access to the Wi-Fi service was denied"},
    };

    const auto description {descriptions.find(errorName)};
    if (description != descriptions.end())
        return description->second;

    // The D-Bus message of an unknown error is all that is left. No passphrase is ever put into
    // errorName or message by the manager, so this cannot echo a credential.
    if (!message.empty())
        return message;

    return errorName;
}
