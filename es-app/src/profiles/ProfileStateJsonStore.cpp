//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileStateJsonStore.cpp
//
//  JSON personal-state store for one profile and one system (design sections 6.2 and 6.4).
//

#include "profiles/ProfileStateJsonStore.h"

#include "MetaData.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace
{
    const MetaDataListType listTypeFor(const ProfileRecordType type)
    {
        return type == ProfileRecordType::game ? GAME_METADATA : FOLDER_METADATA;
    }

    void applyDefaults(MetaDataList& record)
    {
        for (const MetaDataDecl& decl : record.getMDD()) {
            if (isPersonalMetadata(decl))
                record.set(decl.key, decl.defaultValue);
        }
    }

    bool sameFields(const std::vector<ProfileState::JsonField>& left,
                    const std::vector<ProfileState::JsonField>& right)
    {
        if (left.size() != right.size())
            return false;

        std::map<std::string, std::string> leftMap;
        for (const ProfileState::JsonField& field : left)
            leftMap[field.key] = field.value;

        for (const ProfileState::JsonField& field : right) {
            const auto found {leftMap.find(field.key)};
            if (found == leftMap.end() || found->second != field.value)
                return false;
        }

        return true;
    }
} // namespace

ProfileState::JsonStore::JsonStore(Session session, FileOperations& files, Clock& clock)
    : mSession {std::move(session)}
    , mFiles {files}
    , mClock {clock}
{
    mPath = mSession.stateDirectory + "/" + mSession.system + ".json";
    mDocument.system = mSession.system;

    std::string bytes;
    switch (mFiles.readFile(mPath, bytes)) {
        case ReadStatus::missing: {
            // A missing state file with a backup means the file was lost after a write; offer
            // the backup instead of starting a new state. A backup that exists but cannot be
            // read counts as present: starting a new state would silently replace it.
            std::string backup;
            const ReadStatus backupStatus {mFiles.readFile(mPath + ".bak", backup)};
            if (backupStatus == ReadStatus::ok || backupStatus == ReadStatus::error) {
                mResolved = false;
                mFileErrors.emplace_back("the state file is missing but a backup exists at " +
                                         mPath + ".bak");
            }
            break;
        }
        case ReadStatus::error:
            mResolved = false;
            mHasUnusableMain = true;
            mFileErrors.emplace_back("could not read the state file " + mPath);
            break;
        case ReadStatus::ok: {
            mHasPreviousFile = true;
            mPreviousBytes = bytes;

            JsonDocument document;
            std::string error;
            if (loadDocument(bytes, mSession.system, document, error) == LoadResult::loaded) {
                mDocument = std::move(document);
                mFileErrors = mDocument.fileErrors;
                mResolved = !mDocument.unresolved;
                mHasUnusableMain = mDocument.unresolved;
            }
            else {
                mResolved = false;
                mHasUnusableMain = true;
                mFileErrors.emplace_back("the state file " + mPath + " is not usable: " + error);
            }
            break;
        }
    }

    rebuildErrors();
}

void ProfileState::JsonStore::rebuildErrors()
{
    mErrors = mFileErrors;
    for (const std::string& error : mDocument.errors)
        mErrors.push_back(error);
    for (const auto& error : mRecordEditErrors)
        mErrors.push_back(error.second);
    for (const auto& error : mPreferenceEditErrors)
        mErrors.push_back(error.second);
}

const ProfileState::JsonEntry* ProfileState::JsonStore::findEntry(const ProfileRecordKey& key) const
{
    for (const JsonEntry& entry : mDocument.entries) {
        if (entry.type == key.type() && entry.path == key.path())
            return &entry;
    }

    return nullptr;
}

ProfileState::JsonEntry* ProfileState::JsonStore::findEntry(const ProfileRecordKey& key)
{
    for (JsonEntry& entry : mDocument.entries) {
        if (entry.type == key.type() && entry.path == key.path())
            return &entry;
    }

    return nullptr;
}

void ProfileState::JsonStore::apply(const ProfileRecordKey& key, MetaDataList& record) const
{
    if (!mResolved || record.getType() != listTypeFor(key.type())) {
        applyDefaults(record);
        return;
    }

    const JsonEntry* entry {findEntry(key)};
    if (entry == nullptr || !entry->keyValid) {
        applyDefaults(record);
        return;
    }

    // Field by field: a valid field is applied even when a sibling has the wrong type; the
    // wrong-typed field takes its declared default.
    for (const MetaDataDecl& decl : record.getMDD()) {
        if (!isPersonalMetadata(decl))
            continue;

        std::string value;
        bool applied {false};
        for (const JsonField& field : entry->fields) {
            if (field.key != decl.key || !field.typeValid)
                continue;
            applied = metadataFromJsonValue(decl, field.value, value);
            break;
        }

        record.set(decl.key, applied ? value : decl.defaultValue);
    }
}

void ProfileState::JsonStore::capture(const ProfileRecordKey& key, const MetaDataList& record)
{
    if (!mResolved || record.getType() != listTypeFor(key.type()))
        return;

    const std::string recordId {(key.type() == ProfileRecordType::game ? "game" : "folder") +
                                std::string {":"} + key.path()};
    // Only this record's previous edit error is cleared; other records keep theirs.
    mRecordEditErrors.erase(recordId);

    std::vector<JsonField> captured;
    std::map<std::string, std::string> capturedValues;
    std::set<std::string> unencodable;
    std::string encodeError;
    for (const MetaDataDecl& decl : record.getMDD()) {
        if (!isPersonalMetadata(decl))
            continue;

        const std::string& value {record.get(decl.key)};
        if (value == decl.defaultValue)
            continue;

        std::string json;
        if (!jsonValueFromMetadata(decl, value, json)) {
            unencodable.insert(decl.key);
            if (!encodeError.empty())
                encodeError += "; ";
            encodeError += "field \"" + decl.key + "\" cannot be encoded, the stored value is kept";
            continue;
        }

        captured.push_back({decl.key, json, true});
        capturedValues[decl.key] = json;
    }

    auto publish = [&]() {
        if (!encodeError.empty())
            mRecordEditErrors[recordId] = "record \"" + key.path() + "\": " + encodeError;
        rebuildErrors();
    };

    JsonEntry* entry {findEntry(key)};
    if (entry == nullptr) {
        if (captured.empty()) {
            publish();
            return;
        }

        JsonEntry created;
        created.type = key.type();
        created.path = key.path();
        created.fields = std::move(captured);
        mDocument.entries.emplace_back(std::move(created));
        mDocument.errors = recordErrors(mDocument);
        markDirty();
        publish();
        return;
    }

    // Known personal fields are replaced by the captured value or dropped when the record holds
    // the declared default. A field whose JSON type was wrong is preserved verbatim unless the
    // caller supplies a non-default value for that same key: it counts as unknown data, so an
    // entry holding one is never dropped as "all defaults".
    std::vector<JsonField> fields {captured};
    for (const JsonField& field : entry->fields) {
        if (personalDecl(entry->type, field.key) == nullptr) {
            fields.push_back(field);
            continue;
        }

        // Keep the stored value when the new one could not be encoded, and keep a wrong-typed
        // value unless the caller supplies a non-default value for that key.
        if (unencodable.count(field.key) != 0 ||
            (!field.typeValid && capturedValues.count(field.key) == 0))
            fields.push_back(field);
    }

    const bool fieldsChanged {!sameFields(entry->fields, fields)};
    const bool emptied {fields.empty() && entry->extraKeys.empty()};

    if (emptied) {
        const auto found {std::find_if(
            mDocument.entries.begin(), mDocument.entries.end(), [&key](const JsonEntry& candidate) {
                return candidate.type == key.type() && candidate.path == key.path();
            })};
        if (found != mDocument.entries.end())
            mDocument.entries.erase(found);
        mDocument.errors = recordErrors(mDocument);
        markDirty();
        publish();
        return;
    }

    if (fieldsChanged) {
        entry->fields = std::move(fields);
        mDocument.errors = recordErrors(mDocument);
        markDirty();
    }

    publish();
}

ProfileState::FlushResult ProfileState::JsonStore::flush()
{
    if (!mResolved) {
        return {FlushStatus::failure,
                mErrors.empty() ? "the profile state is unresolved" : mErrors.front()};
    }

    if (!mDirty)
        return {FlushStatus::nothingToDo, ""};

    const std::string bytes {serializeDocument(mDocument)};

    // Validate the bytes by re-parsing them before anything on disk is touched.
    JsonDocument check;
    std::string error;
    if (loadDocument(bytes, mSession.system, check, error) != LoadResult::loaded ||
        check.unresolved) {
        return {FlushStatus::failure, "the serialized state did not validate: " + error};
    }

    if (mMoveUnusableMainAside) {
        const std::string asidePath {mPath + ".unresolved-" + mClock.utcTimestamp()};
        std::string existing;
        if (mFiles.readFile(asidePath, existing) != ReadStatus::missing)
            return {FlushStatus::failure, "refusing to overwrite " + asidePath};
        if (!mFiles.renameFile(mPath, asidePath))
            return {FlushStatus::failure,
                    "could not move the unusable state file aside to " + asidePath};
        mMoveUnusableMainAside = false;
    }

    const PublishResult published {
        publishFileAtomically(mFiles, mPath, bytes, mPreviousBytes, mHasPreviousFile)};
    if (!published.success)
        return {FlushStatus::failure, published.reason};

    mDirty = false;
    mPreviousBytes = bytes;
    mHasPreviousFile = true;

    return {FlushStatus::success, ""};
}

bool ProfileState::JsonStore::recoverFromBackup()
{
    const std::string backupPath {mPath + ".bak"};
    std::string bytes;
    if (mFiles.readFile(backupPath, bytes) != ReadStatus::ok) {
        mFileErrors.emplace_back("no readable backup at " + backupPath);
        rebuildErrors();
        return false;
    }

    JsonDocument document;
    std::string error;
    if (loadDocument(bytes, mSession.system, document, error) != LoadResult::loaded ||
        document.unresolved) {
        mFileErrors.emplace_back("the backup at " + backupPath + " is not usable: " + error);
        rebuildErrors();
        return false;
    }

    mDocument = std::move(document);
    mFileErrors = mDocument.fileErrors;
    mResolved = true;
    // Write the recovered state back on the next flush, but never copy the broken or missing
    // file over the backup.
    mDirty = true;
    mHasPreviousFile = false;
    mMoveUnusableMainAside = mHasUnusableMain;
    rebuildErrors();

    return true;
}

void ProfileState::JsonStore::setAlternativeEmulator(const std::string& value)
{
    mPreferenceEditErrors.erase("alternativeEmulator");

    if (!value.empty() && !isValidUtf8(value)) {
        mPreferenceEditErrors["alternativeEmulator"] = "alternativeEmulator is not valid UTF-8";
        rebuildErrors();
        return;
    }

    const bool storedInvalid {mDocument.hasAlternativeEmulator &&
                              !mDocument.alternativeEmulatorValid};

    if (value.empty()) {
        // Writing the default must not erase a wrong-typed value that is kept verbatim.
        if (storedInvalid || !mDocument.hasAlternativeEmulator) {
            rebuildErrors();
            return;
        }
        mDocument.hasAlternativeEmulator = false;
        mDocument.alternativeEmulatorValid = false;
        mDocument.alternativeEmulator.clear();
        mDocument.alternativeEmulatorRaw.clear();
        markDirty();
        rebuildErrors();
        return;
    }

    if (mDocument.hasAlternativeEmulator && mDocument.alternativeEmulatorValid &&
        mDocument.alternativeEmulator == value) {
        rebuildErrors();
        return;
    }

    mDocument.hasAlternativeEmulator = true;
    mDocument.alternativeEmulatorValid = true;
    mDocument.alternativeEmulator = value;
    mDocument.alternativeEmulatorRaw.clear();
    markDirty();
    rebuildErrors();
}

void ProfileState::JsonStore::setLaunchOnOtherScreen(const bool value)
{
    mPreferenceEditErrors.erase("launchOnOtherScreen");

    const bool storedInvalid {mDocument.hasLaunchOnOtherScreen &&
                              !mDocument.launchOnOtherScreenValid};

    // The declared default is true, so storing true means "omit the preference". It must not
    // erase a wrong-typed value that is kept verbatim.
    if (value) {
        if (storedInvalid || !mDocument.hasLaunchOnOtherScreen) {
            rebuildErrors();
            return;
        }
        mDocument.hasLaunchOnOtherScreen = false;
        mDocument.launchOnOtherScreenValid = false;
        mDocument.launchOnOtherScreenRaw.clear();
        markDirty();
        rebuildErrors();
        return;
    }

    if (mDocument.hasLaunchOnOtherScreen && mDocument.launchOnOtherScreenValid &&
        mDocument.launchOnOtherScreen == value) {
        rebuildErrors();
        return;
    }

    mDocument.hasLaunchOnOtherScreen = true;
    mDocument.launchOnOtherScreenValid = true;
    mDocument.launchOnOtherScreen = value;
    mDocument.launchOnOtherScreenRaw.clear();
    markDirty();
    rebuildErrors();
}
