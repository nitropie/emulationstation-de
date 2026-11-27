//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileSwitch.cpp
//

#include "profiles/ProfileSwitch.h"

#include "CollectionSystemsManager.h"
#include "Log.h"
#include "Settings.h"
#include "SystemData.h"
#include "Window.h"
#include "profiles/ProfileMode.h"
#include "utils/PlatformUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>

#if !defined(_WIN64)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* sRequestSchema {"nitropie.profile-switch-request"};
    constexpr int sRequestVersion {1};
    constexpr const char* sProfileMode {"profile"};
    constexpr const char* sChooserMode {"chooser"};
    constexpr const char* sChooserId {"chooser"};

    bool sInitialized {false};
    bool sEnvironmentPresent {false};
    bool sEnabledResolved {false};
    bool sEnabled {false};
    bool sChooser {false};
    bool sReadyAcknowledged {false};
    bool sRequestWritten {false};
    int sReadyFd {-1};
    std::string sMode;
    std::string sNonce;
    std::string sRequestPath;
    std::string sDisabledReason;
    std::string sRequestIdOverride;
    std::function<ProfileSwitch::FlushReport()> sFlushProvider;

    ProfileState::FileOperations* sFiles {nullptr};
    std::function<void()> sQuitHandler;
    std::vector<std::pair<std::string, std::function<bool()>>> sBlockers;

    const char* envOrEmpty(const char* name)
    {
        const char* value {std::getenv(name)};
        return value != nullptr ? value : "";
    }

    bool isLowerHex(const std::string& value, const std::size_t length)
    {
        if (value.size() != length)
            return false;
        return std::all_of(value.cbegin(), value.cend(), [](const char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    }

    // Canonical lowercase UUIDv4, the same shape the supervisor accepts.
    bool isUuidV4(const std::string& value)
    {
        if (value.size() != 36)
            return false;
        for (std::size_t index {0}; index < value.size(); ++index) {
            const char c {value[index]};
            if (index == 8 || index == 13 || index == 18 || index == 23) {
                if (c != '-')
                    return false;
            }
            else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
        return value[14] == '4' && (value[19] == '8' || value[19] == '9' || value[19] == 'a' ||
                                    value[19] == 'b');
    }

    std::string randomHex(const std::size_t bytes)
    {
        std::random_device device;
        static const char* digits {"0123456789abcdef"};
        std::string out;
        out.reserve(bytes * 2);
        for (std::size_t index {0}; index < bytes; ++index) {
            const unsigned int value {device() & 0xffU};
            out.push_back(digits[value >> 4]);
            out.push_back(digits[value & 0x0fU]);
        }
        return out;
    }

    ProfileState::FileOperations& files()
    {
        return sFiles != nullptr ? *sFiles : ProfileState::systemFileOperations();
    }

    const std::string& activeProfileId()
    {
        static const std::string empty;
        if (!ProfileMode::isResolved())
            return empty;
        return ProfileMode::session().profileId();
    }

    // The order is sorted so the bytes are canonical: the supervisor's strict parser and the
    // golden fixtures compare exact text.
    std::string buildRequest(const std::string& sourceId,
                             const std::string& targetId,
                             const ProfileSwitch::FlushReport& report)
    {
        const std::string requestId {sRequestIdOverride.empty() ? randomHex(16) :
                                                                    sRequestIdOverride};
        std::string out {"{"};
        out += "\"flush\":{\"collections\":\"" + report.collections + "\",";
        out += "\"metadata\":\"" + report.metadata + "\",";
        out += "\"overlays\":\"" + report.overlays + "\",";
        out += "\"settings\":\"" + report.settings + "\"},";
        out += "\"idle\":true,";
        out += "\"requestId\":\"" + requestId + "\",";
        out += "\"schema\":\"" + std::string(sRequestSchema) + "\",";
        out += "\"sessionNonce\":\"" + sNonce + "\",";
        out += "\"sourceProfileId\":\"" + sourceId + "\",";
        out += "\"targetProfileId\":\"" + targetId + "\",";
        out += "\"version\":" + std::to_string(sRequestVersion);
        out += "}\n";
        return out;
    }

    bool writeRequestAtomically(const std::string& bytes, std::string& error)
    {
        ProfileState::FileOperations& ops {files()};
        const std::string request {sRequestPath};
        const std::string temporary {request + ".tmp"};
        std::error_code ec;
        fs::path parent {fs::path(request).parent_path()};
        if (parent.empty())
            parent = ".";

        // A leftover temp file from a crashed writer must never be reused: writeNewFile is
        // O_EXCL, so remove it first and let the create fail if anything races us.
        ops.removeFile(temporary);

        if (!ops.writeNewFile(temporary, bytes)) {
            error = "cannot create the switch request temp file";
            return false;
        }
        if (!ops.syncFile(temporary)) {
            ops.removeFile(temporary);
            error = "cannot sync the switch request temp file";
            return false;
        }
        if (!ops.renameFile(temporary, request)) {
            ops.removeFile(temporary);
            error = "cannot publish the switch request";
            return false;
        }
        if (!ops.syncDirectory(parent.string())) {
            // The rename is visible but not durable, so treat it as a failure and remove it.
            ops.removeFile(request);
            error = "cannot sync the session directory";
            return false;
        }
        return true;
    }

    bool sChooserCatalogLoaded {false};
    bool sChooserCatalogValid {false};
    ProfileSession::Catalog sChooserCatalog;

    // The chooser has no profile identity, so it reads the catalog file the launcher named.
    const ProfileSession::Catalog* chooserCatalog()
    {
        if (sChooserCatalogLoaded)
            return sChooserCatalogValid ? &sChooserCatalog : nullptr;
        sChooserCatalogLoaded = true;

        const char* path {std::getenv("NITROPIE_PROFILE_CATALOG")};
        if (path == nullptr || *path == '\0')
            return nullptr;
        std::string bytes;
        if (files().readFile(path, bytes) != ProfileState::ReadStatus::ok)
            return nullptr;
        std::string error;
        sChooserCatalogValid = ProfileSession::readCatalog(bytes, sChooserCatalog, error);
        if (!sChooserCatalogValid)
            LOG(LogWarning) << "ProfileSwitch: " << error;
        return sChooserCatalogValid ? &sChooserCatalog : nullptr;
    }

    bool catalogHasProfile(const std::string& targetId)
    {
        const ProfileSession::Catalog* catalog {nullptr};
        if (ProfileMode::isResolved())
            catalog = &ProfileMode::session().catalog();
        else if (sChooser)
            catalog = chooserCatalog();

        if (catalog == nullptr)
            return false;
        return std::any_of(catalog->profiles.cbegin(), catalog->profiles.cend(),
                           [&targetId](const ProfileSession::CatalogEntry& entry) {
                               return entry.id == targetId;
                           });
    }

    void installDefaultBlockers()
    {
        if (!sBlockers.empty())
            return;

        // es-core/src/Window.h:getGameLaunchedState() - a game launched with RunInBackground.
        sBlockers.emplace_back("game-launched", []() {
            Window* window {Window::getInstance()};
            return window != nullptr &&
                   (window->getGameLaunchedState() || window->getGameLaunched() != nullptr);
        });

        // es-core/src/Window.h:getGuiStackSize() - any dialog, the scraper search and single
        // scrape GUIs, the metadata editor, the application updater and the theme downloader
        // are GuiComponents on the stack. A background thread without a GUI is not covered.
        sBlockers.emplace_back("modal-dialog", []() {
            Window* window {Window::getInstance()};
            return window != nullptr && window->getGuiStackSize() > 1;
        });

        // es-app/src/CollectionSystemsManager.h:isEditing() - custom collection edit mode.
        sBlockers.emplace_back("collection-edit", []() {
            CollectionSystemsManager* manager {CollectionSystemsManager::getInstance()};
            return manager != nullptr && manager->isEditing();
        });

        // es-core/src/utils/PlatformUtil.cpp:runSystemCommand() - detached background scripts.
        sBlockers.emplace_back("background-command",
                               []() { return Utils::Platform::runningCommands() > 0; });

        // es-app/src/profiles/ProfileMode.cpp:hasPendingWrites() - personal-state stores and M.
        sBlockers.emplace_back("pending-writes", []() { return ProfileMode::hasPendingWrites(); });
    }
} // namespace

namespace ProfileSwitch
{
    bool FlushReport::allOk() const
    {
        return overlays != "failure" && metadata != "failure" && settings != "failure" &&
               collections != "failure";
    }

    void initialize()
    {
        if (sInitialized)
            return;
        sInitialized = true;

        sMode = envOrEmpty("NITROPIE_SESSION_MODE");
        sNonce = envOrEmpty("NITROPIE_SESSION_NONCE");
        sRequestPath = envOrEmpty("NITROPIE_SWITCH_REQUEST");
        const std::string readyFd {envOrEmpty("NITROPIE_READY_FD")};
        const std::string sessionRoot {envOrEmpty("NITROPIE_SESSION_ROOT")};
        const std::string notice {envOrEmpty("NITROPIE_SESSION_NOTICE")};

        // Close-on-exec first, before anything in this process can spawn an emulator.
        if (!readyFd.empty()) {
            try {
                sReadyFd = std::stoi(readyFd);
            }
            catch (...) {
                sReadyFd = -1;
            }
        }
#if !defined(_WIN64)
        if (sReadyFd >= 0) {
            const int flags {fcntl(sReadyFd, F_GETFD)};
            if (flags < 0 || fcntl(sReadyFd, F_SETFD, flags | FD_CLOEXEC) < 0) {
                LOG(LogWarning) << "ProfileSwitch: cannot set FD_CLOEXEC on the readiness pipe";
                sReadyFd = -1;
            }
        }
#endif

        sEnvironmentPresent = !sMode.empty();
        if (!sEnvironmentPresent) {
            sDisabledReason = "the switch environment is not present";
            return;
        }
        if (sMode != sProfileMode && sMode != sChooserMode) {
            sDisabledReason = "unknown session mode \"" + sMode + "\"";
            return;
        }
        sChooser = sMode == sChooserMode;
        if (!isLowerHex(sNonce, 32)) {
            sDisabledReason = "the session nonce is missing or not 128-bit hex";
            return;
        }
        if (sRequestPath.empty() || sRequestPath.front() != '/') {
            sDisabledReason = "the switch request path is missing or not absolute";
            return;
        }
        if (sReadyFd < 0) {
            sDisabledReason = "the readiness descriptor is missing or invalid";
            return;
        }
        if (sessionRoot.empty() || notice.size() > 4096) {
            // Not fatal on its own, but a session without a root cannot be switched safely.
            sDisabledReason = "the session root is missing";
            return;
        }
        installDefaultBlockers();
    }

    bool isEnabled()
    {
        if (!sInitialized)
            initialize();
        if (sEnabledResolved)
            return sEnabled;

        sEnabledResolved = true;
        if (!sEnvironmentPresent || !sDisabledReason.empty()) {
            sEnabled = false;
            return sEnabled;
        }
        if (sChooser) {
            sEnabled = true;
            return sEnabled;
        }
        if (!ProfileMode::isMode() || !ProfileMode::isResolved() ||
            activeProfileId().empty()) {
            sDisabledReason = "profile mode has no resolved session";
            sEnabled = false;
            return sEnabled;
        }
        sEnabled = true;
        return sEnabled;
    }

    bool isChooser()
    {
        return sChooser;
    }

    const std::string& disabledReason()
    {
        return sDisabledReason;
    }

    const std::string& nonce()
    {
        return sNonce;
    }

    const std::string& sourceId()
    {
        static const std::string chooser {sChooserId};
        return sChooser ? chooser : activeProfileId();
    }

    const std::string& requestPath()
    {
        return sRequestPath;
    }

    void acknowledgeReady()
    {
        if (sReadyAcknowledged || sReadyFd < 0)
            return;
        sReadyAcknowledged = true;

        const std::string expected {sChooser ? sChooserId : activeProfileId()};
        const std::string line {"READY " + sNonce + " " + expected + "\n"};
#if !defined(_WIN64)
        std::size_t written {0};
        while (written < line.size()) {
            const ssize_t result {::write(sReadyFd, line.data() + written, line.size() - written)};
            if (result <= 0) {
                LOG(LogWarning) << "ProfileSwitch: cannot write the readiness acknowledgement";
                break;
            }
            written += static_cast<std::size_t>(result);
        }
        close(sReadyFd);
        sReadyFd = -1;
#else
        (void)line;
#endif
    }

    bool isIdle(std::string& reason)
    {
        installDefaultBlockers();
        for (const auto& blocker : sBlockers) {
            if (blocker.second()) {
                reason = blocker.first;
                return false;
            }
        }
        return true;
    }

    void registerBlocker(const std::string& name, std::function<bool()> probe)
    {
        sBlockers.emplace_back(name, std::move(probe));
    }

    FlushReport flushAll()
    {
        FlushReport report;

        // overlays: capture every system's personal state, then persist every open store.
        SystemData::captureAllPersonalStates();
        const std::vector<std::pair<std::string, std::string>> failures {
            ProfileMode::flushStores()};
        if (failures.empty()) {
            report.overlays = "ok";
        }
        else {
            report.overlays = "failure";
            for (const auto& failure : failures)
                report.reason += failure.first + ": " + failure.second + "; ";
        }

        // metadata: M has its own pending flag, so an untouched file is nothingToDo.
        if (ProfileMode::metadata() == nullptr || !ProfileMode::isResolved()) {
            report.metadata = "nothingToDo";
        }
        else if (!ProfileMode::metadata()->hasPendingWrites()) {
            report.metadata = "nothingToDo";
        }
        else {
            std::string error;
            if (ProfileMode::save(error)) {
                report.metadata = "ok";
            }
            else {
                report.metadata = "failure";
                report.reason += "metadata: " + error + "; ";
            }
        }

        // settings: es_settings.xml is written on every call, so there is no staged state.
        if (Settings::getInstance()->saveFile()) {
            report.settings = "ok";
        }
        else {
            report.settings = "failure";
            report.reason += "settings: the file could not be written; ";
        }

        // collections: every custom collection file.
        CollectionSystemsManager* manager {CollectionSystemsManager::getInstance()};
        if (manager == nullptr || manager->getCustomCollectionSystems().empty()) {
            report.collections = "nothingToDo";
        }
        else {
            bool ok {true};
            for (const auto& entry : manager->getCustomCollectionSystems()) {
                if (!manager->saveCustomCollection(entry.second.system)) {
                    ok = false;
                    report.reason += "collection " + entry.first + "; ";
                }
            }
            report.collections = ok ? "ok" : "failure";
        }

        return report;
    }

    bool requestSwitch(const std::string& targetId, std::string& error)
    {
        if (!isEnabled()) {
            error = disabledReason();
            return false;
        }
        if (sChooser) {
            error = "a switch request is not available in chooser mode";
            return false;
        }
        if (!isUuidV4(targetId)) {
            error = "the target is not a canonical lowercase UUIDv4";
            return false;
        }
        if (!catalogHasProfile(targetId)) {
            error = "the target is not in the profile catalog";
            return false;
        }
        if (targetId == activeProfileId()) {
            error = "the target is already the active profile";
            return false;
        }
        std::string reason;
        if (!isIdle(reason)) {
            error = "the frontend is not idle: " + reason;
            return false;
        }

        const FlushReport report {sFlushProvider ? sFlushProvider() : flushAll()};
        if (!report.allOk()) {
            error = "the flush failed: " + report.reason;
            return false;
        }

        // Re-check idle: the flush above may have changed the world.
        if (!isIdle(reason)) {
            error = "the frontend stopped being idle: " + reason;
            return false;
        }

        const std::string bytes {buildRequest(activeProfileId(), targetId, report)};
        if (!writeRequestAtomically(bytes, error))
            return false;
        sRequestWritten = true;

        if (sQuitHandler)
            sQuitHandler();
        else
            Utils::Platform::quitES(Utils::Platform::QuitMode::SWITCH_PROFILE);
        return true;
    }

    bool requestSelection(const std::string& targetId, std::string& error)
    {
        if (!isEnabled()) {
            error = disabledReason();
            return false;
        }
        if (!sChooser) {
            error = "a selection request is only available in chooser mode";
            return false;
        }
        if (!isUuidV4(targetId)) {
            error = "the target is not a canonical lowercase UUIDv4";
            return false;
        }
        if (!catalogHasProfile(targetId)) {
            error = "the target is not in the profile catalog";
            return false;
        }

        FlushReport report;
        report.overlays = "nothingToDo";
        report.metadata = "nothingToDo";
        report.settings = "nothingToDo";
        report.collections = "nothingToDo";

        const std::string bytes {buildRequest(sChooserId, targetId, report)};
        if (!writeRequestAtomically(bytes, error))
            return false;
        sRequestWritten = true;

        if (sQuitHandler)
            sQuitHandler();
        else
            Utils::Platform::quitES(Utils::Platform::QuitMode::SWITCH_PROFILE);
        return true;
    }

    bool hasRequest()
    {
        if (sRequestPath.empty())
            return false;
        return fs::exists(fs::path(sRequestPath));
    }

    void discardRequest()
    {
        if (!sRequestPath.empty())
            files().removeFile(sRequestPath);
        sRequestWritten = false;
    }

    int finalizeExitCode(int platformExitCode, bool finalFlushOk)
    {
        if (platformExitCode == Utils::Platform::SWITCH_PROFILE_EXIT_CODE && !finalFlushOk) {
            discardRequest();
            return 1;
        }
        return platformExitCode;
    }

    void setFileOperationsForTesting(ProfileState::FileOperations& operations)
    {
        sFiles = &operations;
    }

    void setQuitHandlerForTesting(std::function<void()> handler)
    {
        sQuitHandler = std::move(handler);
    }

    void setRequestIdForTesting(const std::string& requestId)
    {
        sRequestIdOverride = requestId;
    }

    void setFlushReportForTesting(std::function<FlushReport()> provider)
    {
        sFlushProvider = std::move(provider);
    }

    void resetForTesting()
    {
        sInitialized = false;
        sEnvironmentPresent = false;
        sEnabledResolved = false;
        sEnabled = false;
        sChooser = false;
        sReadyAcknowledged = false;
        sRequestWritten = false;
        sReadyFd = -1;
        sMode.clear();
        sNonce.clear();
        sRequestPath.clear();
        sDisabledReason.clear();
        sRequestIdOverride.clear();
        sFlushProvider = nullptr;
        sFiles = nullptr;
        sQuitHandler = nullptr;
        sBlockers.clear();
        sChooserCatalogLoaded = false;
        sChooserCatalogValid = false;
        sChooserCatalog = ProfileSession::Catalog {};
    }
} // namespace ProfileSwitch
