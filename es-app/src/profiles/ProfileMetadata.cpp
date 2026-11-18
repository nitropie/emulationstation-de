//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileMetadata.cpp
//
//  Reader and writer for the profile metadata file M.
//

#include "profiles/ProfileMetadata.h"

#include "profiles/ProfileJson.h"

#include "rapidjson/document.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace
{
    constexpr const char* sProfileSchema {"nitropie.profile"};

    std::string trim(const std::string& text)
    {
        std::size_t begin {0};
        std::size_t end {text.size()};

        auto isSpace = [](const char character) {
            return character == ' ' || character == '\t' || character == '\n' || character == '\r';
        };

        while (begin < end && isSpace(text[begin]))
            ++begin;
        while (end > begin && isSpace(text[end - 1]))
            --end;

        return text.substr(begin, end - begin);
    }

    std::size_t codePointCount(const std::string& text)
    {
        std::size_t count {0};
        for (const unsigned char character : text) {
            if ((character & 0xC0) != 0x80)
                ++count;
        }
        return count;
    }

    bool hasControlCharacter(const std::string& text)
    {
        for (const unsigned char character : text) {
            if (character < 0x20 || character == 0x7F)
                return true;
        }
        return false;
    }

    bool validateDisplayName(const std::string& raw, std::string& trimmed, std::string& error)
    {
        trimmed = trim(raw);

        if (!isValidUtf8(trimmed)) {
            error = "the display name is not valid UTF-8";
            return false;
        }

        const std::size_t length {codePointCount(trimmed)};
        if (length < 1 || length > 32) {
            error = "the display name must be 1 to 32 characters";
            return false;
        }

        if (hasControlCharacter(trimmed)) {
            error = "the display name must not contain control characters";
            return false;
        }

        return true;
    }
} // namespace

ProfileMetadata::ProfileMetadata(std::string path,
                                 std::string expectedId,
                                 ProfileState::FileOperations& files,
                                 ProfileState::Clock& clock)
    : mPath {std::move(path)}
    , mExpectedId {std::move(expectedId)}
    , mFiles {files}
    , mClock {clock}
{
    std::string bytes;
    switch (mFiles.readFile(mPath, bytes)) {
        case ProfileState::ReadStatus::missing: {
            // Nix seeds M before the session starts, so a missing file is an anomaly. A backup
            // can be recovered.
            mResolved = false;
            std::string backup;
            const ProfileState::ReadStatus backupStatus {mFiles.readFile(mPath + ".bak", backup)};
            if (backupStatus == ProfileState::ReadStatus::ok ||
                backupStatus == ProfileState::ReadStatus::error)
                mErrors.emplace_back("the profile metadata is missing but a backup exists at " +
                                     mPath + ".bak");
            else
                mErrors.emplace_back("the profile metadata is missing at " + mPath);
            break;
        }
        case ProfileState::ReadStatus::error:
            mResolved = false;
            mErrors.emplace_back("could not read the profile metadata " + mPath);
            break;
        case ProfileState::ReadStatus::ok: {
            mHasPreviousFile = true;
            mPreviousBytes = bytes;

            std::string error;
            if (!readDocument(bytes, error)) {
                mResolved = false;
                mErrors.emplace_back("the profile metadata " + mPath + " is not usable: " + error);
            }
            break;
        }
    }
}

bool ProfileMetadata::readDocument(const std::string& bytes, std::string& error)
{
    rapidjson::Document document;
    if (!ProfileJson::parseObject(bytes, document, error))
        return false;

    std::string duplicate;
    if (ProfileJson::hasDuplicateKeys(document, duplicate)) {
        error = "duplicate key \"" + duplicate + "\"";
        return false;
    }

    const rapidjson::Value::ConstMemberIterator schema {document.FindMember("schema")};
    if (schema == document.MemberEnd() || !schema->value.IsString() ||
        std::string {schema->value.GetString()} != sProfileSchema) {
        error = "the schema is not \"" + std::string {sProfileSchema} + "\"";
        return false;
    }

    const rapidjson::Value::ConstMemberIterator version {document.FindMember("version")};
    if (version == document.MemberEnd() || !version->value.IsInt() || version->value.GetInt() < 1) {
        error = "the file has no supported version";
        return false;
    }

    if (version->value.GetInt() > 1) {
        mVersion = version->value.GetInt();
        mErrors.emplace_back("the profile metadata has version " + std::to_string(mVersion) +
                             ", newer than 1");
        return false;
    }

    const rapidjson::Value::ConstMemberIterator id {document.FindMember("id")};
    if (id == document.MemberEnd() || !id->value.IsString()) {
        error = "the file has no id";
        return false;
    }

    if (mExpectedId != id->value.GetString()) {
        error = "the id \"" + std::string {id->value.GetString()} +
                "\" does not match the profile directory \"" + mExpectedId + "\"";
        return false;
    }

    const rapidjson::Value::ConstMemberIterator name {document.FindMember("displayName")};
    if (name == document.MemberEnd() || !name->value.IsString()) {
        error = "the file has no displayName";
        return false;
    }

    mDisplayName = name->value.GetString();

    const rapidjson::Value::ConstMemberIterator avatar {document.FindMember("avatar")};
    if (avatar == document.MemberEnd() || !avatar->value.IsString()) {
        error = "the file has no avatar";
        return false;
    }

    mAvatar = avatar->value.GetString();

    // Report, but keep, values the screen can let the user fix.
    std::string trimmed;
    std::string nameError;
    if (!validateDisplayName(mDisplayName, trimmed, nameError))
        mErrors.emplace_back(nameError);

    if (!ProfileAvatars::isAvatarId(mAvatar))
        mErrors.emplace_back("the avatar \"" + mAvatar + "\" is not a valid avatar id");

    for (rapidjson::Value::ConstMemberIterator member {document.MemberBegin()};
         member != document.MemberEnd(); ++member) {
        const std::string key {member->name.GetString()};
        if (key == "schema" || key == "version" || key == "id" || key == "displayName" ||
            key == "avatar")
            continue;
        mExtraKeys.push_back({key, ProfileJson::fragment(member->value), true});
    }

    return true;
}

std::string ProfileMetadata::serialize() const
{
    rapidjson::StringBuffer buffer;
    ProfileJson::Writer writer {buffer};
    writer.SetIndent(' ', 2);

    writer.StartObject();
    writer.Key("schema");
    writer.String(sProfileSchema);
    writer.Key("version");
    writer.Int(1);
    writer.Key("id");
    writer.String(mExpectedId.c_str(), static_cast<rapidjson::SizeType>(mExpectedId.size()));
    writer.Key("displayName");
    writer.String(mDisplayName.c_str(), static_cast<rapidjson::SizeType>(mDisplayName.size()));
    writer.Key("avatar");
    writer.String(mAvatar.c_str(), static_cast<rapidjson::SizeType>(mAvatar.size()));

    std::vector<ProfileState::JsonField> extra {mExtraKeys};
    std::sort(extra.begin(), extra.end(),
              [](const ProfileState::JsonField& left, const ProfileState::JsonField& right) {
                  return left.key < right.key;
              });
    for (const ProfileState::JsonField& field : extra) {
        writer.Key(field.key.c_str(), static_cast<rapidjson::SizeType>(field.key.size()));
        ProfileJson::emitFragment(writer, field.value);
    }

    writer.EndObject();
    buffer.Put('\n');

    return std::string {buffer.GetString(), buffer.GetSize()};
}

bool ProfileMetadata::setDisplayName(const std::string& name, std::string& error)
{
    std::string trimmed;
    if (!validateDisplayName(name, trimmed, error))
        return false;

    error.clear();
    if (trimmed == mDisplayName)
        return true;

    mDisplayName = trimmed;
    mDirty = true;
    return true;
}

bool ProfileMetadata::setAvatar(const std::string& avatarId, std::string& error)
{
    if (!ProfileAvatars::isAvatarId(avatarId)) {
        error = "the avatar \"" + avatarId + "\" is not a valid avatar id";
        return false;
    }

    error.clear();
    if (avatarId == mAvatar)
        return true;

    mAvatar = avatarId;
    mDirty = true;
    return true;
}

std::string ProfileMetadata::displayedAvatar(const ProfileAvatars::Manifest& manifest) const
{
    return manifest.displayFor(mAvatar).id;
}

ProfileState::FlushResult ProfileMetadata::flush()
{
    if (!mResolved) {
        return {ProfileState::FlushStatus::failure,
                mErrors.empty() ? "the profile metadata is unresolved" : mErrors.front()};
    }

    if (!mDirty)
        return {ProfileState::FlushStatus::nothingToDo, ""};

    const std::string bytes {serialize()};

    // Validate the bytes with the reader before anything on disk is touched. The check object
    // re-reads the current file in its constructor, so its diagnostics are cleared first.
    ProfileMetadata check {mPath, mExpectedId, mFiles, mClock};
    check.mErrors.clear();
    check.mExtraKeys.clear();
    std::string error;
    if (!check.readDocument(bytes, error) || !check.mErrors.empty()) {
        return {ProfileState::FlushStatus::failure,
                "the serialized profile metadata did not validate: " + error};
    }

    const ProfileState::PublishResult published {ProfileState::publishFileAtomically(
        mFiles, mPath, bytes, mPreviousBytes, mHasPreviousFile)};
    if (!published.success)
        return {ProfileState::FlushStatus::failure, published.reason};

    mDirty = false;
    mPreviousBytes = bytes;
    mHasPreviousFile = true;

    return {ProfileState::FlushStatus::success, ""};
}

bool ProfileMetadata::recoverFromBackup()
{
    const std::string backupPath {mPath + ".bak"};
    std::string bytes;
    if (mFiles.readFile(backupPath, bytes) != ProfileState::ReadStatus::ok) {
        mErrors.emplace_back("no readable backup at " + backupPath);
        return false;
    }

    // Parse the backup into the document state, restoring the current state on failure.
    const int version {mVersion};
    const std::string name {mDisplayName};
    const std::string avatar {mAvatar};
    const std::vector<ProfileState::JsonField> extra {mExtraKeys};
    const std::vector<std::string> errors {mErrors};

    mVersion = 1;
    mErrors.clear();
    mExtraKeys.clear();

    std::string error;
    if (!readDocument(bytes, error)) {
        mVersion = version;
        mDisplayName = name;
        mAvatar = avatar;
        mExtraKeys = extra;
        mErrors = errors;
        mErrors.emplace_back("the backup at " + backupPath + " is not usable: " + error);
        return false;
    }

    mResolved = true;
    mDirty = true;
    mHasPreviousFile = false;
    return true;
}
