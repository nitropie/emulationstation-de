//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  SystemStatusEthernetTests.cpp
//
//  Test cases for the wired Ethernet detection over a synthetic /sys/class/net tree, and for the
//  bundled Ethernet status icon.
//

#include "TestHarness.h"

#include "SystemStatus.h"

#include "lunasvg.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace
{
    // A throwaway /sys/class/net replacement. Every interface is a directory and every sysfs
    // attribute is a file with a trailing newline, exactly like the kernel exposes them.
    class SysfsTree
    {
    public:
        SysfsTree()
        {
            // The name only has to be unique among the test processes that run at the same time.
            // A steady clock reading and a process-local counter are both portable, and the
            // create call settles the rare case of two processes picking the same name.
            static std::atomic<std::uint64_t> counter {0};

            std::error_code error;
            for (int attempt {0}; attempt < 100; ++attempt) {
                const auto stamp {std::chrono::steady_clock::now().time_since_epoch().count()};
                mRoot = std::filesystem::temp_directory_path() /
                        ("es-de-status-" + std::to_string(stamp) + "-" +
                         std::to_string(counter.fetch_add(1)));
                if (std::filesystem::create_directories(mRoot, error))
                    break;
            }
        }

        ~SysfsTree()
        {
            std::error_code error;
            std::filesystem::remove_all(mRoot, error);
        }

        void interface(const std::string& name)
        {
            std::filesystem::create_directories(mRoot / name);
        }

        void directory(const std::string& name, const std::string& child)
        {
            std::filesystem::create_directories(mRoot / name / child);
        }

        void file(const std::string& name, const std::string& child, const std::string& value)
        {
            std::ofstream output {mRoot / name / child};
            output << value;
        }

        std::string path() const { return mRoot.string(); }

    private:
        std::filesystem::path mRoot;
    };

    void physicalEthernet(SysfsTree& tree,
                          const std::string& name,
                          const std::string& operstate,
                          const std::string& carrier = "1")
    {
        tree.interface(name);
        tree.directory(name, "device");
        tree.file(name, "type", "1\n");
        tree.file(name, "operstate", operstate + "\n");
        tree.file(name, "carrier", carrier + "\n");
    }
} // namespace

TEST_CASE(physicalEthernetLinkUpIsDetected)
{
    SysfsTree tree;
    physicalEthernet(tree, "eth0", "up");

    bool hasWifi {true};
    bool hasEthernet {false};
    SystemStatus::probeLinuxNetworkInterfaces(tree.path(), hasWifi, hasEthernet);

    EXPECT(!hasWifi);
    EXPECT(hasEthernet);
}

TEST_CASE(physicalEthernetLinkDownIsNotDetected)
{
    SysfsTree tree;
    physicalEthernet(tree, "eth0", "down", "1");

    bool hasWifi {true};
    bool hasEthernet {true};
    SystemStatus::probeLinuxNetworkInterfaces(tree.path(), hasWifi, hasEthernet);

    EXPECT(!hasWifi);
    EXPECT(!hasEthernet);
}

TEST_CASE(unknownOperstateFallsBackToCarrier)
{
    SysfsTree up;
    physicalEthernet(up, "eth0", "unknown", "1");

    bool hasWifi {true};
    bool hasEthernet {false};
    SystemStatus::probeLinuxNetworkInterfaces(up.path(), hasWifi, hasEthernet);
    EXPECT(hasEthernet);

    SysfsTree down;
    physicalEthernet(down, "eth0", "unknown", "0");

    hasEthernet = true;
    SystemStatus::probeLinuxNetworkInterfaces(down.path(), hasWifi, hasEthernet);
    EXPECT(!hasEthernet);
}

TEST_CASE(wirelessInterfaceIsDetected)
{
    SysfsTree wireless;
    wireless.interface("wlan0");
    wireless.directory("wlan0", "wireless");
    wireless.file("wlan0", "operstate", "up\n");

    bool hasWifi {false};
    bool hasEthernet {true};
    SystemStatus::probeLinuxNetworkInterfaces(wireless.path(), hasWifi, hasEthernet);
    EXPECT(hasWifi);
    EXPECT(!hasEthernet);

    SysfsTree phy;
    phy.interface("wlp2s0");
    phy.directory("wlp2s0", "phy80211");
    phy.file("wlp2s0", "operstate", "up\n");

    hasWifi = false;
    hasEthernet = true;
    SystemStatus::probeLinuxNetworkInterfaces(phy.path(), hasWifi, hasEthernet);
    EXPECT(hasWifi);
    EXPECT(!hasEthernet);
}

TEST_CASE(virtualAndNonEthernetInterfacesAreIgnored)
{
    SysfsTree tree;

    // Loopback has no device node.
    tree.interface("lo");
    tree.file("lo", "type", "772\n");
    tree.file("lo", "operstate", "unknown\n");

    // Bridges and bonds are virtual even though they have a device node in some setups.
    tree.interface("docker0");
    tree.directory("docker0", "device");
    tree.directory("docker0", "bridge");
    tree.file("docker0", "type", "1\n");
    tree.file("docker0", "operstate", "up\n");

    tree.interface("br0");
    tree.directory("br0", "device");
    tree.directory("br0", "bridge");
    tree.file("br0", "type", "1\n");
    tree.file("br0", "operstate", "up\n");

    tree.interface("bond0");
    tree.directory("bond0", "device");
    tree.directory("bond0", "bonding");
    tree.file("bond0", "type", "1\n");
    tree.file("bond0", "operstate", "up\n");

    // A tunnel is marked by tun_flags.
    tree.interface("tun0");
    tree.directory("tun0", "device");
    tree.directory("tun0", "tun_flags");
    tree.file("tun0", "type", "1\n");
    tree.file("tun0", "operstate", "up\n");

    // Bluetooth PAN interfaces are excluded by name.
    physicalEthernet(tree, "bnep0", "up");

    // A dummy interface has no device node.
    tree.interface("dummy0");
    tree.file("dummy0", "type", "1\n");
    tree.file("dummy0", "operstate", "up\n");

    // A non-Ethernet hardware type is never wired Ethernet.
    tree.interface("eth1");
    tree.directory("eth1", "device");
    tree.file("eth1", "type", "772\n");
    tree.file("eth1", "operstate", "up\n");

    bool hasWifi {true};
    bool hasEthernet {true};
    SystemStatus::probeLinuxNetworkInterfaces(tree.path(), hasWifi, hasEthernet);

    EXPECT(!hasWifi);
    EXPECT(!hasEthernet);
}

TEST_CASE(ethernetIconIsBundledAndLoadable)
{
    const std::string path {std::string {ES_CORE_SOURCE_DIR} +
                            "/../resources/graphics/systemstatus/ethernet.svg"};

    std::ifstream input {path};
    EXPECT(input.good());
    if (!input.good())
        return;

    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string svg {contents.str()};

    EXPECT(svg.find("viewBox=\"0 0 256 256\"") != std::string::npos);
    EXPECT(svg.find("stroke-width=\"16\"") != std::string::npos);
    EXPECT(svg.find("#ffffff") != std::string::npos);

    const std::unique_ptr<lunasvg::Document> document {lunasvg::Document::loadFromData(svg)};
    EXPECT(document != nullptr);
}
