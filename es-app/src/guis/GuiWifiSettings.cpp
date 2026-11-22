//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GuiWifiSettings.cpp
//
//  Wi-Fi network management screen.
//

#include "guis/GuiWifiSettings.h"

#include "Settings.h"
#include "components/ComponentList.h"
#include "components/SwitchComponent.h"
#include "components/TextComponent.h"
#include "guis/GuiMsgBox.h"
#include "guis/GuiTextEditKeyboardPopup.h"
#include "guis/GuiTextEditPopup.h"
#include "resources/Font.h"
#include "utils/StringUtil.h"

#include <algorithm>
#include <functional>

namespace
{
    bool isConnectingState(const std::string& state)
    {
        return state == "authenticating" || state == "associating" || state == "associated" ||
               state == "4way_handshake" || state == "group_handshake";
    }
} // namespace

GuiWifiSettings::GuiWifiSettings()
    : GuiSettings {_("WI-FI")}
{
    WifiManager& manager {WifiManager::getInstance()};

    // An error that predates the screen belongs to an earlier attempt. Seeding the mirror keeps
    // update() from showing it again when the screen is opened.
    const std::shared_ptr<const WifiSnapshot> snapshot {manager.getSnapshot()};
    if (snapshot != nullptr) {
        mLastError = snapshot->lastError;
        mErrorSequence = snapshot->errorSequence;
        if (snapshot->available && snapshot->powered && !snapshot->scanning)
            manager.startScan();
    }

    rebuild();
}

GuiWifiSettings::~GuiWifiSettings() = default;

void GuiWifiSettings::update(int deltaTime)
{
    GuiSettings::update(deltaTime);

    WifiManager& manager {WifiManager::getInstance()};
    const std::shared_ptr<const WifiSnapshot> snapshot {manager.getSnapshot()};
    if (snapshot == nullptr)
        return;

    // A failure is shown once per publication. The sequence also moves when the text repeats, so
    // a second identical rejection opens a second dialog.
    if (snapshot->errorSequence != mErrorSequence) {
        mErrorSequence = snapshot->errorSequence;
        mLastError = snapshot->lastError;
        if (!mLastError.empty())
            mWindow->pushGui(new GuiMsgBox(Utils::String::toUpper(mLastError)));
    }

    if (snapshot->sequence == mSequence)
        return;

    mRebuildElapsed += deltaTime;

    const std::string immediate {immediateState(*snapshot)};
    if (immediate != mImmediateState || mRebuildElapsed >= sRebuildInterval) {
        mSequence = snapshot->sequence;
        mRebuildElapsed = 0;
        rebuild();
    }
}

void GuiWifiSettings::rebuild()
{
    const std::shared_ptr<const WifiSnapshot> snapshot {WifiManager::getInstance().getSnapshot()};

    const std::shared_ptr<ComponentList> list {getMenu().getList()};
    const int previousCursor {list->getCursorId()};
    const std::string cursorSsid {
        (previousCursor >= 0 && previousCursor < static_cast<int>(mRowSsids.size())) ?
            mRowSsids.at(previousCursor) :
            std::string {}};

    list->clear();
    mRowSsids.clear();

    auto addRow = [this](const ComponentListRow& row, const std::string& ssid) {
        mRowSsids.push_back(ssid);
        getMenu().addRow(row);
    };

    mImmediateState = snapshot == nullptr ? std::string {} : immediateState(*snapshot);

    if (snapshot == nullptr || !snapshot->available) {
        ComponentListRow row;
        row.addElement(std::make_shared<TextComponent>(_("WI-FI IS NOT AVAILABLE"),
                                                       Font::get(FONT_SIZE_MEDIUM),
                                                       mMenuColorSecondary, ALIGN_LEFT),
                       true);
        addRow(row, std::string {});
        return;
    }

    auto powerSwitch {std::make_shared<SwitchComponent>(snapshot->powered)};
    SwitchComponent* switchPointer {powerSwitch.get()};
    powerSwitch->setCallback(
        [switchPointer] { WifiManager::getInstance().setPowered(switchPointer->getState()); });

    ComponentListRow powerRow;
    powerRow.addElement(std::make_shared<TextComponent>(_("POWER"), Font::get(FONT_SIZE_MEDIUM),
                                                        mMenuColorPrimary, ALIGN_LEFT),
                        true);
    powerRow.addElement(powerSwitch, false);
    addRow(powerRow, std::string {});

    ComponentListRow statusRow;
    statusRow.addElement(std::make_shared<TextComponent>(summaryLabel(*snapshot),
                                                         Font::get(FONT_SIZE_SMALL),
                                                         mMenuColorSecondary, ALIGN_LEFT),
                         true);
    addRow(statusRow, std::string {});

    if (snapshot->powered) {
        ComponentListRow scanRow;
        scanRow.addElement(std::make_shared<TextComponent>(
                               snapshot->scanning ? _("SCANNING...") : _("SCAN FOR NETWORKS"),
                               Font::get(FONT_SIZE_MEDIUM), mMenuColorPrimary, ALIGN_LEFT),
                           true);

        if (snapshot->scanning) {
            scanRow.addElement(std::make_shared<TextComponent>(
                                   std::to_string(snapshot->networks.size()),
                                   Font::get(FONT_SIZE_SMALL), mMenuColorSecondary, ALIGN_RIGHT),
                               false);
        }

        scanRow.makeAcceptInputHandler([this] {
            WifiManager& manager {WifiManager::getInstance()};
            const std::shared_ptr<const WifiSnapshot> current {manager.getSnapshot()};
            if (current != nullptr && !current->scanning)
                manager.startScan();
        });
        addRow(scanRow, std::string {});

        // The network rows belong to the powered screen. A cached BSS stays in the snapshot after
        // the radio is switched off, and showing it would offer a network that cannot be joined.
        for (const WifiNetwork& network : snapshot->networks) {
            ComponentListRow row;

            auto name {std::make_shared<TextComponent>(
                networkLabel(network), Font::get(FONT_SIZE_MEDIUM), mMenuColorPrimary, ALIGN_LEFT)};
            name->setHorizontalScrolling(true);
            row.addElement(name, true);
            row.addElement(std::make_shared<TextComponent>(stateLabel(network),
                                                           Font::get(FONT_SIZE_SMALL),
                                                           mMenuColorSecondary, ALIGN_RIGHT),
                           false);

            const std::string ssid {network.ssid};
            row.makeAcceptInputHandler([this, ssid] { openNetworkActions(ssid); });
            addRow(row, ssid);
        }
    }

    const int rows {static_cast<int>(mRowSsids.size())};
    if (rows == 0)
        return;

    int target {std::clamp(previousCursor, 0, rows - 1)};

    if (!cursorSsid.empty()) {
        const auto found {std::find(mRowSsids.cbegin(), mRowSsids.cend(), cursorSsid)};
        if (found != mRowSsids.cend())
            target = static_cast<int>(found - mRowSsids.cbegin());
    }

    if (target != list->getCursorId())
        list->moveCursor(target - list->getCursorId());
}

void GuiWifiSettings::openNetworkActions(const std::string& ssid)
{
    const std::shared_ptr<const WifiSnapshot> snapshot {WifiManager::getInstance().getSnapshot()};

    // The rows are not rendered while the radio is off, but a queued accept event could still
    // arrive after a power change. Nothing may be configured in that state.
    if (snapshot == nullptr || !snapshot->powered)
        return;

    const WifiNetwork* network {nullptr};
    for (const WifiNetwork& candidate : snapshot->networks) {
        if (candidate.ssid == ssid) {
            network = &candidate;
            break;
        }
    }

    if (network == nullptr)
        return;

    auto settings = new GuiSettings(_("WI-FI NETWORK"));

    ComponentListRow header;
    header.addElement(std::make_shared<TextComponent>(Utils::String::toUpper(ssid),
                                                      Font::get(FONT_SIZE_MEDIUM),
                                                      mMenuColorPrimary, ALIGN_LEFT),
                      true);
    std::string security {securityLabel(network->security)};
    if (network->visible)
        security += "  " + std::to_string(network->signal) + "%";
    header.addElement(std::make_shared<TextComponent>(security, Font::get(FONT_SIZE_SMALL),
                                                      mMenuColorSecondary, ALIGN_RIGHT),
                      false);
    settings->addRow(header);

    const bool busy {network->pending != WifiOp::none};

    auto addAction = [settings](const std::string& label, const bool enabled,
                                const std::function<void()>& action) {
        ComponentListRow row;
        auto text {std::make_shared<TextComponent>(label, Font::get(FONT_SIZE_MEDIUM),
                                                   mMenuColorPrimary, ALIGN_LEFT)};

        if (!enabled) {
            text->setOpacity(0.5f);
        }
        else {
            row.makeAcceptInputHandler([action] { action(); });
        }

        row.addElement(text, true);
        settings->addRow(row);
    };

    const WifiNetwork selected {*network};

    // CONNECT joins a network that already has credentials, and asks for a password for an
    // unsaved personal one. The submenu is removed before the prompt is pushed so the prompt is
    // never deleted with it.
    addAction(_("CONNECT"),
              !busy && !selected.connected && selected.visible &&
                  selected.security != WifiSecurity::enterprise,
              [this, settings, selected] {
                  // The action runs before the submenu is deleted: the handler closure keeps its
                  // captures in the row, so they must not be read after the row is gone.
                  if (selected.security == WifiSecurity::personal && !selected.saved)
                      openPasswordPrompt(selected.ssid);
                  else
                      WifiManager::getInstance().connect(selected.ssid);
                  delete settings;
              });

    addAction(_("ENTER PASSWORD"),
              !busy && selected.visible && selected.security == WifiSecurity::personal,
              [this, settings, ssid] {
                  openPasswordPrompt(ssid);
                  delete settings;
              });

    addAction(_("DISCONNECT"), !busy && selected.connected, [settings] {
        WifiManager::getInstance().disconnect();
        delete settings;
    });

    addAction(_("FORGET"), !busy && (selected.saved || selected.connected), [settings, ssid] {
        WifiManager::getInstance().forget(ssid);
        delete settings;
    });

    mWindow->pushGui(settings);
}

void GuiWifiSettings::openPasswordPrompt(const std::string& ssid)
{
    // The password is always entered from scratch, and only the callback copies it to the manager
    // command queue for as long as the attempt needs it.
    auto connect = [ssid](const std::string& passphrase) {
        if (!passphrase.empty())
            WifiManager::getInstance().connectWithPassword(ssid, passphrase);
    };

    if (Settings::getInstance()->getBool("VirtualKeyboard")) {
        mWindow->pushGui(new GuiTextEditKeyboardPopup(getMenu().getPosition().y,
                                                      _("WI-FI PASSWORD"), "", connect, false,
                                                      _("CONNECT"), _("CONNECT TO WI-FI?")));
    }
    else {
        mWindow->pushGui(new GuiTextEditPopup(_("WI-FI PASSWORD"), "", connect, false));
    }
}

std::string GuiWifiSettings::networkLabel(const WifiNetwork& network)
{
    return Utils::String::toUpper(network.ssid);
}

std::string GuiWifiSettings::stateLabel(const WifiNetwork& network)
{
    std::string state;

    switch (network.pending) {
        case WifiOp::connecting:
            state = _("CONNECTING");
            break;
        case WifiOp::disconnecting:
            state = _("DISCONNECTING");
            break;
        case WifiOp::forgetting:
            state = _("REMOVING");
            break;
        case WifiOp::none:
            if (network.connected)
                state = _("CONNECTED");
            else if (network.saved)
                state = _("SAVED");
            else if (network.security == WifiSecurity::open)
                state = _("OPEN");
            else if (network.security == WifiSecurity::enterprise)
                state = _("ENTERPRISE");
            else
                state = _("SECURED");
            break;
    }

    if (network.visible)
        state += "  " + std::to_string(network.signal) + "%";

    return state;
}

std::string GuiWifiSettings::securityLabel(const WifiSecurity security)
{
    switch (security) {
        case WifiSecurity::open:
            return _("OPEN");
        case WifiSecurity::personal:
            return _("WPA/WPA2/WPA3");
        case WifiSecurity::enterprise:
            return _("ENTERPRISE");
    }

    return std::string {};
}

std::string GuiWifiSettings::summaryLabel(const WifiSnapshot& snapshot)
{
    const std::string interface {snapshot.interfaceName};

    if (!snapshot.powered)
        return interface +
               (snapshot.hardBlocked ? " (" + _("HARD BLOCKED") + ")" : " (" + _("OFF") + ")");

    // While a switch is in progress the daemon may already be disconnected, so the pending
    // target is the only useful summary. Without this the line flickers to NOT CONNECTED.
    for (const WifiNetwork& network : snapshot.networks) {
        if (network.pending == WifiOp::connecting)
            return interface + " — " + _("CONNECTING TO") + " " + network.ssid;
    }

    if (snapshot.connected) {
        std::string summary {interface + " — " + _("CONNECTED TO") + " " + snapshot.activeSsid};
        if (!snapshot.ipAddress.empty())
            summary += " (" + snapshot.ipAddress + ")";
        return summary;
    }

    if (isConnectingState(snapshot.state)) {
        if (snapshot.activeSsid.empty())
            return interface + " — " + _("CONNECTING");
        return interface + " — " + _("CONNECTING TO") + " " + snapshot.activeSsid;
    }

    return interface + " — " + _("NOT CONNECTED");
}

std::string GuiWifiSettings::immediateState(const WifiSnapshot& snapshot)
{
    std::string state;
    state += snapshot.available ? '1' : '0';
    state += snapshot.powered ? '1' : '0';
    state += snapshot.hardBlocked ? '1' : '0';
    state += snapshot.scanning ? '1' : '0';
    state += snapshot.connected ? '1' : '0';
    state += snapshot.state;
    state += snapshot.activeSsid;
    state += snapshot.ipAddress;

    // A network that changes what it is doing, or that becomes saved or connected, has to show up
    // at once. The signal strength can wait for the next rebuild.
    for (const WifiNetwork& network : snapshot.networks) {
        state += network.ssid;
        state += static_cast<char>('0' + static_cast<int>(network.pending));
        state += network.connected ? '1' : '0';
        state += network.saved ? '1' : '0';
    }

    return state;
}
