//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  WifiMessageParser.cpp
//
//  Parsing of the sd-bus messages that wpa_supplicant sends and replies with.
//

#include "wifi/WifiMessageParser.h"

#if defined(WIFI_MANAGER_WPASUPPLICANT)

namespace WifiMessageParser
{
    namespace
    {
        // Reads an "ay" into a vector, one element at a time. Reading the array element by
        // element keeps the length, including the NUL bytes that an SSID may contain, and does not
        // depend on sd_bus_message_read_array(), which rejects a hand built message with EBADMSG
        // on some libsystemd versions.
        bool readByteArray(sd_bus_message* message, std::vector<std::uint8_t>& bytes)
        {
            if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "y") < 0)
                return false;

            std::uint8_t byte {0};
            int result {0};
            while ((result = sd_bus_message_read(message, "y", &byte)) > 0)
                bytes.push_back(byte);

            if (result < 0)
                return false;

            return sd_bus_message_exit_container(message) >= 0;
        }

        // Reads an "as".
        bool readStringArray(sd_bus_message* message, std::vector<std::string>& strings)
        {
            if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "s") < 0)
                return false;

            const char* text {nullptr};
            int result {0};
            while ((result = sd_bus_message_read(message, "s", &text)) > 0)
                strings.emplace_back(text);

            if (result < 0)
                return false;

            return sd_bus_message_exit_container(message) >= 0;
        }

        // Reads the nested WPA and RSN "a{sv}" dictionaries. Only the string arrays are kept, which
        // is where KeyMgmt and PairwiseCiphers live; every other value type is skipped without
        // losing the position in the dictionary.
        bool readNestedProperties(sd_bus_message* message,
                                  std::map<std::string, std::vector<std::string>>& properties)
        {
            if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}") < 0)
                return false;

            while (true) {
                const int result {
                    sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv")};
                if (result < 0)
                    return false;
                if (result == 0)
                    break;

                const char* name {nullptr};
                if (sd_bus_message_read(message, "s", &name) < 0)
                    return false;

                char type {0};
                const char* contents {nullptr};
                if (sd_bus_message_peek_type(message, &type, &contents) >= 0 &&
                    type == SD_BUS_TYPE_VARIANT &&
                    sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, contents) >= 0) {
                    if (contents != nullptr && std::string {contents} == "as") {
                        std::vector<std::string> values;
                        if (!readStringArray(message, values))
                            return false;
                        properties[name] = std::move(values);
                    }
                    else if (sd_bus_message_skip(message, nullptr) < 0) {
                        return false;
                    }

                    if (sd_bus_message_exit_container(message) < 0)
                        return false;
                }
                else if (sd_bus_message_skip(message, nullptr) < 0) {
                    return false;
                }

                if (sd_bus_message_exit_container(message) < 0)
                    return false;
            }

            return sd_bus_message_exit_container(message) >= 0;
        }

        // Reads the value inside an entered variant.
        bool readPropertyValue(sd_bus_message* message, WifiPropertyValue& value)
        {
            char type {0};
            const char* contents {nullptr};
            if (sd_bus_message_peek_type(message, &type, &contents) < 0)
                return false;

            switch (type) {
                case SD_BUS_TYPE_BOOLEAN: {
                    int boolean {0};
                    if (sd_bus_message_read(message, "b", &boolean) < 0)
                        return false;
                    value = boolean != 0;
                    return true;
                }
                case SD_BUS_TYPE_BYTE: {
                    std::uint8_t number {0};
                    if (sd_bus_message_read(message, "y", &number) < 0)
                        return false;
                    value = static_cast<std::uint64_t>(number);
                    return true;
                }
                case SD_BUS_TYPE_INT16: {
                    std::int16_t number {0};
                    if (sd_bus_message_read(message, "n", &number) < 0)
                        return false;
                    value = static_cast<std::int64_t>(number);
                    return true;
                }
                case SD_BUS_TYPE_UINT16: {
                    std::uint16_t number {0};
                    if (sd_bus_message_read(message, "q", &number) < 0)
                        return false;
                    value = static_cast<std::uint64_t>(number);
                    return true;
                }
                case SD_BUS_TYPE_INT32: {
                    std::int32_t number {0};
                    if (sd_bus_message_read(message, "i", &number) < 0)
                        return false;
                    value = static_cast<std::int64_t>(number);
                    return true;
                }
                case SD_BUS_TYPE_UINT32: {
                    std::uint32_t number {0};
                    if (sd_bus_message_read(message, "u", &number) < 0)
                        return false;
                    value = static_cast<std::uint64_t>(number);
                    return true;
                }
                case SD_BUS_TYPE_INT64: {
                    std::int64_t number {0};
                    if (sd_bus_message_read(message, "x", &number) < 0)
                        return false;
                    value = number;
                    return true;
                }
                case SD_BUS_TYPE_UINT64: {
                    std::uint64_t number {0};
                    if (sd_bus_message_read(message, "t", &number) < 0)
                        return false;
                    value = number;
                    return true;
                }
                case SD_BUS_TYPE_STRING: {
                    const char* text {nullptr};
                    if (sd_bus_message_read(message, "s", &text) < 0)
                        return false;
                    value = std::string {text};
                    return true;
                }
                case SD_BUS_TYPE_OBJECT_PATH: {
                    const char* text {nullptr};
                    if (sd_bus_message_read(message, "o", &text) < 0)
                        return false;
                    value = std::string {text};
                    return true;
                }
                case SD_BUS_TYPE_SIGNATURE: {
                    const char* text {nullptr};
                    if (sd_bus_message_read(message, "g", &text) < 0)
                        return false;
                    value = std::string {text};
                    return true;
                }
                case SD_BUS_TYPE_ARRAY: {
                    if (contents == nullptr)
                        return false;

                    const std::string signature {contents};
                    if (signature == "y") {
                        std::vector<std::uint8_t> bytes;
                        if (!readByteArray(message, bytes))
                            return false;
                        value = std::move(bytes);
                        return true;
                    }
                    if (signature == "s") {
                        std::vector<std::string> strings;
                        if (!readStringArray(message, strings))
                            return false;
                        value = std::move(strings);
                        return true;
                    }
                    if (signature == "{sv}") {
                        std::map<std::string, std::vector<std::string>> nested;
                        if (!readNestedProperties(message, nested))
                            return false;
                        value = std::move(nested);
                        return true;
                    }
                    return false;
                }
                default:
                    return false;
            }
        }

        const WifiPropertyValue* findProperty(const WifiProperties& properties, const char* name)
        {
            const auto property {properties.find(name)};
            return property == properties.end() ? nullptr : &property->second;
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

        bool getUnsigned(const WifiProperties& properties, const char* name, std::uint64_t& result)
        {
            const WifiPropertyValue* value {findProperty(properties, name)};
            if (value == nullptr)
                return false;

            if (const auto number {std::get_if<std::uint64_t>(value)}; number != nullptr) {
                result = *number;
                return true;
            }
            if (const auto number {std::get_if<std::int64_t>(value)}; number != nullptr) {
                if (*number < 0)
                    return false;
                result = static_cast<std::uint64_t>(*number);
                return true;
            }

            return false;
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

        bool getByteArray(const WifiProperties& properties,
                          const char* name,
                          std::vector<std::uint8_t>& result)
        {
            const WifiPropertyValue* value {findProperty(properties, name)};
            if (value == nullptr)
                return false;

            const auto bytes {std::get_if<std::vector<std::uint8_t>>(value)};
            if (bytes == nullptr)
                return false;

            result = *bytes;
            return true;
        }

        // A string property holds the SSID in encoded form when the value is not a byte array. The
        // parser accepts both, as a fake or a future wpa_supplicant may use either.
        void applySsid(const WifiProperties& properties, std::string& ssid)
        {
            std::vector<std::uint8_t> bytes;
            if (getByteArray(properties, "SSID", bytes) ||
                getByteArray(properties, "ssid", bytes)) {
                if (!bytes.empty())
                    ssid.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                else
                    ssid.clear();
                return;
            }

            std::string text;
            if (getString(properties, "SSID", text) || getString(properties, "ssid", text))
                ssid = WifiState::decodeSsid(text);
        }

        // Appends the KeyMgmt suites of a nested WPA or RSN dictionary.
        void applyKeyMgmt(const WifiProperties& properties, const char* name, WifiBss& bss)
        {
            const WifiPropertyValue* value {findProperty(properties, name)};
            if (value == nullptr)
                return;

            const auto nested {std::get_if<std::map<std::string, std::vector<std::string>>>(value)};
            if (nested == nullptr)
                return;

            const auto keyMgmt {nested->find("KeyMgmt")};
            if (keyMgmt == nested->end())
                return;

            bss.keyMgmt.insert(bss.keyMgmt.end(), keyMgmt->second.begin(), keyMgmt->second.end());
        }

    } // namespace

    bool readProperties(sd_bus_message* message, WifiProperties& properties)
    {
        if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}") < 0)
            return false;

        while (true) {
            const int result {
                sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv")};
            if (result < 0)
                return false;
            if (result == 0)
                break;

            const char* name {nullptr};
            if (sd_bus_message_read(message, "s", &name) < 0)
                return false;

            // Every value of an a{sv} is wrapped in a variant, and the type of the value is only
            // known from the signature of that variant.
            char type {0};
            const char* contents {nullptr};

            const bool isVariant {
                sd_bus_message_peek_type(message, &type, &contents) >= 0 &&
                type == SD_BUS_TYPE_VARIANT &&
                sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, contents) >= 0};

            if (isVariant) {
                WifiPropertyValue value;
                if (readPropertyValue(message, value))
                    properties[name] = std::move(value);
                else if (sd_bus_message_skip(message, nullptr) < 0)
                    return false;

                if (sd_bus_message_exit_container(message) < 0)
                    return false;
            }
            else {
                // Not a variant at all, so consume it to leave the dictionary balanced.
                if (sd_bus_message_skip(message, nullptr) < 0)
                    return false;
            }

            if (sd_bus_message_exit_container(message) < 0)
                return false;
        }

        return sd_bus_message_exit_container(message) >= 0;
    }

    bool readPropertiesChanged(sd_bus_message* message,
                               WifiProperties& changed,
                               std::vector<std::string>& invalidated)
    {
        // The arguments are carried directly on the message, not inside a struct. The caller has
        // already consumed the interface name of the standard
        // org.freedesktop.DBus.Properties.PropertiesChanged signal, whose body is (sa{sv}as).
        // wpa_supplicant's own PropertiesChanged on a BSS or a Network only has the (a{sv}) part,
        // so the invalidated array is optional.
        if (!readProperties(message, changed))
            return false;

        if (sd_bus_message_at_end(message, 0) > 0)
            return true;

        char type {0};
        const char* contents {nullptr};
        if (sd_bus_message_peek_type(message, &type, &contents) <= 0 || type != SD_BUS_TYPE_ARRAY)
            return true;

        return readStringArray(message, invalidated);
    }

    bool readObjectAdded(sd_bus_message* message, std::string& path, WifiProperties& properties)
    {
        // The object path and the properties are two top-level arguments of the signal, not a
        // struct.
        const char* objectPath {nullptr};
        if (sd_bus_message_read(message, "o", &objectPath) < 0)
            return false;
        path = objectPath;

        return readProperties(message, properties);
    }

    void applyBssProperties(const WifiProperties& properties, WifiBss& bss)
    {
        applySsid(properties, bss.ssid);

        std::vector<std::uint8_t> bssid;
        if (getByteArray(properties, "BSSID", bssid)) {
            bss.bssid = formatMacAddress(bssid);
        }
        else {
            std::string text;
            if (getString(properties, "BSSID", text))
                bss.bssid = text;
        }

        std::int64_t signal {0};
        if (getSigned(properties, "Signal", signal))
            bss.signal = static_cast<int>(signal);

        std::uint64_t frequency {0};
        if (getUnsigned(properties, "Frequency", frequency))
            bss.frequency = static_cast<std::uint32_t>(frequency);

        applyKeyMgmt(properties, "WPA", bss);
        applyKeyMgmt(properties, "RSN", bss);
    }

    void applyNetworkProperties(const WifiProperties& properties, WifiSavedNetwork& network)
    {
        applySsid(properties, network.ssid);

        std::string keyMgmt;
        if (getString(properties, "key_mgmt", keyMgmt)) {
            network.keyMgmt = keyMgmt;
        }
        else {
            const WifiPropertyValue* value {findProperty(properties, "key_mgmt")};
            if (value != nullptr) {
                if (const auto list {std::get_if<std::vector<std::string>>(value)};
                    list != nullptr && !list->empty())
                    network.keyMgmt = list->front();
            }
        }

        // wpa_supplicant reports the state of a Network either as "disabled" in the Properties
        // dictionary or as the "Enabled" property of the interface.
        bool flag {false};
        if (getBoolean(properties, "disabled", flag))
            network.enabled = !flag;
        else if (getBoolean(properties, "Enabled", flag))
            network.enabled = flag;
    }

    std::string formatMacAddress(const std::vector<std::uint8_t>& bytes)
    {
        static constexpr char digits[] {"0123456789ABCDEF"};

        std::string address;
        address.reserve(bytes.size() * 3);

        for (std::size_t index {0}; index < bytes.size(); ++index) {
            if (index != 0)
                address.push_back(':');
            address.push_back(digits[bytes[index] >> 4]);
            address.push_back(digits[bytes[index] & 0x0f]);
        }

        return address;
    }

} // namespace WifiMessageParser

#endif // WIFI_MANAGER_WPASUPPLICANT
