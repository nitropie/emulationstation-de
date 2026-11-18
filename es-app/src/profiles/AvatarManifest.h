//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  AvatarManifest.h
//
//  Reader for the baked-in avatar manifest (design section 7.2).
//

#ifndef ES_APP_PROFILES_AVATAR_MANIFEST_H
#define ES_APP_PROFILES_AVATAR_MANIFEST_H

#include "profiles/ProfileStateFile.h"

#include <optional>
#include <string>
#include <vector>

namespace ProfileAvatars
{
    struct AvatarEntry {
        std::string id;
        std::string file;
        std::string label;
    };

    class Manifest
    {
    public:
        // Reads and validates avatars.json. Returns no manifest and explains why on failure.
        static std::optional<Manifest> load(const std::string& path,
                                            ProfileState::FileOperations& files,
                                            std::string& error);

        const std::string& directory() const { return mDirectory; }
        const std::string& defaultId() const { return mDefaultId; }
        const std::vector<AvatarEntry>& avatars() const { return mAvatars; }

        const AvatarEntry* find(const std::string& id) const;
        // The avatar to display for a stored id: the entry itself or the default.
        const AvatarEntry& displayFor(const std::string& storedId) const;
        std::string pathFor(const AvatarEntry& entry) const;

    private:
        std::string mDirectory;
        std::string mDefaultId;
        std::vector<AvatarEntry> mAvatars;
    };

    // True for ^[a-z0-9-]{1,32}$.
    bool isAvatarId(const std::string& text);
} // namespace ProfileAvatars

#endif // ES_APP_PROFILES_AVATAR_MANIFEST_H
