//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GuiWifiSettings.h
//
//  Wi-Fi network management screen.
//
//  Every row is rebuilt from the manager's snapshot, so this screen is a view of the Wi-Fi state
//  instead of a form that is filled in and saved. All actions are sent directly to the manager.
//

#ifndef ES_APP_GUIS_GUI_WIFI_SETTINGS_H
#define ES_APP_GUIS_GUI_WIFI_SETTINGS_H

#include "guis/GuiSettings.h"
#include "wifi/WifiManager.h"

#include <cstdint>
#include <string>
#include <vector>

class GuiWifiSettings : public GuiSettings
{
public:
    GuiWifiSettings();
    ~GuiWifiSettings() override;

    void update(int deltaTime) override;

private:
    // Rebuilds every row from the current snapshot, keeping the cursor on the same network.
    void rebuild();

    // The CONNECT, ENTER PASSWORD, DISCONNECT and FORGET screen of one network.
    void openNetworkActions(const std::string& ssid);

    // Always opens with an empty value, so a stored passphrase is never shown again.
    void openPasswordPrompt(const std::string& ssid);

    static std::string networkLabel(const WifiNetwork& network);
    static std::string stateLabel(const WifiNetwork& network);
    static std::string securityLabel(const WifiSecurity security);
    static std::string summaryLabel(const WifiSnapshot& snapshot);

    // The parts of the snapshot that have to reach the screen without waiting for the throttle:
    // the radio and interface state, the scan state, and what every visible network is doing.
    static std::string immediateState(const WifiSnapshot& snapshot);

    // Rebuilding at most twice a second keeps the signal strength figures usable without moving
    // the list under the cursor all the time.
    static constexpr int sRebuildInterval {500};

    std::uint64_t mSequence {0};
    std::uint64_t mErrorSequence {0};
    std::string mLastError;
    std::string mImmediateState;
    int mRebuildElapsed {0};
    // The SSID behind each row, empty for the power, status and scan rows. Used to keep the cursor
    // on the same network when the rows above it change.
    std::vector<std::string> mRowSsids;
};

#endif // ES_APP_GUIS_GUI_WIFI_SETTINGS_H
