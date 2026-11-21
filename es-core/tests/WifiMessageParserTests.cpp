//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  WifiMessageParserTests.cpp
//
//  Test cases for the parsing of the wpa_supplicant messages. They build, seal and rewind a
//  message instead of talking to a daemon, so the parsing can be checked without Wi-Fi hardware.
//

#include "TestHarness.h"

// The parser only exists in builds with the wpa_supplicant backend.
#if defined(WIFI_MANAGER_WPASUPPLICANT)

#include "wifi/WifiMessageParser.h"

#include <systemd/sd-bus.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace
{
    constexpr const char* interfacePath {"/fi/w1/wpa_supplicant1/Interfaces/1"};
    constexpr const char* bssPath {"/fi/w1/wpa_supplicant1/Interfaces/1/BSSs/1"};
    constexpr const char* ssidText {"Cafe WiFi"};
    constexpr const char* bssidText {"AA:BB:CC:DD:EE:FF"};

    bool sealAndRewind(sd_bus_message* message)
    {
        if (sd_bus_message_seal(message, 1, 0) < 0)
            return false;

        return sd_bus_message_rewind(message, true) >= 0;
    }

    std::shared_ptr<sd_bus_message> newSignal(sd_bus* bus, const char* member)
    {
        sd_bus_message* message {nullptr};
        if (sd_bus_message_new_signal(bus, &message, interfacePath,
                                      "fi.w1.wpa_supplicant1.Interface", member) < 0)
            return nullptr;

        return std::shared_ptr<sd_bus_message> {message, sd_bus_message_unref};
    }

    void openProperty(sd_bus_message* message, const char* name, const char* signature)
    {
        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", name);
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, signature);
    }

    void closeProperty(sd_bus_message* message)
    {
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
    }

    void appendByteArrayProperty(sd_bus_message* message,
                                 const char* name,
                                 const std::vector<std::uint8_t>& bytes)
    {
        openProperty(message, name, "ay");
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "y");
        for (const std::uint8_t byte : bytes)
            sd_bus_message_append(message, "y", byte);
        sd_bus_message_close_container(message);
        closeProperty(message);
    }

    void appendStringProperty(sd_bus_message* message, const char* name, const char* value)
    {
        openProperty(message, name, "s");
        sd_bus_message_append(message, "s", value);
        closeProperty(message);
    }

    void appendObjectPathProperty(sd_bus_message* message, const char* name, const char* value)
    {
        openProperty(message, name, "o");
        sd_bus_message_append(message, "o", value);
        closeProperty(message);
    }

    void appendInt16Property(sd_bus_message* message, const char* name, const int16_t value)
    {
        openProperty(message, name, "n");
        sd_bus_message_append(message, "n", value);
        closeProperty(message);
    }

    void appendUInt16Property(sd_bus_message* message, const char* name, const uint16_t value)
    {
        openProperty(message, name, "q");
        sd_bus_message_append(message, "q", value);
        closeProperty(message);
    }

    void appendBooleanProperty(sd_bus_message* message, const char* name, const bool value)
    {
        openProperty(message, name, "b");
        sd_bus_message_append(message, "b", value ? 1 : 0);
        closeProperty(message);
    }

    // A value type that the parser does not reduce, to check that it is skipped without disturbing
    // the rest of the dictionary.
    void appendDoubleProperty(sd_bus_message* message, const char* name, const double value)
    {
        openProperty(message, name, "d");
        sd_bus_message_append(message, "d", value);
        closeProperty(message);
    }

    // A composite value type that the parser does not reduce either.
    void appendStructProperty(sd_bus_message* message, const char* name)
    {
        openProperty(message, name, "(ib)");
        sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT, "ib");
        sd_bus_message_append(message, "i", 7);
        sd_bus_message_append(message, "b", 1);
        sd_bus_message_close_container(message);
        closeProperty(message);
    }

    // The nested WPA and RSN dictionaries. KeyMgmt is an array of strings, which is what the parser
    // keeps; GroupCipher is a plain string, which has no list form and is skipped.
    void appendNestedKeyMgmtProperty(sd_bus_message* message,
                                     const char* name,
                                     const std::vector<std::string>& keyMgmt)
    {
        openProperty(message, name, "a{sv}");
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");

        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", "KeyMgmt");
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "as");
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "s");
        for (const std::string& suite : keyMgmt)
            sd_bus_message_append(message, "s", suite.c_str());
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);

        sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        sd_bus_message_append(message, "s", "GroupCipher");
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "s");
        sd_bus_message_append(message, "s", "CCMP");
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);

        sd_bus_message_close_container(message);
        closeProperty(message);
    }

    std::vector<std::uint8_t> byteValues(const std::string& text)
    {
        return std::vector<std::uint8_t> {text.begin(), text.end()};
    }

    // A body that is a plain a{sv} dictionary, as the properties of a BSS or a Network arrive.
    std::shared_ptr<sd_bus_message> buildPropertiesMessage(sd_bus* bus, const bool unknownsFirst)
    {
        sd_bus_message* message {nullptr};
        if (sd_bus_message_new_method_call(bus, &message, "fi.w1.wpa_supplicant1", interfacePath,
                                           "fi.w1.wpa_supplicant1.Interface", "Properties") < 0)
            return nullptr;

        std::shared_ptr<sd_bus_message> holder {message, sd_bus_message_unref};

        if (sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}") < 0)
            return nullptr;

        if (unknownsFirst) {
            appendDoubleProperty(message, "Unsupported", 1.5);
            appendStructProperty(message, "AnotherUnknown");
        }

        appendByteArrayProperty(message, "SSID", byteValues(ssidText));
        appendByteArrayProperty(message, "BSSID", {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF});
        appendObjectPathProperty(message, "CurrentBSS", bssPath);
        appendInt16Property(message, "Signal", -42);
        appendUInt16Property(message, "Frequency", 5180);
        appendBooleanProperty(message, "Privacy", true);
        appendNestedKeyMgmtProperty(message, "WPA", {"WPA-PSK"});
        appendNestedKeyMgmtProperty(message, "RSN", {"SAE"});

        if (sd_bus_message_close_container(message) < 0)
            return nullptr;

        if (!sealAndRewind(message))
            return nullptr;

        return holder;
    }

    // The two top-level arguments (oa{sv}) of a BSSAdded signal.
    std::shared_ptr<sd_bus_message> buildBssAddedMessage(sd_bus* bus)
    {
        const std::shared_ptr<sd_bus_message> holder {newSignal(bus, "BSSAdded")};
        if (holder == nullptr)
            return nullptr;

        sd_bus_message* message {holder.get()};
        sd_bus_message_append(message, "o", bssPath);
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
        appendByteArrayProperty(message, "SSID", byteValues(ssidText));
        appendInt16Property(message, "Signal", -55);
        appendNestedKeyMgmtProperty(message, "RSN", {"WPA-PSK", "SAE"});
        sd_bus_message_close_container(message);

        if (!sealAndRewind(message))
            return nullptr;

        return holder;
    }

    // The two top-level arguments (oa{sv}) of a NetworkAdded signal.
    std::shared_ptr<sd_bus_message> buildNetworkAddedMessage(sd_bus* bus, const bool disabled)
    {
        const std::shared_ptr<sd_bus_message> holder {newSignal(bus, "NetworkAdded")};
        if (holder == nullptr)
            return nullptr;

        sd_bus_message* message {holder.get()};
        sd_bus_message_append(message, "o", "/fi/w1/wpa_supplicant1/Interfaces/1/Networks/0");
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
        appendByteArrayProperty(message, "ssid", byteValues("Home"));
        appendStringProperty(message, "key_mgmt", "WPA-PSK");
        appendBooleanProperty(message, "disabled", disabled);
        appendUInt16Property(message, "Frequency", 2412);
        sd_bus_message_close_container(message);

        if (!sealAndRewind(message))
            return nullptr;

        return holder;
    }

    // The top-level arguments of a PropertiesChanged signal. The standard
    // org.freedesktop.DBus.Properties.PropertiesChanged body is (sa{sv}as); wpa_supplicant's own
    // BSS and Network PropertiesChanged signal only carries (a{sv}).
    std::shared_ptr<sd_bus_message> buildPropertiesChangedMessage(sd_bus* bus,
                                                                  const bool withInterfaceName)
    {
        const std::shared_ptr<sd_bus_message> holder {newSignal(bus, "PropertiesChanged")};
        if (holder == nullptr)
            return nullptr;

        sd_bus_message* message {holder.get()};
        if (withInterfaceName)
            sd_bus_message_append(message, "s", "fi.w1.wpa_supplicant1.BSS");

        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
        appendByteArrayProperty(message, "SSID", byteValues(ssidText));
        appendInt16Property(message, "Signal", -50);
        appendDoubleProperty(message, "Unsupported", 2.5);
        sd_bus_message_close_container(message);

        if (withInterfaceName) {
            sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "s");
            sd_bus_message_append(message, "s", "BSSID");
            sd_bus_message_append(message, "s", "WPA");
            sd_bus_message_close_container(message);
        }

        if (!sealAndRewind(message))
            return nullptr;

        return holder;
    }

    // Opens a method call that only exists to carry a body for the parser.
    sd_bus* openTestBus()
    {
        sd_bus* bus {nullptr};
        if (sd_bus_open_user(&bus) < 0)
            return nullptr;

        return bus;
    }

} // namespace

TEST_CASE(propertiesAreParsedWithEveryValueType)
{
    sd_bus* bus {openTestBus()};
    EXPECT(bus != nullptr);
    if (bus == nullptr)
        return;

    const std::shared_ptr<sd_bus_message> message {buildPropertiesMessage(bus, false)};
    sd_bus_unref(bus);

    EXPECT(message != nullptr);
    if (message == nullptr)
        return;

    WifiProperties properties;
    EXPECT(WifiMessageParser::readProperties(message.get(), properties));
    EXPECT(properties.size() == 8);

    const auto ssid {properties.find("SSID")};
    EXPECT(ssid != properties.end());
    if (ssid != properties.end()) {
        const auto bytes {std::get_if<std::vector<std::uint8_t>>(&ssid->second)};
        EXPECT(bytes != nullptr && bytes->size() == 9);
        if (bytes != nullptr) {
            const std::string decoded {bytes->begin(), bytes->end()};
            EXPECT(decoded == ssidText);
        }
    }

    const auto signal {properties.find("Signal")};
    EXPECT(signal != properties.end());
    if (signal != properties.end()) {
        const auto number {std::get_if<std::int64_t>(&signal->second)};
        EXPECT(number != nullptr && *number == -42);
    }

    // An object path inside a{sv}, as wpa_supplicant reports CurrentBSS and CurrentNetwork.
    const auto currentBss {properties.find("CurrentBSS")};
    EXPECT(currentBss != properties.end());
    if (currentBss != properties.end()) {
        const auto path {std::get_if<std::string>(&currentBss->second)};
        EXPECT(path != nullptr && *path == bssPath);
    }

    const auto frequency {properties.find("Frequency")};
    EXPECT(frequency != properties.end());
    if (frequency != properties.end()) {
        const auto number {std::get_if<std::uint64_t>(&frequency->second)};
        EXPECT(number != nullptr && *number == 5180);
    }

    const auto privacy {properties.find("Privacy")};
    EXPECT(privacy != properties.end());
    if (privacy != properties.end()) {
        const auto boolean {std::get_if<bool>(&privacy->second)};
        EXPECT(boolean != nullptr && *boolean);
    }

    // The nested dictionaries only keep their string arrays.
    const auto wpa {properties.find("WPA")};
    EXPECT(wpa != properties.end());
    if (wpa != properties.end()) {
        const auto nested {
            std::get_if<std::map<std::string, std::vector<std::string>>>(&wpa->second)};
        EXPECT(nested != nullptr);
        if (nested != nullptr) {
            const auto suites {nested->find("KeyMgmt")};
            EXPECT(suites != nested->end());
            if (suites != nested->end()) {
                EXPECT(suites->second.size() == 1);
                if (suites->second.size() == 1)
                    EXPECT(suites->second.front() == "WPA-PSK");
            }
            EXPECT(nested->find("GroupCipher") == nested->end());
        }
    }

    // Each access point is reduced into a BSS with the decoded SSID, the MAC address, the signal,
    // the frequency and the key management suites of both information elements.
    WifiBss bss;
    WifiMessageParser::applyBssProperties(properties, bss);
    EXPECT(bss.ssid == ssidText);
    EXPECT(bss.bssid == bssidText);
    EXPECT(bss.signal == -42);
    EXPECT(bss.frequency == 5180);
    EXPECT(bss.keyMgmt.size() == 2);
    if (bss.keyMgmt.size() == 2) {
        EXPECT(bss.keyMgmt.at(0) == "WPA-PSK");
        EXPECT(bss.keyMgmt.at(1) == "SAE");
    }
}

TEST_CASE(byteArraysKeepEveryByte)
{
    sd_bus* bus {openTestBus()};
    EXPECT(bus != nullptr);
    if (bus == nullptr)
        return;

    sd_bus_message* raw {nullptr};
    if (sd_bus_message_new_method_call(bus, &raw, "fi.w1.wpa_supplicant1", interfacePath,
                                       "fi.w1.wpa_supplicant1.Interface", "Properties") < 0) {
        sd_bus_unref(bus);
        EXPECT(false);
        return;
    }

    std::shared_ptr<sd_bus_message> message {raw, sd_bus_message_unref};
    sd_bus_message_open_container(raw, SD_BUS_TYPE_ARRAY, "{sv}");
    appendByteArrayProperty(raw, "SSID", {0x41, 0x00, 0x42, 0xFF});
    sd_bus_message_close_container(raw);
    const bool sealed {sealAndRewind(raw)};
    sd_bus_unref(bus);

    EXPECT(sealed);
    if (!sealed)
        return;

    WifiProperties properties;
    EXPECT(WifiMessageParser::readProperties(message.get(), properties));

    const auto ssid {properties.find("SSID")};
    EXPECT(ssid != properties.end());
    if (ssid != properties.end()) {
        const auto bytes {std::get_if<std::vector<std::uint8_t>>(&ssid->second)};
        EXPECT(bytes != nullptr);
        if (bytes != nullptr) {
            EXPECT(bytes->size() == 4);
            if (bytes->size() == 4) {
                EXPECT(bytes->at(0) == 0x41);
                EXPECT(bytes->at(1) == 0x00);
                EXPECT(bytes->at(2) == 0x42);
                EXPECT(bytes->at(3) == 0xFF);
            }
        }
    }

    // The SSID keeps the embedded NUL and the trailing byte; it is not a C string.
    const std::string expectedSsid {"A\0B\xff", 4};
    WifiBss bss;
    WifiMessageParser::applyBssProperties(properties, bss);
    EXPECT(bss.ssid.size() == 4);
    EXPECT(bss.ssid == expectedSsid);
}

TEST_CASE(bssAddedCarriesTheObjectPath)
{
    sd_bus* bus {openTestBus()};
    EXPECT(bus != nullptr);
    if (bus == nullptr)
        return;

    const std::shared_ptr<sd_bus_message> message {buildBssAddedMessage(bus)};
    sd_bus_unref(bus);

    EXPECT(message != nullptr);
    if (message == nullptr)
        return;

    std::string path;
    WifiProperties properties;
    EXPECT(WifiMessageParser::readObjectAdded(message.get(), path, properties));
    EXPECT(path == bssPath);

    WifiBss bss;
    WifiMessageParser::applyBssProperties(properties, bss);
    EXPECT(bss.ssid == ssidText);
    EXPECT(bss.signal == -55);
    EXPECT(bss.keyMgmt.size() == 2);
}

TEST_CASE(propertiesChangedCarriesInvalidatedNames)
{
    sd_bus* bus {openTestBus()};
    EXPECT(bus != nullptr);
    if (bus == nullptr)
        return;

    const std::shared_ptr<sd_bus_message> message {buildPropertiesChangedMessage(bus, true)};
    sd_bus_unref(bus);

    EXPECT(message != nullptr);
    if (message == nullptr)
        return;

    // The standard org.freedesktop.DBus.Properties.PropertiesChanged body starts with the
    // interface name, which the manager reads before handing the rest to the parser.
    const char* interfaceName {nullptr};
    EXPECT(sd_bus_message_read(message.get(), "s", &interfaceName) > 0);
    const std::string readInterface {interfaceName != nullptr ? interfaceName : ""};
    EXPECT(readInterface == "fi.w1.wpa_supplicant1.BSS");

    WifiProperties changed;
    std::vector<std::string> invalidated;
    EXPECT(WifiMessageParser::readPropertiesChanged(message.get(), changed, invalidated));

    EXPECT(changed.size() == 2);
    const auto signal {changed.find("Signal")};
    EXPECT(signal != changed.end());
    if (signal != changed.end()) {
        const auto number {std::get_if<std::int64_t>(&signal->second)};
        EXPECT(number != nullptr && *number == -50);
    }
    EXPECT(changed.find("Unsupported") == changed.end());

    EXPECT(invalidated.size() == 2);
    if (invalidated.size() == 2) {
        EXPECT(invalidated.at(0) == "BSSID");
        EXPECT(invalidated.at(1) == "WPA");
    }
}

TEST_CASE(propertiesChangedWithoutInvalidatedNames)
{
    sd_bus* bus {openTestBus()};
    EXPECT(bus != nullptr);
    if (bus == nullptr)
        return;

    // wpa_supplicant's own PropertiesChanged on a BSS or Network only carries the a{sv}.
    const std::shared_ptr<sd_bus_message> message {buildPropertiesChangedMessage(bus, false)};
    sd_bus_unref(bus);

    EXPECT(message != nullptr);
    if (message == nullptr)
        return;

    WifiProperties changed;
    std::vector<std::string> invalidated;
    EXPECT(WifiMessageParser::readPropertiesChanged(message.get(), changed, invalidated));

    EXPECT(changed.size() == 2);
    EXPECT(changed.find("SSID") != changed.end());
    EXPECT(changed.find("Signal") != changed.end());
    EXPECT(invalidated.empty());
}

TEST_CASE(networkPropertiesAreParsed)
{
    sd_bus* bus {openTestBus()};
    EXPECT(bus != nullptr);
    if (bus == nullptr)
        return;

    const std::shared_ptr<sd_bus_message> message {buildNetworkAddedMessage(bus, false)};
    sd_bus_unref(bus);

    EXPECT(message != nullptr);
    if (message == nullptr)
        return;

    std::string path;
    WifiProperties properties;
    EXPECT(WifiMessageParser::readObjectAdded(message.get(), path, properties));
    EXPECT(path == "/fi/w1/wpa_supplicant1/Interfaces/1/Networks/0");

    WifiSavedNetwork network;
    WifiMessageParser::applyNetworkProperties(properties, network);
    EXPECT(network.ssid == "Home");
    EXPECT(network.keyMgmt == "WPA-PSK");
    EXPECT(network.enabled);
}

TEST_CASE(disabledNetworksAreParsed)
{
    sd_bus* bus {openTestBus()};
    EXPECT(bus != nullptr);
    if (bus == nullptr)
        return;

    const std::shared_ptr<sd_bus_message> message {buildNetworkAddedMessage(bus, true)};
    sd_bus_unref(bus);

    EXPECT(message != nullptr);
    if (message == nullptr)
        return;

    WifiProperties properties;
    std::string path;
    EXPECT(WifiMessageParser::readObjectAdded(message.get(), path, properties));

    WifiSavedNetwork network;
    WifiMessageParser::applyNetworkProperties(properties, network);
    EXPECT(network.ssid == "Home");
    EXPECT(!network.enabled);
}

TEST_CASE(unknownVariantsDoNotBreakTheDictionary)
{
    sd_bus* bus {openTestBus()};
    EXPECT(bus != nullptr);
    if (bus == nullptr)
        return;

    const std::shared_ptr<sd_bus_message> message {buildPropertiesMessage(bus, true)};
    sd_bus_unref(bus);

    EXPECT(message != nullptr);
    if (message == nullptr)
        return;

    WifiProperties properties;
    EXPECT(WifiMessageParser::readProperties(message.get(), properties));

    // The two string and double values in front of the known properties were dropped, and the
    // dictionary after them was still read to its end.
    EXPECT(properties.find("Unsupported") == properties.end());
    EXPECT(properties.find("AnotherUnknown") == properties.end());
    EXPECT(properties.find("SSID") != properties.end());
    EXPECT(properties.find("Signal") != properties.end());
    EXPECT(properties.find("RSN") != properties.end());

    WifiBss bss;
    WifiMessageParser::applyBssProperties(properties, bss);
    EXPECT(bss.ssid == ssidText);
    EXPECT(bss.signal == -42);
}

#endif // WIFI_MANAGER_WPASUPPLICANT
