//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileStateJsonStore.h
//
//  JSON personal-state store for one profile and one system (design sections 6.2 and 6.4).
//
//  The state directory itself is created by the launcher or by home-manager (design section
//  .3); the store never creates it. When it is missing, reads see no file and a flush fails
//  with an explicit reason.
//

#ifndef ES_APP_PROFILES_PROFILE_STATE_JSON_STORE_H
#define ES_APP_PROFILES_PROFILE_STATE_JSON_STORE_H

#include "profiles/ProfileStateFile.h"
#include "profiles/ProfileStateJson.h"
#include "profiles/ProfileStateStore.h"

#include <map>
#include <string>
#include <vector>

namespace ProfileState
{
    class JsonStore final : public Store
    {
    public:
        JsonStore(Session session, FileOperations& files, Clock& clock = systemClock());

        const Session& session() const override { return mSession; }
        bool isResolved() const override { return mResolved; }
        // File, record and edit diagnostics. A record whose value cannot be encoded and a
        // preference that cannot be stored are reported here until that same record or
        // preference is written cleanly.
        const std::vector<std::string>& loadErrors() const override { return mErrors; }
        bool recoverFromBackup() override;

        void apply(const ProfileRecordKey& key, MetaDataList& record) const override;
        void capture(const ProfileRecordKey& key, const MetaDataList& record) override;
        FlushResult flush() override;
        bool hasPendingWrites() const override { return mDirty; }
        bool canSwitchProfile() const override { return mResolved && !mDirty; }

        // True when a backup exists that this reader can load. Does not change the store, so a
        // caller can offer a recovery instead of performing one.
        bool hasUsableBackup() const;

        bool hasAlternativeEmulator() const
        {
            return mDocument.hasAlternativeEmulator && mDocument.alternativeEmulatorValid;
        }
        const std::string& alternativeEmulator() const { return mDocument.alternativeEmulator; }
        void setAlternativeEmulator(const std::string& value);
        bool hasLaunchOnOtherScreen() const
        {
            return mDocument.hasLaunchOnOtherScreen && mDocument.launchOnOtherScreenValid;
        }
        bool launchOnOtherScreen() const { return mDocument.launchOnOtherScreen; }
        void setLaunchOnOtherScreen(bool value);

        // The path of the state file, for diagnostics and tests.
        const std::string& statePath() const { return mPath; }

    private:
        const JsonEntry* findEntry(const ProfileRecordKey& key) const;
        JsonEntry* findEntry(const ProfileRecordKey& key);
        void markDirty() { mDirty = true; }
        void rebuildErrors();

        Session mSession;
        FileOperations& mFiles;
        Clock& mClock;
        std::string mPath;
        JsonDocument mDocument;
        std::vector<std::string> mFileErrors;
        // Capture and setter diagnostics, keyed by record and by preference name so one entry
        // does not erase another. Cleared only when that same record or preference is captured
        // or set cleanly again. loadErrors() also carries these.
        std::map<std::string, std::string> mRecordEditErrors;
        std::map<std::string, std::string> mPreferenceEditErrors;
        std::vector<std::string> mErrors;
        bool mResolved {true};
        bool mDirty {false};
        // The bytes of the last file that was read or written, kept for <system>.json.bak.
        bool mHasPreviousFile {false};
        std::string mPreviousBytes;
        // True when the main file exists but cannot be used, so recovery moves it aside first.
        bool mHasUnusableMain {false};
        bool mMoveUnusableMainAside {false};
    };
} // namespace ProfileState

#endif // ES_APP_PROFILES_PROFILE_STATE_JSON_STORE_H
