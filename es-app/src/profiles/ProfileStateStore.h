//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileStateStore.h
//
//  Format-independent personal-state store.
//
//  A store covers one fixed profile and one system. Personal metadata is applied to a record
//  from the profile's state and captured back from it; the persistence format lives behind this
//  interface. When profile mode is disabled no store is created and the legacy gamelist adapter
//  is used unchanged.
//

#ifndef ES_APP_PROFILES_PROFILE_STATE_STORE_H
#define ES_APP_PROFILES_PROFILE_STATE_STORE_H

#include "profiles/ProfileRecordKey.h"

#include <string>
#include <vector>

class MetaDataList;

namespace ProfileState
{
    // The construction context owns the profile identity and paths. A store never changes them.
    struct Session {
        // The profile UUID from the catalog.
        std::string profileId;
        // The writable profile root, P/<profile-id>.
        std::string profileRoot;
        // The directory that holds one personal-state file per system, E/profile-state.
        std::string stateDirectory;
        // The system this store covers.
        std::string system;
    };

    enum class FlushStatus {
        // The staged changes are durable.
        success,
        // Nothing was staged, so no write was attempted.
        nothingToDo,
        // The write failed; the staged changes remain pending.
        failure
    };

    struct FlushResult {
        FlushStatus status {FlushStatus::failure};
        // Why the write failed. Empty for success and nothingToDo.
        std::string reason;
    };

    class Store
    {
    public:
        virtual ~Store() = default;

        virtual const Session& session() const = 0;

        // False when the state file is too new, corrupt or unreadable. An unresolved store never
        // overwrites the file and never reports itself as safely switchable.
        virtual bool isResolved() const = 0;

        // Diagnostics for unresolved files and for records that were kept but not applied, such
        // as a field whose JSON type does not match its metadata type.
        virtual const std::vector<std::string>& loadErrors() const = 0;

        // Replaces the in-memory state with the last known good copy and marks it for writing.
        // Returns false when no readable copy exists, leaving the store unresolved.
        virtual bool recoverFromBackup() = 0;

        // Overlays the profile's personal fields onto the record. A field without a stored value
        // takes its declared default; the shared gamelist is never used as a fallback, so an old
        // favourite or play count in the shared XML cannot leak into another profile. Shared
        // fields are left untouched. The record type has to match the key type.
        virtual void apply(const ProfileRecordKey& key, MetaDataList& record) const = 0;

        // Stages the record's personal fields. Fields equal to their declared default are
        // omitted, so an absent field always means the default. Returns false when a field
        // could not be written in its stored form: the stored value for that field is kept and
        // the rejection is reported through loadErrors(), and the caller must treat the record
        // as still pending rather than dropping the edit.
        virtual bool capture(const ProfileRecordKey& key, const MetaDataList& record) = 0;

        // Persists the staged changes and returns an explicit result. The dirty state is cleared
        // only after a successful durable write; on failure the staged changes stay pending.
        virtual FlushResult flush() = 0;

        virtual bool hasPendingWrites() const = 0;

        // A store is tied to one profile. The supervisor may only replace it when nothing is
        // pending, so a switch can never drop a staged personal change.
        virtual bool canSwitchProfile() const = 0;

        // System preferences (the alternative-emulator label and the launch-on-other-screen
        // choice) are personal state of the same profile and live in the same state file, so the
        // implementation persists them alongside the records. They belong to the system rather
        // than to a record and are therefore not part of the record API.
    };
} // namespace ProfileState

#endif // ES_APP_PROFILES_PROFILE_STATE_STORE_H
