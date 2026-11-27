//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileSession.h
//
//  Immutable session context for one logical profile (design sections 4, 5 and 8).
//
//  The launcher sets NITROPIE_PROFILE_CATALOG and NITROPIE_PROFILE_ID; without them the
//  frontend runs in legacy mode and this context is inactive. The catalog and the session are
//  read-only here: nothing is preserved or written back.
//

#ifndef ES_APP_PROFILES_PROFILE_SESSION_H
#define ES_APP_PROFILES_PROFILE_SESSION_H

#include "profiles/ProfileStateFile.h"

#include <string>
#include <vector>

namespace ProfileSession
{
    struct CatalogEntry {
        std::string id;
        std::string metadata;
        std::string home;
        std::string appdata;
        std::string saves;
        std::string xdgConfigHome;
        std::string xdgDataHome;
        std::string xdgStateHome;
        std::string xdgCacheHome;
    };

    struct Catalog {
        int version {1};
        std::string defaultProfile;
        std::string libraryRoot;
        std::string avatarManifest;
        std::vector<CatalogEntry> profiles;
    };

    class Context
    {
    public:
        // True when a profile session exists. False in legacy mode or when the environment is
        // present but unusable (then error() is set).
        bool active() const { return mActive; }

        const std::string& error() const { return mError; }
        const std::string& profileId() const { return mEntry.id; }
        const std::string& metadataPath() const { return mEntry.metadata; }
        const std::string& avatarManifestPath() const { return mCatalog.avatarManifest; }
        const std::string& defaultProfileId() const { return mCatalog.defaultProfile; }
        // Shared library root from the catalog; the frontend reads shared gamelists from here.
        const std::string& libraryRoot() const { return mCatalog.libraryRoot; }
        const CatalogEntry& entry() const { return mEntry; }
        const Catalog& catalog() const { return mCatalog; }

    private:
        friend Context fromCatalog(const std::string& catalogBytes, const std::string& profileId);
        friend Context fromEnvironment(ProfileState::FileOperations& files);

        bool mActive {false};
        std::string mError;
        CatalogEntry mEntry;
        Catalog mCatalog;
    };

    // Resolves the session from NITROPIE_PROFILE_CATALOG and NITROPIE_PROFILE_ID.
    Context fromEnvironment(
        ProfileState::FileOperations& files = ProfileState::systemFileOperations());

    // Parses catalog bytes and selects one profile. Used by the environment resolver and by
    // tests.
    Context fromCatalog(const std::string& catalogBytes, const std::string& profileId);

    // True for a canonical lowercase UUIDv4 (design section 4).
    bool isUuidV4(const std::string& text);
} // namespace ProfileSession

#endif // ES_APP_PROFILES_PROFILE_SESSION_H
