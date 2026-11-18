//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileMode.cpp
//
//  Profile mode state for the current process.
//

#include "profiles/ProfileMode.h"

#include "Log.h"
#include "utils/FileSystemUtil.h"

#include <cstdlib>
#include <map>
#include <optional>

namespace
{
    bool sMode {false};
    bool sResolved {false};
    std::string sError;
    ProfileSession::Context sSession;
    std::optional<ProfileAvatars::Manifest> sManifest;
    std::unique_ptr<ProfileMetadata> sMetadata;
    // One store per system, in the order of the first request.
    std::map<std::string, std::unique_ptr<ProfileState::JsonStore>> sStores;

    void logProblem(const std::string& problem)
    {
        LOG(LogError) << "Profile mode: " << problem;
    }

    bool loadManifest()
    {
        sManifest.reset();

        std::string error;
        std::optional<ProfileAvatars::Manifest> manifest {ProfileAvatars::Manifest::load(
            sSession.avatarManifestPath(), ProfileState::systemFileOperations(), error)};
        if (!manifest.has_value()) {
            sError = error;
            logProblem(error);
            return false;
        }

        sManifest = std::move(manifest);
        return true;
    }
} // namespace

void ProfileMode::initialize()
{
    sMode = false;
    sResolved = false;
    sError.clear();
    sManifest.reset();
    sMetadata.reset();
    sStores.clear();

    const char* catalog {getenv("NITROPIE_PROFILE_CATALOG")};
    const char* id {getenv("NITROPIE_PROFILE_ID")};
    const bool hasCatalog {catalog != nullptr && *catalog != '\0'};
    const bool hasId {id != nullptr && *id != '\0'};
    if (!hasCatalog && !hasId)
        return;

    sMode = true;

    sSession = ProfileSession::fromEnvironment(ProfileState::systemFileOperations());
    if (!sSession.active()) {
        sError = sSession.error().empty() ? "the profile session is unresolved"
                                          : sSession.error();
        logProblem(sError);
        return;
    }

    if (!loadManifest())
        return;

    sMetadata = std::make_unique<ProfileMetadata>(sSession.metadataPath(),
                                                  sSession.profileId(),
                                                  ProfileState::systemFileOperations());
    if (!sMetadata->isResolved()) {
        sError = sMetadata->loadErrors().empty() ? "the profile metadata is unresolved"
                                                 : sMetadata->loadErrors().front();
        logProblem(sError);
        return;
    }

    sResolved = true;
}

bool ProfileMode::isMode()
{
    return sMode;
}

bool ProfileMode::isResolved()
{
    return sResolved;
}

const std::string& ProfileMode::error()
{
    return sError;
}

const ProfileSession::Context& ProfileMode::session()
{
    return sSession;
}

ProfileMetadata* ProfileMode::metadata()
{
    return sMetadata.get();
}

ProfileAvatars::Manifest* ProfileMode::manifest()
{
    return sManifest.has_value() ? &*sManifest : nullptr;
}

ProfileState::JsonStore* ProfileMode::storeForSystem(const std::string& system,
                                                     const bool collectionSystem)
{
    if (!sMode || !sSession.active() || collectionSystem || system.empty())
        return nullptr;

    const std::map<std::string, std::unique_ptr<ProfileState::JsonStore>>::iterator existing {
        sStores.find(system)};
    if (existing != sStores.end())
        return existing->second.get();

    ProfileState::Session storeSession;
    storeSession.profileId = sSession.profileId();
    // The catalog carries H, E and S but not P. P is the parent of H in the layout the launcher
    // generates; the store itself only uses stateDirectory.
    storeSession.profileRoot = Utils::FileSystem::getParent(sSession.entry().home);
    storeSession.stateDirectory = sSession.entry().appdata + "/profile-state";
    storeSession.system = system;

    std::unique_ptr<ProfileState::JsonStore> store {
        std::make_unique<ProfileState::JsonStore>(storeSession,
                                                  ProfileState::systemFileOperations())};
    ProfileState::JsonStore* result {store.get()};
    sStores.emplace(system, std::move(store));
    return result;
}

bool ProfileMode::storesResolved()
{
    if (!sMode)
        return true;

    if (!sSession.active() || !sResolved)
        return false;

    for (const std::pair<const std::string, std::unique_ptr<ProfileState::JsonStore>>& entry :
         sStores) {
        if (!entry.second->isResolved())
            return false;
    }

    return true;
}

std::string ProfileMode::unresolvedReason()
{
    std::string reason;

    if (sMode && !sSession.active())
        return sSession.error().empty() ? "the profile session is unresolved" : sSession.error();

    if (sMode && !sResolved && !sError.empty())
        reason = sError;

    for (const std::pair<const std::string, std::unique_ptr<ProfileState::JsonStore>>& entry :
         sStores) {
        if (entry.second->isResolved())
            continue;

        if (!reason.empty())
            reason += "\n";
        reason += entry.first + ": ";
        const std::vector<std::string>& errors {entry.second->loadErrors()};
        reason += errors.empty() ? "the personal state is unresolved" : errors.front();
    }

    return reason;
}

bool ProfileMode::hasRecoverableStore()
{
    for (const std::pair<const std::string, std::unique_ptr<ProfileState::JsonStore>>& entry :
         sStores) {
        if (!entry.second->isResolved() && entry.second->hasUsableBackup())
            return true;
    }

    return false;
}

bool ProfileMode::recoverStores()
{
    for (std::pair<const std::string, std::unique_ptr<ProfileState::JsonStore>>& entry : sStores) {
        if (entry.second->isResolved())
            continue;

        if (entry.second->recoverFromBackup())
            LOG(LogInfo) << "Profile mode: recovered the personal state for \"" << entry.first
                         << "\" from its backup";
        else
            logProblem("could not recover the personal state for \"" + entry.first + "\"");
    }

    return storesResolved();
}

bool ProfileMode::reloadMetadata()
{
    if (!sSession.active())
        return false;

    sMetadata = std::make_unique<ProfileMetadata>(sSession.metadataPath(),
                                                  sSession.profileId(),
                                                  ProfileState::systemFileOperations());
    sResolved = sMetadata->isResolved() && sManifest.has_value();
    if (!sMetadata->isResolved()) {
        sError = sMetadata->loadErrors().empty() ? "the profile metadata is unresolved"
                                                 : sMetadata->loadErrors().front();
    }

    return sMetadata->isResolved();
}

bool ProfileMode::save(std::string& error)
{
    if (!sMetadata) {
        error = "the profile metadata is not loaded";
        return false;
    }

    const ProfileState::FlushResult result {sMetadata->flush()};
    if (result.status == ProfileState::FlushStatus::failure) {
        error = result.reason;
        return false;
    }

    error.clear();
    return true;
}
