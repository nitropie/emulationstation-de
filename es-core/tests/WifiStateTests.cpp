//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  WifiStateTests.cpp
//
//  Test cases for the reduction of the wpa_supplicant objects into a WifiSnapshot.
//

#include "TestHarness.h"

#include "wifi/WifiState.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    WifiBss makeBss(const std::string& path,
                    const std::string& ssid,
                    const int signal,
                    const std::vector<std::string>& keyMgmt = {})
    {
        WifiBss bss;
        bss.path = path;
        bss.ssid = ssid;
        bss.bssid = "AA:BB:CC:DD:EE:FF";
        bss.signal = signal;
        bss.frequency = 2412;
        bss.keyMgmt = keyMgmt;
        return bss;
    }

    WifiSavedNetwork makeNetwork(const std::string& path,
                                 const std::string& ssid,
                                 const std::string& keyMgmt = "WPA-PSK")
    {
        WifiSavedNetwork network;
        network.path = path;
        network.ssid = ssid;
        network.keyMgmt = keyMgmt;
        network.enabled = true;
        return network;
    }

    // A state with one connected, three saved and two scanned networks, chosen so that an order by
    // signal strength alone would list them differently.
    WifiState sortedState()
    {
        WifiState state;
        state.setAvailable(true);
        state.setInterfacePath("/fi/w1/wpa_supplicant1/Interfaces/1");
        state.setInterfaceName("wlan0");
        state.setInterfaceState("completed");
        state.setActiveSsid("Home");

        state.setNetwork("/network/home", makeNetwork("/network/home", "Home"));
        state.setNetwork("/network/alpha", makeNetwork("/network/alpha", "Alpha"));
        state.setNetwork("/network/zulu", makeNetwork("/network/zulu", "Zulu"));

        state.setBss("/bss/home", makeBss("/bss/home", "Home", -70, {"WPA-PSK"}));
        state.setBss("/bss/alpha", makeBss("/bss/alpha", "Alpha", -75, {"WPA-PSK"}));
        state.setBss("/bss/beta", makeBss("/bss/beta", "Beta", -40, {"WPA-PSK"}));
        state.setBss("/bss/echo", makeBss("/bss/echo", "Echo", -40, {"WPA-PSK"}));
        state.setBss("/bss/gamma", makeBss("/bss/gamma", "Gamma", -90, {"WPA-PSK"}));

        return state;
    }

} // namespace

TEST_CASE(ssidsAreDecodedAndEncoded)
{
    const std::string nulBytes {"nul\0byte", 8};
    const std::string rawBytes {"raw\0bytes", 9};

    // The quoted form is what wpa_supplicant uses for a readable SSID.
    EXPECT(WifiState::decodeSsid("\"Cafe WiFi\"") == "Cafe WiFi");

    // The unquoted form is the hexadecimal form of the raw bytes, in either case.
    EXPECT(WifiState::decodeSsid("436166652057694669") == "Cafe WiFi");
    EXPECT(WifiState::decodeSsid("436166C3A9") == "Caf\xc3\xa9");

    // A backslash escapes the next character and \xNN writes a raw byte.
    EXPECT(WifiState::decodeSsid("\"quote\\\"inside\"") == "quote\"inside");
    EXPECT(WifiState::decodeSsid("\"back\\\\slash\"") == "back\\slash");
    EXPECT(WifiState::decodeSsid("\"tab\\there\"") == "tab\there");
    EXPECT(WifiState::decodeSsid("\"nul\\x00byte\"") == nulBytes);

    // A value that is neither quoted nor hexadecimal is already the SSID.
    EXPECT(WifiState::decodeSsid("plain") == "plain");

    // Readable ASCII without a quote or a backslash is quoted.
    EXPECT(WifiState::encodeSsid("Cafe WiFi") == "\"Cafe WiFi\"");
    EXPECT(WifiState::encodeSsid("with space and ~") == "\"with space and ~\"");

    // A quote, a backslash or a byte outside printable ASCII forces the hexadecimal form.
    EXPECT(WifiState::encodeSsid("with\"quote") == "776974682271756f7465");
    EXPECT(WifiState::encodeSsid("back\\slash") == "6261636b5c736c617368");
    EXPECT(WifiState::encodeSsid(std::string {"nul\0byte", 8}) == "6e756c0062797465");
    EXPECT(WifiState::encodeSsid("Caf\xc3\xa9") == "436166c3a9");

    // An empty SSID has no string form.
    EXPECT(WifiState::encodeSsid("").empty());

    // Both directions agree.
    EXPECT(WifiState::decodeSsid(WifiState::encodeSsid("Round Trip")) == "Round Trip");
    EXPECT(WifiState::decodeSsid(WifiState::encodeSsid(rawBytes)) == rawBytes);
}

TEST_CASE(bssidsAreDeduplicatedBySsid)
{
    WifiState state;
    state.setAvailable(true);

    // Three access points with one SSID. The strongest one decides the signal, and it is also the
    // one whose key management is used: the weaker WPA-PSK access point must not turn the network
    // into a WPA-PSK network when the strongest one is SAE only.
    state.setBss("/bss/1", makeBss("/bss/1", "Cafe", -80, {"WPA-PSK"}));
    state.setBss("/bss/2", makeBss("/bss/2", "Cafe", -35, {"SAE"}));
    state.setBss("/bss/3", makeBss("/bss/3", "Cafe", -60, {"WPA-PSK"}));

    // A hidden access point publishes no SSID and must not become a network.
    state.setBss("/bss/hidden", makeBss("/bss/hidden", "", -20));
    state.setBss("/bss/zero", makeBss("/bss/zero", std::string {"\0\0", 2}, -20));

    // A second, unrelated network stays separate.
    state.setBss("/bss/other", makeBss("/bss/other", "Other", -55, {"WPA-PSK"}));

    const WifiSnapshot snapshot {state.toSnapshot(1)};
    EXPECT(snapshot.networks.size() == 2);

    const auto cafe =
        std::find_if(snapshot.networks.begin(), snapshot.networks.end(),
                     [](const WifiNetwork& network) { return network.ssid == "Cafe"; });
    EXPECT(cafe != snapshot.networks.end());
    if (cafe != snapshot.networks.end()) {
        EXPECT(cafe->visible);
        EXPECT(!cafe->saved);
        EXPECT(!cafe->connected);
        EXPECT(cafe->rssi == -35);
        EXPECT(cafe->signal == 100);
        EXPECT(cafe->security == WifiSecurity::personal);
        EXPECT(cafe->preferredKeyMgmt == WifiKeyMgmt::sae);
    }

    state.removeBss("/bss/other");
    EXPECT(state.toSnapshot(2).networks.size() == 1);
}

TEST_CASE(networksAreOrderedByStateThenSignalThenName)
{
    const WifiSnapshot snapshot {sortedState().toSnapshot(9)};

    EXPECT(snapshot.sequence == 9);
    EXPECT(snapshot.available);
    EXPECT(snapshot.connected);
    EXPECT(snapshot.activeSsid == "Home");
    EXPECT(snapshot.networks.size() == 6);
    if (snapshot.networks.size() != 6)
        return;

    // The connected network comes first, then the saved network in range even though the scanned
    // networks are stronger, then the scanned networks by signal and name, then the saved network
    // that is out of range.
    EXPECT(snapshot.networks.at(0).ssid == "Home");
    EXPECT(snapshot.networks.at(1).ssid == "Alpha");
    EXPECT(snapshot.networks.at(2).ssid == "Beta");
    EXPECT(snapshot.networks.at(3).ssid == "Echo");
    EXPECT(snapshot.networks.at(4).ssid == "Gamma");
    EXPECT(snapshot.networks.at(5).ssid == "Zulu");

    EXPECT(snapshot.networks.at(0).connected);
    EXPECT(snapshot.networks.at(0).saved);
    EXPECT(snapshot.networks.at(0).visible);

    EXPECT(!snapshot.networks.at(1).connected);
    EXPECT(snapshot.networks.at(1).saved);
    EXPECT(snapshot.networks.at(1).visible);

    EXPECT(!snapshot.networks.at(2).saved);
    EXPECT(snapshot.networks.at(2).visible);

    // The out of range network keeps its classification but has no signal.
    EXPECT(snapshot.networks.at(5).saved);
    EXPECT(!snapshot.networks.at(5).visible);
    EXPECT(snapshot.networks.at(5).rssi == -100);
    EXPECT(snapshot.networks.at(5).signal == 0);
    EXPECT(snapshot.networks.at(5).security == WifiSecurity::personal);
    EXPECT(snapshot.networks.at(5).preferredKeyMgmt == WifiKeyMgmt::wpaPsk);
}

TEST_CASE(signalStrengthBecomesAPercentage)
{
    EXPECT(WifiState::signalPercentage(-35) == 100);
    EXPECT(WifiState::signalPercentage(-50) == 100);
    EXPECT(WifiState::signalPercentage(-75) == 50);
    EXPECT(WifiState::signalPercentage(-99) == 2);
    EXPECT(WifiState::signalPercentage(-100) == 0);
    EXPECT(WifiState::signalPercentage(-120) == 0);

    // No signal was reported.
    EXPECT(WifiState::signalPercentage(0) == 0);
    EXPECT(WifiState::signalPercentage(10) == 0);
}

TEST_CASE(securityIsClassifiedFromKeyManagement)
{
    EXPECT(WifiState::classifySecurity({}) == WifiSecurity::open);
    EXPECT(WifiState::preferredKeyMgmt({}) == WifiKeyMgmt::none);

    EXPECT(WifiState::classifySecurity({"NONE"}) == WifiSecurity::open);
    EXPECT(WifiState::preferredKeyMgmt({"NONE"}) == WifiKeyMgmt::none);

    EXPECT(WifiState::classifySecurity({"WPA-PSK"}) == WifiSecurity::personal);
    EXPECT(WifiState::preferredKeyMgmt({"WPA-PSK"}) == WifiKeyMgmt::wpaPsk);

    // Suite names arrive in any case.
    EXPECT(WifiState::classifySecurity({"wpa-psk"}) == WifiSecurity::personal);
    EXPECT(WifiState::preferredKeyMgmt({"wpa-psk"}) == WifiKeyMgmt::wpaPsk);

    EXPECT(WifiState::classifySecurity({"WPA-PSK-SHA256"}) == WifiSecurity::personal);
    EXPECT(WifiState::preferredKeyMgmt({"WPA-PSK-SHA256"}) == WifiKeyMgmt::wpaPsk);

    // WPA2/WPA3 transition mode advertises both. PSK is chosen to avoid the SAE stalls of
    // Broadcom brcmfmac cards.
    EXPECT(WifiState::classifySecurity({"WPA-PSK", "SAE"}) == WifiSecurity::personal);
    EXPECT(WifiState::preferredKeyMgmt({"WPA-PSK", "SAE"}) == WifiKeyMgmt::wpaPsk);

    // A WPA3 only access point uses SAE.
    EXPECT(WifiState::classifySecurity({"SAE"}) == WifiSecurity::personal);
    EXPECT(WifiState::preferredKeyMgmt({"SAE"}) == WifiKeyMgmt::sae);

    EXPECT(WifiState::classifySecurity({"WPA-EAP"}) == WifiSecurity::enterprise);
    EXPECT(WifiState::preferredKeyMgmt({"WPA-EAP"}) == WifiKeyMgmt::wpaEap);

    // Enterprise wins when an access point advertises both.
    EXPECT(WifiState::classifySecurity({"WPA-EAP", "WPA-PSK"}) == WifiSecurity::enterprise);
    EXPECT(WifiState::preferredKeyMgmt({"WPA-EAP", "WPA-PSK"}) == WifiKeyMgmt::wpaEap);

    // The suite names that dbus_new_handlers.c emits are lower case and hyphenated.
    EXPECT(WifiState::classifySecurity({"wpa-psk-sha256"}) == WifiSecurity::personal);
    EXPECT(WifiState::preferredKeyMgmt({"wpa-psk-sha256"}) == WifiKeyMgmt::wpaPsk);

    EXPECT(WifiState::classifySecurity({"wpa-ft-psk"}) == WifiSecurity::personal);
    EXPECT(WifiState::preferredKeyMgmt({"wpa-ft-psk"}) == WifiKeyMgmt::wpaPsk);

    EXPECT(WifiState::classifySecurity({"ft-psk"}) == WifiSecurity::personal);
    EXPECT(WifiState::preferredKeyMgmt({"ft-psk"}) == WifiKeyMgmt::wpaPsk);

    EXPECT(WifiState::classifySecurity({"ft-sae"}) == WifiSecurity::personal);
    EXPECT(WifiState::preferredKeyMgmt({"ft-sae"}) == WifiKeyMgmt::sae);

    EXPECT(WifiState::classifySecurity({"ft-sae-ext-key"}) == WifiSecurity::personal);
    EXPECT(WifiState::preferredKeyMgmt({"ft-sae-ext-key"}) == WifiKeyMgmt::sae);

    // A transition mode that advertises wpa-ft-psk and sae still prefers the PSK suite.
    EXPECT(WifiState::classifySecurity({"wpa-ft-psk", "sae"}) == WifiSecurity::personal);
    EXPECT(WifiState::preferredKeyMgmt({"wpa-ft-psk", "sae"}) == WifiKeyMgmt::wpaPsk);

    EXPECT(WifiState::classifySecurity({"wpa-eap-sha256"}) == WifiSecurity::enterprise);
    EXPECT(WifiState::preferredKeyMgmt({"wpa-eap-sha256"}) == WifiKeyMgmt::wpaEap);

    EXPECT(WifiState::classifySecurity({"wpa-ft-eap"}) == WifiSecurity::enterprise);
    EXPECT(WifiState::preferredKeyMgmt({"wpa-ft-eap"}) == WifiKeyMgmt::wpaEap);

    EXPECT(WifiState::classifySecurity({"wpa-eap-suite-b"}) == WifiSecurity::enterprise);
    EXPECT(WifiState::preferredKeyMgmt({"wpa-eap-suite-b"}) == WifiKeyMgmt::wpaEap);

    EXPECT(WifiState::classifySecurity({"wpa-eap-sha384"}) == WifiSecurity::enterprise);
    EXPECT(WifiState::preferredKeyMgmt({"wpa-eap-sha384"}) == WifiKeyMgmt::wpaEap);

    // OWE is an open network that the frontend does not configure.
    EXPECT(WifiState::classifySecurity({"OWE"}) == WifiSecurity::open);
    EXPECT(WifiState::preferredKeyMgmt({"OWE"}) == WifiKeyMgmt::none);
    EXPECT(WifiState::classifySecurity({"WPA-NONE"}) == WifiSecurity::open);
    EXPECT(WifiState::preferredKeyMgmt({"WPA-NONE"}) == WifiKeyMgmt::none);
}

TEST_CASE(passphrasesAreValidated)
{
    const std::string symbols {" ~!@#$%^&*()_+{}", 16};

    // The 8..63 character printable range.
    EXPECT(!WifiState::validatePassphrase(""));
    EXPECT(!WifiState::validatePassphrase("1234567"));
    EXPECT(WifiState::validatePassphrase("12345678"));
    EXPECT(WifiState::validatePassphrase(std::string(63, 'a')));
    EXPECT(WifiState::validatePassphrase(symbols));

    // A character outside printable ASCII is rejected even at the right length.
    EXPECT(!WifiState::validatePassphrase("p\xc3\xa4ssword"));
    EXPECT(!WifiState::validatePassphrase(std::string(8, '\x1f')));
    EXPECT(!WifiState::validatePassphrase(std::string(8, '\x7f')));

    // A 64 character value is only accepted as the hexadecimal form of a raw PSK.
    EXPECT(WifiState::validatePassphrase(std::string(64, 'a')));
    EXPECT(WifiState::validatePassphrase(std::string(64, 'A')));
    EXPECT(WifiState::validatePassphrase(std::string(64, '0')));
    EXPECT(!WifiState::validatePassphrase(std::string(64, 'z')));
    EXPECT(!WifiState::validatePassphrase(std::string(63, 'a') + "g"));
}

TEST_CASE(errorsDoNotEchoCredentials)
{
    // A known error name maps to a fixed description. The D-Bus message is deliberately not
    // repeated, as it is the only place where a diagnostic string could carry a credential.
    const std::string described {WifiState::describeError("fi.w1.wpa_supplicant1.InvalidArgs",
                                                          "psk \"hunter2hunter2\" invalid")};
    EXPECT(described == "The network settings were not accepted");
    EXPECT(described.find("hunter2") == std::string::npos);

    EXPECT(WifiState::describeError("fi.w1.wpa_supplicant1.NetworkUnknown", "") ==
           "The network is no longer known");
    EXPECT(WifiState::describeError("fi.w1.wpa_supplicant1.InterfaceDisabled", "") ==
           "The Wi-Fi interface is disabled");
    EXPECT(WifiState::describeError("fi.w1.wpa_supplicant1.Interface.ScanError", "") ==
           "The scan failed");
    EXPECT(WifiState::describeError("fi.w1.wpa_supplicant1.ConnectChannelUnavailable", "") ==
           "The Wi-Fi service is busy");
    EXPECT(WifiState::describeError("fi.w1.wpa_supplicant1.ConnectUnspecifiedError", "") ==
           "The Wi-Fi operation failed");
    EXPECT(WifiState::describeError("org.freedesktop.DBus.Error.ServiceUnknown", "") ==
           "The Wi-Fi service is not running");
    EXPECT(WifiState::describeError("org.freedesktop.DBus.Error.AccessDenied", "") ==
           "Access to the Wi-Fi service was denied");

    // An unknown name falls back on the message, and then on the name itself.
    EXPECT(WifiState::describeError("org.example.Error.Custom", "Something specific") ==
           "Something specific");
    EXPECT(WifiState::describeError("org.example.Error.Custom", "") == "org.example.Error.Custom");

    // The passphrase message is a constant and cannot contain the attempted value.
    const std::string message {WifiState::passphraseError()};
    EXPECT(message == "Password must be between 8 and 63 characters");
    EXPECT(message.find("hunter2") == std::string::npos);
}

TEST_CASE(repeatedErrorsAdvanceTheErrorSequence)
{
    WifiState state;
    EXPECT(state.toSnapshot(1).errorSequence == 0);

    state.setLastError("The Wi-Fi password was not accepted");
    const WifiSnapshot first {state.toSnapshot(2)};
    EXPECT(first.errorSequence == 1);
    EXPECT(first.lastError == "The Wi-Fi password was not accepted");

    // The exact same text is still a new failure and has to advance the sequence.
    state.setLastError("The Wi-Fi password was not accepted");
    const WifiSnapshot second {state.toSnapshot(3)};
    EXPECT(second.errorSequence == 2);
    EXPECT(second.lastError == first.lastError);

    // Clearing the error is not a failure.
    state.setLastError("");
    EXPECT(state.toSnapshot(4).errorSequence == 2);

    state.setLastError("Another failure");
    EXPECT(state.toSnapshot(5).errorSequence == 3);

    state.clear();
    EXPECT(state.toSnapshot(6).errorSequence == 0);
}

TEST_CASE(interfaceStateAndPendingReachTheSnapshot)
{
    WifiState state;
    state.setAvailable(true);
    state.setPowered(true);
    state.setHardBlocked(false);
    state.setScanning(true);
    state.setInterfacePath("/fi/w1/wpa_supplicant1/Interfaces/1");
    state.setInterfaceName("wlan0");
    state.setInterfaceState("scanning");
    state.setIpAddress("192.168.1.5");
    state.setLastError("The Wi-Fi operation failed");
    state.setBss("/bss/cafe", makeBss("/bss/cafe", "Cafe", -60, {"WPA-PSK"}));
    state.setPending("Cafe", WifiOp::connecting);

    WifiSnapshot snapshot {state.toSnapshot(4)};
    EXPECT(snapshot.available);
    EXPECT(snapshot.powered);
    EXPECT(!snapshot.hardBlocked);
    EXPECT(snapshot.scanning);
    EXPECT(snapshot.interfacePath == "/fi/w1/wpa_supplicant1/Interfaces/1");
    EXPECT(snapshot.interfaceName == "wlan0");
    EXPECT(snapshot.state == "scanning");
    EXPECT(!snapshot.connected);
    EXPECT(snapshot.activeSsid.empty());
    EXPECT(snapshot.ipAddress == "192.168.1.5");
    EXPECT(snapshot.lastError == "The Wi-Fi operation failed");
    EXPECT(snapshot.networks.size() == 1);
    if (snapshot.networks.size() == 1)
        EXPECT(snapshot.networks.front().pending == WifiOp::connecting);

    state.setInterfaceState("completed");
    state.setActiveSsid("Cafe");
    state.setScanning(false);
    state.clearPending("Cafe");
    state.setLastError("");

    snapshot = state.toSnapshot(5);
    EXPECT(snapshot.connected);
    EXPECT(snapshot.networks.size() == 1);
    if (snapshot.networks.size() == 1) {
        EXPECT(snapshot.networks.front().connected);
        EXPECT(snapshot.networks.front().pending == WifiOp::none);
    }
    EXPECT(snapshot.lastError.empty());

    state.clear();
    const WifiSnapshot cleared {state.toSnapshot(6)};
    EXPECT(!cleared.available);
    EXPECT(!cleared.powered);
    EXPECT(cleared.networks.empty());
    EXPECT(cleared.activeSsid.empty());
}
