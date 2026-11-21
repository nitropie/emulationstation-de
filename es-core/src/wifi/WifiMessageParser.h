//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  WifiMessageParser.h
//
//  Parsing of the sd-bus messages that wpa_supplicant sends and replies with.
//
//  The parsers are separate from the manager that uses them so that they can be exercised with a
//  hand built message instead of a live bus.
//

#ifndef ES_CORE_WIFI_MESSAGE_PARSER_H
#define ES_CORE_WIFI_MESSAGE_PARSER_H

// The parser only exists in builds with the wpa_supplicant backend.
#if defined(WIFI_MANAGER_WPASUPPLICANT)

#include "wifi/WifiState.h"

#include <systemd/sd-bus.h>

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

// A D-Bus property reduced to the value types that the wpa_supplicant interfaces use. All signed
// integers are reduced to std::int64_t and all unsigned ones to std::uint64_t. An array of bytes
// is reduced to a vector, so that an SSID or a BSSID with a NUL byte keeps its length. The nested
// WPA and RSN dictionaries are reduced to their string arrays, which is where KeyMgmt lives.
using WifiPropertyValue = std::variant<bool,
                                       std::int64_t,
                                       std::uint64_t,
                                       std::string,
                                       std::vector<std::uint8_t>,
                                       std::vector<std::string>,
                                       std::map<std::string, std::vector<std::string>>>;

// The properties of one a{sv} dictionary, by property name.
using WifiProperties = std::map<std::string, WifiPropertyValue>;

namespace WifiMessageParser
{
    // Reads an a{sv} dictionary, unwrapping the variant that every value is carried in and
    // skipping the values whose type cannot be reduced.
    bool readProperties(sd_bus_message* message, WifiProperties& properties);

    // Reads the a{sv} and optional as arguments of a PropertiesChanged signal. The caller reads
    // the interface name of org.freedesktop.DBus.Properties.PropertiesChanged itself, so the body
    // this reads is (a{sv}as) on the standard signal and (a{sv}) on wpa_supplicant's own BSS and
    // Network signals.
    bool readPropertiesChanged(sd_bus_message* message,
                               WifiProperties& changed,
                               std::vector<std::string>& invalidated);

    // Reads the two top-level arguments (oa{sv}) of a BSSAdded or NetworkAdded signal.
    bool readObjectAdded(sd_bus_message* message, std::string& path, WifiProperties& properties);

    // Fills a BSS from an a{sv} dictionary: the SSID and the BSSID byte arrays, the Signal and
    // Frequency numbers, and the KeyMgmt suites of the nested WPA and RSN dictionaries.
    void applyBssProperties(const WifiProperties& properties, WifiBss& bss);

    // Fills a saved network from an a{sv} dictionary: the SSID byte array or string, the key
    // management and the enabled state.
    void applyNetworkProperties(const WifiProperties& properties, WifiSavedNetwork& network);

    // Formats a byte array as AA:BB:CC:DD:EE:FF, one colon-separated pair per byte. An empty
    // array stays empty.
    std::string formatMacAddress(const std::vector<std::uint8_t>& bytes);

} // namespace WifiMessageParser

#endif // WIFI_MANAGER_WPASUPPLICANT

#endif // ES_CORE_WIFI_MESSAGE_PARSER_H
