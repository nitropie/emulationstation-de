//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileMode.h
//
//  Profile mode state for the current process: the session resolved from the launcher
//  environment, the profile metadata file M and the avatar manifest. Without the environment the
//  frontend runs exactly as before.
//

#ifndef ES_APP_PROFILES_PROFILE_MODE_H
#define ES_APP_PROFILES_PROFILE_MODE_H

#include "profiles/AvatarManifest.h"
#include "profiles/ProfileMetadata.h"
#include "profiles/ProfileSession.h"

#include <memory>
#include <string>

namespace ProfileMode
{
    // Reads the environment once at startup. Logs why a profile is unresolved.
    void initialize();

    // True when the profile environment was supplied, whether or not it resolved. The PROFILE
    // screen exists in this case.
    bool isMode();
    bool isResolved();
    const std::string& error();

    const ProfileSession::Context& session();
    ProfileMetadata* metadata();
    ProfileAvatars::Manifest* manifest();

    // Re-reads M from disk, for example after a failed write.
    bool reloadMetadata();
    // Flushes M. Returns false and sets `error` when the write failed.
    bool save(std::string& error);
} // namespace ProfileMode

#endif // ES_APP_PROFILES_PROFILE_MODE_H
