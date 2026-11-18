//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileMetadata.h
//
//  Reader and writer for the profile metadata file M (design section 7.1):
//
//    {
//      "schema": "nitropie.profile",
//      "version": 1,
//      "id": "<uuid>",
//      "displayName": "David",
//      "avatar": "fox"
//    }
//
//  The file sits outside the profile HOME so the chooser can read every profile's name and
//  avatar. An unknown avatar is displayed as the manifest default and kept in the file.
//

#ifndef ES_APP_PROFILES_PROFILE_METADATA_H
#define ES_APP_PROFILES_PROFILE_METADATA_H

#include "profiles/AvatarManifest.h"
#include "profiles/ProfileStateFile.h"
#include "profiles/ProfileStateJson.h"
#include "profiles/ProfileStateStore.h"

#include <string>
#include <vector>

class ProfileMetadata
{
public:
    // `expectedId` is the profile directory name from the catalog. A mismatch makes the file
    // unresolved.
    ProfileMetadata(std::string path,
                    std::string expectedId,
                    ProfileState::FileOperations& files,
                    ProfileState::Clock& clock = ProfileState::systemClock());

    bool isResolved() const { return mResolved; }
    const std::vector<std::string>& loadErrors() const { return mErrors; }
    bool hasPendingWrites() const { return mDirty; }
    bool recoverFromBackup();

    const std::string& id() const { return mExpectedId; }
    const std::string& displayName() const { return mDisplayName; }
    // The stored avatar id, which may be unknown to the manifest.
    const std::string& avatarId() const { return mAvatar; }
    // The avatar to display: the stored one when known, otherwise the manifest default.
    std::string displayedAvatar(const ProfileAvatars::Manifest& manifest) const;

    // Both return false and set `error` without staging anything when the value is invalid.
    bool setDisplayName(const std::string& name, std::string& error);
    bool setAvatar(const std::string& avatarId, std::string& error);

    ProfileState::FlushResult flush();

    const std::string& path() const { return mPath; }

private:
    bool readDocument(const std::string& bytes, std::string& error);
    std::string serialize() const;

    std::string mPath;
    std::string mExpectedId;
    ProfileState::FileOperations& mFiles;
    ProfileState::Clock& mClock;

    bool mResolved {true};
    bool mDirty {false};
    bool mHasPreviousFile {false};
    std::string mPreviousBytes;
    std::vector<std::string> mErrors;

    int mVersion {1};
    std::string mDisplayName;
    std::string mAvatar;
    std::vector<ProfileState::JsonField> mExtraKeys;
};

#endif // ES_APP_PROFILES_PROFILE_METADATA_H
