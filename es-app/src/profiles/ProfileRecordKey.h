//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileRecordKey.h
//
//  Identity of one personal-state record.
//
//  A record is keyed by (system, type, path). The path is relative to the system's ROM root,
//  uses "/" separators, and is compared byte for byte. A path that is not normalized, or that
//  can leave the root, is rejected: falling back to the displayed title, the basename or an
//  absolute mount path would let an unrelated file inherit someone's history.
//

#ifndef ES_APP_PROFILES_PROFILE_RECORD_KEY_H
#define ES_APP_PROFILES_PROFILE_RECORD_KEY_H

#include <optional>
#include <string>

enum class ProfileRecordType {
    game,
    folder
};

class ProfileRecordKey
{
public:
    // Returns no key when the system is empty or when the path is empty, absolute, not
    // normalized (a "." or ".." segment, an empty segment or a trailing slash) or contains a
    // NUL byte. A valid path is stored unchanged.
    static std::optional<ProfileRecordKey> fromPath(std::string system,
                                                    ProfileRecordType type,
                                                    std::string path);

    // A collection entry is a view over a source game and never owns a record of its own.
    static ProfileRecordKey fromCollectionEntry(const ProfileRecordKey& sourceGame)
    {
        return sourceGame;
    }

    const std::string& system() const { return mSystem; }
    ProfileRecordType type() const { return mType; }
    const std::string& path() const { return mPath; }

    bool operator==(const ProfileRecordKey& other) const;
    bool operator!=(const ProfileRecordKey& other) const { return !(*this == other); }

    // Sorts by system, then type, then path. The comparison is byte-wise.
    bool operator<(const ProfileRecordKey& other) const;

    // A readable form for diagnostics and test failures.
    std::string describe() const;

private:
    ProfileRecordKey(std::string system, ProfileRecordType type, std::string path);

    std::string mSystem;
    std::string mPath;
    ProfileRecordType mType;
};

#endif // ES_APP_PROFILES_PROFILE_RECORD_KEY_H
