//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileSession.cpp
//
//  Immutable session context for one logical profile.
//

#include "profiles/ProfileSession.h"

#include "profiles/ProfileJson.h"

#include "rapidjson/document.h"

#include <cstdlib>
#include <set>

namespace
{
    bool isHex(const char character)
    {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    }

    // Reads a string member. Returns false when it is absent or not a string.
    bool readString(const rapidjson::Value& object, const char* key, std::string& value)
    {
        const rapidjson::Value::ConstMemberIterator member {object.FindMember(key)};
        if (member == object.MemberEnd() || !member->value.IsString())
            return false;
        value = member->value.GetString();
        return true;
    }

    bool readEntry(const rapidjson::Value& value,
                   ProfileSession::CatalogEntry& entry,
                   std::string& error)
    {
        if (!value.IsObject()) {
            error = "a catalog profile is not an object";
            return false;
        }

        const char* const fields[] = {"id",          "metadata",     "home",
                                      "appdata",     "saves",        "xdgConfigHome",
                                      "xdgDataHome", "xdgStateHome", "xdgCacheHome"};

        for (const char* field : fields) {
            std::string text;
            if (!readString(value, field, text) || text.empty()) {
                error = std::string {"a catalog profile has no "} + field;
                return false;
            }
        }

        readString(value, "id", entry.id);
        readString(value, "metadata", entry.metadata);
        readString(value, "home", entry.home);
        readString(value, "appdata", entry.appdata);
        readString(value, "saves", entry.saves);
        readString(value, "xdgConfigHome", entry.xdgConfigHome);
        readString(value, "xdgDataHome", entry.xdgDataHome);
        readString(value, "xdgStateHome", entry.xdgStateHome);
        readString(value, "xdgCacheHome", entry.xdgCacheHome);

        return true;
    }
} // namespace

bool ProfileSession::isUuidV4(const std::string& text)
{
    if (text.size() != 36)
        return false;

    for (std::size_t index {0}; index < text.size(); ++index) {
        const char character {text[index]};
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (character != '-')
                return false;
            continue;
        }

        if (index == 14) {
            if (character != '4')
                return false;
            continue;
        }

        if (index == 19) {
            if (character != '8' && character != '9' && character != 'a' && character != 'b')
                return false;
            continue;
        }

        if (!isHex(character))
            return false;
    }

    return true;
}

ProfileSession::Context ProfileSession::fromCatalog(const std::string& catalogBytes,
                                                    const std::string& profileId)
{
    Context context;

    rapidjson::Document document;
    std::string error;
    if (!ProfileJson::parseObject(catalogBytes, document, error)) {
        context.mError = "the profile catalog is not usable: " + error;
        return context;
    }

    std::string duplicate;
    if (ProfileJson::hasDuplicateKeys(document, duplicate)) {
        context.mError = "the profile catalog has a duplicate key \"" + duplicate + "\"";
        return context;
    }

    const rapidjson::Value::ConstMemberIterator version {document.FindMember("version")};
    if (version == document.MemberEnd() || !version->value.IsInt() ||
        version->value.GetInt() != 1) {
        context.mError = "the profile catalog has an unsupported version";
        return context;
    }

    const rapidjson::Value::ConstMemberIterator entries {document.FindMember("profiles")};
    if (entries == document.MemberEnd() || !entries->value.IsArray()) {
        context.mError = "the profile catalog has no profiles";
        return context;
    }

    if (!readString(document, "default", context.mCatalog.defaultProfile) ||
        context.mCatalog.defaultProfile.empty()) {
        context.mError = "the profile catalog has no default profile";
        return context;
    }

    if (!readString(document, "libraryRoot", context.mCatalog.libraryRoot) ||
        context.mCatalog.libraryRoot.empty()) {
        context.mError = "the profile catalog has no library root";
        return context;
    }

    if (!readString(document, "avatarManifest", context.mCatalog.avatarManifest) ||
        context.mCatalog.avatarManifest.empty()) {
        context.mError = "the profile catalog has no avatar manifest";
        return context;
    }

    std::set<std::string> ids;
    for (const rapidjson::Value& value : entries->value.GetArray()) {
        CatalogEntry entry;
        if (!readEntry(value, entry, error)) {
            context.mError = error;
            return context;
        }

        if (!isUuidV4(entry.id)) {
            context.mError = "the profile catalog has a malformed profile id \"" + entry.id + "\"";
            return context;
        }

        if (!ids.insert(entry.id).second) {
            context.mError = "the profile catalog has a duplicate profile id \"" + entry.id + "\"";
            return context;
        }

        context.mCatalog.profiles.emplace_back(std::move(entry));
    }

    if (ids.count(context.mCatalog.defaultProfile) == 0) {
        context.mError = "the profile catalog default is not a profile";
        return context;
    }

    // An empty id means "catalog only": the chooser has no profile identity but still needs the
    // declared profile ids to validate its selection.
    if (profileId.empty())
        return context;

    if (!isUuidV4(profileId)) {
        context.mError = "the profile id \"" + profileId + "\" is not a canonical UUIDv4";
        return context;
    }

    for (const CatalogEntry& entry : context.mCatalog.profiles) {
        if (entry.id == profileId) {
            context.mEntry = entry;
            context.mActive = true;
            return context;
        }
    }

    context.mError = "the profile \"" + profileId + "\" is not in the catalog";
    return context;
}

bool ProfileSession::readCatalog(const std::string& catalogBytes,
                                 Catalog& catalog,
                                 std::string& error)
{
    Context context {fromCatalog(catalogBytes, "")};
    if (!context.error().empty()) {
        error = context.error();
        return false;
    }
    catalog = context.catalog();
    return true;
}

ProfileSession::Context ProfileSession::fromEnvironment(ProfileState::FileOperations& files)
{
    Context context;

    const char* catalogPath {getenv("NITROPIE_PROFILE_CATALOG")};
    const char* profileId {getenv("NITROPIE_PROFILE_ID")};

    const bool hasCatalog {catalogPath != nullptr && *catalogPath != '\0'};
    const bool hasProfile {profileId != nullptr && *profileId != '\0'};

    // Legacy mode: no profile environment at all.
    if (!hasCatalog && !hasProfile)
        return context;

    if (!hasCatalog || !hasProfile) {
        context.mError = "the profile environment is incomplete";
        return context;
    }

    std::string bytes;
    if (files.readFile(catalogPath, bytes) != ProfileState::ReadStatus::ok) {
        context.mError = std::string {"could not read the profile catalog "} + catalogPath;
        return context;
    }

    return fromCatalog(bytes, profileId);
}
