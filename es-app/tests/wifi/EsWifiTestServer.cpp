//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  EsWifiTestServer.cpp
//
//  A wpa_supplicant D-Bus service for the native UI tests. It reads wifi-mock.json, starts the
//  fake service on the session bus and prints a ready line so the runner knows when the frontend
//  can be launched.
//

#include "FakeWpaSupplicantServer.h"

#if defined(WIFI_MANAGER_WPASUPPLICANT)

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>

#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
    volatile std::sig_atomic_t sStop {0};

    void handleSignal(int) { sStop = 1; }

    std::vector<std::string> readStringArray(const rapidjson::Value& value)
    {
        std::vector<std::string> strings;
        if (!value.IsArray())
            return strings;

        for (const auto& entry : value.GetArray()) {
            if (entry.IsString())
                strings.emplace_back(entry.GetString());
        }
        return strings;
    }
} // namespace

int main(int argc, char** argv)
{
    std::string configPath;
    std::string outputPath;

    for (int index {1}; index < argc; ++index) {
        const std::string argument {argv[index]};
        if (argument == "--config" && index + 1 < argc)
            configPath = argv[++index];
        else if (argument == "--config-path" && index + 1 < argc)
            outputPath = argv[++index];
        else {
            std::cerr << "usage: " << argv[0] << " --config <wifi-mock.json> [--config-path <file>]"
                      << std::endl;
            return 2;
        }
    }

    if (configPath.empty()) {
        std::cerr << "es-wifi-test-server: no --config given" << std::endl;
        return 2;
    }

    std::ifstream input {configPath};
    if (!input) {
        std::cerr << "es-wifi-test-server: cannot read " << configPath << std::endl;
        return 1;
    }

    rapidjson::IStreamWrapper stream {input};
    rapidjson::Document document;
    document.ParseStream(stream);
    if (document.HasParseError() || !document.IsObject()) {
        std::cerr << "es-wifi-test-server: invalid JSON in " << configPath << std::endl;
        return 1;
    }

    FakeWpaSupplicantServer server;

    if (document.HasMember("interfaces") && document["interfaces"].IsArray()) {
        for (const auto& entry : document["interfaces"].GetArray()) {
            if (!entry.IsObject() || !entry.HasMember("path") || !entry.HasMember("ifname"))
                continue;
            server.addInterface(entry["path"].GetString(), entry["ifname"].GetString());
        }
    }

    if (document.HasMember("bsss") && document["bsss"].IsArray()) {
        for (const auto& entry : document["bsss"].GetArray()) {
            if (!entry.IsObject() || !entry.HasMember("path") || !entry.HasMember("ssid"))
                continue;

            FakeWpaSupplicantServer::Bss bss;
            bss.ssid = entry["ssid"].GetString();
            if (entry.HasMember("signal") && entry["signal"].IsInt())
                bss.signal = static_cast<std::int16_t>(entry["signal"].GetInt());
            if (entry.HasMember("frequency") && entry["frequency"].IsUint())
                bss.frequency = static_cast<std::uint16_t>(entry["frequency"].GetUint());
            if (entry.HasMember("keyMgmt"))
                bss.keyMgmt = readStringArray(entry["keyMgmt"]);
            server.addBss(entry["path"].GetString(), bss);
        }
    }

    if (document.HasMember("networks") && document["networks"].IsArray()) {
        for (const auto& entry : document["networks"].GetArray()) {
            if (!entry.IsObject() || !entry.HasMember("path") || !entry.HasMember("ssid"))
                continue;

            const char* keyMgmt {entry.HasMember("keyMgmt") && entry["keyMgmt"].IsString() ?
                                     entry["keyMgmt"].GetString() :
                                     "WPA-PSK"};
            const bool enabled {!entry.HasMember("enabled") ||
                                (entry["enabled"].IsBool() && entry["enabled"].GetBool())};
            server.addSavedNetwork(entry["path"].GetString(), entry["ssid"].GetString(), keyMgmt,
                                   enabled);
        }
    }

    if (document.HasMember("passphrases") && document["passphrases"].IsObject()) {
        for (const auto& entry : document["passphrases"].GetObject()) {
            if (entry.value.IsString())
                server.setExpectedPassphrase(entry.name.GetString(), entry.value.GetString());
        }
    }

    if (document.HasMember("associationDelayMs") && document["associationDelayMs"].IsInt())
        server.setAssociationDelayMs(document["associationDelayMs"].GetInt());

    if (document.HasMember("scanFailure") && document["scanFailure"].IsString())
        server.setScanFailure(document["scanFailure"].GetString());

    if (document.HasMember("rfkillPoll") && document["rfkillPoll"].IsBool() &&
        document["rfkillPoll"].GetBool()) {
        const int delay {document.HasMember("rfkillTransitionDelayMs") &&
                                 document["rfkillTransitionDelayMs"].IsInt() ?
                             document["rfkillTransitionDelayMs"].GetInt() :
                             150};
        server.setRfkillPolling(true, delay);
    }

    if (document.HasMember("connected") && document["connected"].IsObject()) {
        const auto& connected {document["connected"]};
        if (connected.HasMember("network") && connected.HasMember("bss")) {
            server.setConnectedState(connected["network"].GetString(),
                                     connected["bss"].GetString());
        }
    }

    if (!outputPath.empty())
        server.setConfigPath(outputPath);

    std::signal(SIGTERM, handleSignal);
    std::signal(SIGINT, handleSignal);

    if (!server.start()) {
        std::cerr << "es-wifi-test-server: could not acquire fi.w1.wpa_supplicant1" << std::endl;
        return 1;
    }

    // The runner waits for this line before launching the frontend, so the name is owned before
    // the first Properties.Get is sent.
    std::cout << "es-wifi-test-server ready" << std::endl;

    while (!sStop)
        usleep(100000);

    server.stop();
    return 0;
}

#else

int main()
{
    std::fprintf(stderr, "es-wifi-test-server requires WIFI_MANAGER\n");
    return 1;
}

#endif // WIFI_MANAGER_WPASUPPLICANT
