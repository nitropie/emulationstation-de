//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  WifiManager.cpp
//
//  Selects the Wi-Fi network management implementation and provides the null implementation that
//  is used on every platform without a backend.
//

#include "wifi/WifiManager.h"

#if defined(WIFI_MANAGER_WPASUPPLICANT)
// Defined by WifiManagerWpaSupplicant.cpp.
std::unique_ptr<WifiManager> createWpaSupplicantWifiManager();
#endif

namespace
{
    // Reports the service as unavailable and ignores all actions. This is what every platform
    // without a backend, and every build without WIFI_MANAGER, gets.
    class NullWifiManager : public WifiManager
    {
    public:
        NullWifiManager()
            : mSnapshot {std::make_shared<WifiSnapshot>()}
        {
        }

        bool isAvailable() const override { return false; }
        std::shared_ptr<const WifiSnapshot> getSnapshot() const override { return mSnapshot; }
        std::uint64_t getSequence() const override { return mSnapshot->sequence; }

        void setPowered(const bool) override {}
        void startScan() override {}
        void connect(const std::string&) override {}
        void connectWithPassword(const std::string&, const std::string&) override {}
        void disconnect() override {}
        void forget(const std::string&) override {}

    private:
        std::shared_ptr<const WifiSnapshot> mSnapshot;
    };

    // The backend is chosen here, at build time.
    std::unique_ptr<WifiManager> createWifiManager()
    {
#if defined(WIFI_MANAGER_WPASUPPLICANT)
        return createWpaSupplicantWifiManager();
#else
        return std::make_unique<NullWifiManager>();
#endif
    }

} // namespace

WifiManager& WifiManager::getInstance()
{
    static const std::unique_ptr<WifiManager> instance {createWifiManager()};
    return *instance;
}
