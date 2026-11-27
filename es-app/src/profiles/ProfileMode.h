//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileMode.h
//
//  Profile mode state for the current process: the session resolved from the launcher
//  environment, the profile metadata file M, the avatar manifest and the per-system personal
//  state stores. Without the environment the frontend runs exactly as before.
//

#ifndef ES_APP_PROFILES_PROFILE_MODE_H
#define ES_APP_PROFILES_PROFILE_MODE_H

#include "profiles/AvatarManifest.h"
#include "profiles/ProfileMetadata.h"
#include "profiles/ProfileSession.h"
#include "profiles/ProfileStateJsonStore.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

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

    // The personal state store for one system. The session owns one store per real system, created
    // on first use and kept for the whole process, so a rescan or a system reload never loses
    // staged changes. Returns nullptr in legacy mode and for collection systems; the caller passes
    // the collection flag because a system is not yet registered while its constructor runs.
    // System loading runs on the main thread (startup and ROM rescans), so no locking is needed.
    ProfileState::JsonStore* storeForSystem(const std::string& system,
                                            const bool collectionSystem);

    // True when the session is usable and every store that was opened is resolved. When false the
    // profile's library must not be shown.
    bool storesResolved();

    // A user-facing description of what is unresolved: the session, M or avatar manifest, and every
    // unresolved store. Empty when everything is resolved.
    std::string unresolvedReason();

    // True when at least one unresolved store has a backup that this reader can load.
    bool hasRecoverableStore();

    // Recovers every unresolved store from its backup. Returns true when every store is resolved
    // afterwards. Nothing is written: the aside file and the next flush belong to the writers.
    bool recoverStores();

    // Re-reads M from disk, for example after a failed write.
    bool reloadMetadata();
    // Flushes M. Returns false and sets `error` when the write failed.
    bool save(std::string& error);

    // True when any open store or the profile metadata file has staged changes that a switch
    // would have to flush first.
    bool hasPendingWrites();

    // Flushes every open personal-state store, in system order. Returns one entry per failed
    // store: the system name and the reason. Empty when every store is clean. This covers stores
    // whose SystemData was torn down earlier, for example by a rescan that removed a system.
    std::vector<std::pair<std::string, std::string>> flushStores();
} // namespace ProfileMode

#endif // ES_APP_PROFILES_PROFILE_MODE_H
