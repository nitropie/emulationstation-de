//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileStateJson.h
//
//  JSON document for one system's personal state (design section 6.2).
//
//  The document keeps every value the reader does not type-check as compact JSON text, so
//  unknown keys at the top level, inside an entry and inside `fields` survive a read/write cycle.
//  Duplicate member names and duplicate (type, path) entries are invalid documents.
//

#ifndef ES_APP_PROFILES_PROFILE_STATE_JSON_H
#define ES_APP_PROFILES_PROFILE_STATE_JSON_H

#include "MetaData.h"
#include "profiles/ProfileRecordKey.h"

#include <string>
#include <vector>

namespace ProfileState
{
    constexpr int sStateVersion {1};
    constexpr const char* sStateSchema {"nitropie.profile-state"};

    struct JsonField {
        std::string key;
        // Compact JSON text of the value.
        std::string value;
        // False when the JSON type does not match the metadata type. The field is kept as-is.
        bool typeValid {true};
    };

    struct JsonEntry {
        ProfileRecordType type {ProfileRecordType::game};
        std::string path;
        // False for a key that does not normalize below the system root or is not valid UTF-8.
        // The record is kept but never applied or updated.
        bool keyValid {true};
        std::vector<JsonField> fields;
        // Unknown members of the entry object, outside `fields`.
        std::vector<JsonField> extraKeys;
    };

    struct JsonDocument {
        int version {sStateVersion};
        std::string system;
        // Preferences keep their key exactly once, even when the file value has the wrong type.
        bool hasAlternativeEmulator {false};
        bool alternativeEmulatorValid {false};
        std::string alternativeEmulator;
        std::string alternativeEmulatorRaw;
        bool hasLaunchOnOtherScreen {false};
        bool launchOnOtherScreenValid {false};
        bool launchOnOtherScreen {true};
        std::string launchOnOtherScreenRaw;
        std::vector<JsonField> extraPreferences;
        std::vector<JsonField> extraTop;
        std::vector<JsonEntry> entries;
        // Preference and header diagnostics (file-level, kept after a capture).
        std::vector<std::string> fileErrors;
        // Record-level diagnostics, rebuilt by recordErrors() after a capture.
        std::vector<std::string> errors;
        // Too new for this reader. The file is never overwritten.
        bool unresolved {false};
    };

    enum class LoadResult {
        // The file is absent or empty, which starts a new state.
        empty,
        // The document was read. Check `unresolved` for a too-new version.
        loaded,
        // The file is corrupt, has an unusable header or contains duplicate keys or entries.
        invalid
    };

    LoadResult loadDocument(const std::string& bytes,
                            const std::string& system,
                            JsonDocument& document,
                            std::string& error);

    // Deterministic output: two-space indent, fixed key order, entries sorted by path bytes and
    // then game before folder, and a trailing newline.
    std::string serializeDocument(const JsonDocument& document);

    // Metadata value <-> JSON value for a personal field, following the MetaData type.
    bool jsonValueFromMetadata(const MetaDataDecl& decl,
                               const std::string& value,
                               std::string& json);
    bool metadataFromJsonValue(const MetaDataDecl& decl,
                               const std::string& json,
                               std::string& value);
    bool jsonMatchesMetadataType(const MetaDataDecl& decl, const std::string& json);

    // The declaration for a personal key of an entry type, or nullptr for an unknown or shared
    // key.
    const MetaDataDecl* personalDecl(ProfileRecordType type, const std::string& key);

    // Diagnostics for the records currently in the document, one line per invalid field or
    // unresolved key.
    std::vector<std::string> recordErrors(const JsonDocument& document);
} // namespace ProfileState

#endif // ES_APP_PROFILES_PROFILE_STATE_JSON_H
