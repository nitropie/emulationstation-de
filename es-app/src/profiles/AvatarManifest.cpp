//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  AvatarManifest.cpp
//
//  Reader for the baked-in avatar manifest.
//

#include "profiles/AvatarManifest.h"

#include "profiles/ProfileJson.h"

#include "rapidjson/document.h"

#include <set>

namespace
{
    std::string parentDirectory(const std::string& path)
    {
        const std::size_t separator {path.find_last_of('/')};
        return separator == std::string::npos ? std::string {"."} : path.substr(0, separator);
    }
} // namespace

bool ProfileAvatars::isAvatarId(const std::string& text)
{
    if (text.empty() || text.size() > 32)
        return false;

    for (const char character : text) {
        const bool valid {(character >= 'a' && character <= 'z') ||
                          (character >= '0' && character <= '9') || character == '-'};
        if (!valid)
            return false;
    }

    return true;
}

std::optional<ProfileAvatars::Manifest> ProfileAvatars::Manifest::load(
    const std::string& path, ProfileState::FileOperations& files, std::string& error)
{
    std::string bytes;
    if (files.readFile(path, bytes) != ProfileState::ReadStatus::ok) {
        error = "could not read the avatar manifest " + path;
        return std::nullopt;
    }

    rapidjson::Document document;
    if (!ProfileJson::parseObject(bytes, document, error)) {
        error = "the avatar manifest is not usable: " + error;
        return std::nullopt;
    }

    std::string duplicate;
    if (ProfileJson::hasDuplicateKeys(document, duplicate)) {
        error = "the avatar manifest has a duplicate key \"" + duplicate + "\"";
        return std::nullopt;
    }

    const rapidjson::Value::ConstMemberIterator version {document.FindMember("version")};
    if (version == document.MemberEnd() || !version->value.IsInt() ||
        version->value.GetInt() != 1) {
        error = "the avatar manifest has an unsupported version";
        return std::nullopt;
    }

    const rapidjson::Value::ConstMemberIterator defaultId {document.FindMember("default")};
    if (defaultId == document.MemberEnd() || !defaultId->value.IsString() ||
        !isAvatarId(defaultId->value.GetString())) {
        error = "the avatar manifest has no valid default";
        return std::nullopt;
    }

    const rapidjson::Value::ConstMemberIterator avatars {document.FindMember("avatars")};
    if (avatars == document.MemberEnd() || !avatars->value.IsArray()) {
        error = "the avatar manifest has no avatars";
        return std::nullopt;
    }

    Manifest manifest;
    manifest.mDirectory = parentDirectory(path);
    manifest.mDefaultId = defaultId->value.GetString();

    std::set<std::string> ids;
    std::set<std::string> fileNames;

    for (const rapidjson::Value& value : avatars->value.GetArray()) {
        if (!value.IsObject()) {
            error = "an avatar entry is not an object";
            return std::nullopt;
        }

        const rapidjson::Value::ConstMemberIterator id {value.FindMember("id")};
        const rapidjson::Value::ConstMemberIterator file {value.FindMember("file")};
        const rapidjson::Value::ConstMemberIterator label {value.FindMember("label")};
        if (id == value.MemberEnd() || !id->value.IsString() || file == value.MemberEnd() ||
            !file->value.IsString() || label == value.MemberEnd() || !label->value.IsString()) {
            error = "an avatar entry is missing its id, file or label";
            return std::nullopt;
        }

        const std::string avatarId {id->value.GetString()};
        if (!isAvatarId(avatarId)) {
            error = "the avatar manifest has a malformed avatar id \"" + avatarId + "\"";
            return std::nullopt;
        }

        if (!ids.insert(avatarId).second) {
            error = "the avatar manifest has a duplicate avatar id \"" + avatarId + "\"";
            return std::nullopt;
        }

        const std::string fileName {file->value.GetString()};
        if (fileName.empty() || fileName.find('/') != std::string::npos) {
            error = "the avatar \"" + avatarId + "\" has an invalid file name";
            return std::nullopt;
        }

        if (!fileNames.insert(fileName).second) {
            error = "the avatar manifest references \"" + fileName + "\" twice";
            return std::nullopt;
        }

        manifest.mAvatars.push_back({avatarId, fileName, label->value.GetString()});
    }

    if (ids.count(manifest.mDefaultId) == 0) {
        error = "the avatar default \"" + manifest.mDefaultId + "\" is not in the list";
        return std::nullopt;
    }

    return manifest;
}

const ProfileAvatars::AvatarEntry* ProfileAvatars::Manifest::find(const std::string& id) const
{
    for (const AvatarEntry& entry : mAvatars) {
        if (entry.id == id)
            return &entry;
    }

    return nullptr;
}

const ProfileAvatars::AvatarEntry& ProfileAvatars::Manifest::displayFor(
    const std::string& storedId) const
{
    const AvatarEntry* entry {find(storedId)};
    if (entry != nullptr)
        return *entry;

    // An unknown stored id displays the default but stays in the profile file.
    return *find(mDefaultId);
}

std::string ProfileAvatars::Manifest::pathFor(const AvatarEntry& entry) const
{
    return mDirectory + "/" + entry.file;
}
