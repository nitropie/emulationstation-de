//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileMode.cpp
//
//  Profile mode state for the current process.
//

#include "profiles/ProfileMode.h"

#include "Log.h"

#include <cstdlib>
#include <optional>

namespace
{
    bool sMode {false};
    bool sResolved {false};
    std::string sError;
    ProfileSession::Context sSession;
    std::optional<ProfileAvatars::Manifest> sManifest;
    std::unique_ptr<ProfileMetadata> sMetadata;

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
