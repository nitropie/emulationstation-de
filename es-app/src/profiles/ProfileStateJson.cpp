//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileStateJson.cpp
//
//  JSON document for one system's personal state (design section 6.2).
//

#include "profiles/ProfileStateJson.h"

#include "profiles/ProfileJson.h"

#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <set>

namespace
{
    using rapidjson::Value;

    bool isInteger(const Value& value)
    {
        return value.IsInt() || value.IsInt64() || value.IsUint() || value.IsUint64();
    }

    // Accepts only 0..INT64_MAX: a negative or too-large playcount is a wrong-typed field.
    bool integerValue(const Value& value, long long& result)
    {
        if (value.IsUint64()) {
            if (value.GetUint64() > static_cast<unsigned long long>(INT64_MAX))
                return false;
            result = static_cast<long long>(value.GetUint64());
            return true;
        }
        if (value.IsInt64()) {
            result = value.GetInt64();
            return result >= 0;
        }
        if (value.IsUint()) {
            result = static_cast<long long>(value.GetUint());
            return true;
        }
        if (value.IsInt()) {
            result = static_cast<long long>(value.GetInt());
            return result >= 0;
        }
        return false;
    }

    bool isTimeValue(const std::string& text)
    {
        if (text.size() != 15 || text[8] != 'T')
            return false;

        for (std::size_t index {0}; index < text.size(); ++index) {
            if (index == 8)
                continue;
            if (text[index] < '0' || text[index] > '9')
                return false;
        }

        return true;
    }

} // namespace

const MetaDataDecl* ProfileState::personalDecl(const ProfileRecordType type, const std::string& key)
{
    const MetaDataListType listType {type == ProfileRecordType::game ? GAME_METADATA :
                                                                       FOLDER_METADATA};

    for (const MetaDataDecl& decl : getMDDByType(listType)) {
        if (decl.key == key)
            return isPersonalMetadata(decl) ? &decl : nullptr;
    }

    return nullptr;
}

bool ProfileState::jsonMatchesMetadataType(const MetaDataDecl& decl, const std::string& json)
{
    rapidjson::Document parsed;
    if (parsed.Parse<rapidjson::kParseValidateEncodingFlag>(json.c_str(), json.size())
            .HasParseError())
        return false;

    switch (decl.type) {
        case MD_BOOL:
            return parsed.IsBool();
        case MD_INT: {
            long long value {0};
            return integerValue(parsed, value);
        }
        case MD_TIME:
            return parsed.IsString() && isTimeValue(parsed.GetString());
        default:
            return parsed.IsString();
    }
}

bool ProfileState::isEncodableValue(const MetaDataDecl& decl, const std::string& value)
{
    switch (decl.type) {
        case MD_BOOL:
            return true;
        case MD_INT: {
            char* end {nullptr};
            errno = 0;
            const long long number {std::strtoll(value.c_str(), &end, 10)};
            return errno == 0 && end != value.c_str() && *end == '\0' && number >= 0;
        }
        case MD_TIME:
            return isTimeValue(value);
        default:
            return isValidUtf8(value);
    }
}

bool ProfileState::jsonValueFromMetadata(const MetaDataDecl& decl,
                                         const std::string& value,
                                         std::string& json)
{
    if (!isEncodableValue(decl, value))
        return false;

    switch (decl.type) {
        case MD_BOOL:
            json = (value == "true") ? "true" : "false";
            return true;
        case MD_INT: {
            char* end {nullptr};
            errno = 0;
            const long long number {std::strtoll(value.c_str(), &end, 10)};
            if (errno != 0 || end == value.c_str() || *end != '\0' || number < 0)
                return false;
            json = std::to_string(number);
            return true;
        }
        case MD_TIME:
            // The reader rejects anything else, so the writer must not produce it either.
            if (!isTimeValue(value))
                return false;
            [[fallthrough]];
        default: {
            if (!isValidUtf8(value))
                return false;

            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer, rapidjson::UTF8<>, rapidjson::UTF8<>,
                              rapidjson::CrtAllocator, rapidjson::kWriteValidateEncodingFlag>
                writer {buffer};
            writer.String(value.c_str(), static_cast<rapidjson::SizeType>(value.size()));
            if (!writer.IsComplete())
                return false;
            json.assign(buffer.GetString(), buffer.GetSize());
            return true;
        }
    }
}

bool ProfileState::metadataFromJsonValue(const MetaDataDecl& decl,
                                         const std::string& json,
                                         std::string& value)
{
    rapidjson::Document parsed;
    if (parsed.Parse<rapidjson::kParseValidateEncodingFlag>(json.c_str(), json.size())
            .HasParseError())
        return false;

    switch (decl.type) {
        case MD_BOOL:
            if (!parsed.IsBool())
                return false;
            value = parsed.GetBool() ? "true" : "false";
            return true;
        case MD_INT: {
            long long number {0};
            if (!integerValue(parsed, number))
                return false;
            value = std::to_string(number);
            return true;
        }
        case MD_TIME:
            if (!parsed.IsString() || !isTimeValue(parsed.GetString()))
                return false;
            value = parsed.GetString();
            return true;
        default:
            if (!parsed.IsString())
                return false;
            value = parsed.GetString();
            return true;
    }
}

ProfileState::LoadResult ProfileState::loadDocument(const std::string& bytes,
                                                    const std::string& system,
                                                    JsonDocument& document,
                                                    std::string& error)
{
    document = JsonDocument {};
    document.system = system;
    error.clear();

    rapidjson::Document parsed;
    if (!ProfileJson::parseObject(bytes, parsed, error))
        return LoadResult::invalid;

    std::string duplicate;
    if (ProfileJson::hasDuplicateKeys(parsed, duplicate)) {
        error = "duplicate top-level key \"" + duplicate + "\"";
        return LoadResult::invalid;
    }

    const Value::ConstMemberIterator schema {parsed.FindMember("schema")};
    if (schema == parsed.MemberEnd() || !schema->value.IsString() ||
        std::string {schema->value.GetString()} != sStateSchema) {
        error = "the state file schema is not \"" + std::string {sStateSchema} + "\"";
        return LoadResult::invalid;
    }

    const Value::ConstMemberIterator fileSystem {parsed.FindMember("system")};
    if (fileSystem == parsed.MemberEnd() || !fileSystem->value.IsString() ||
        system != fileSystem->value.GetString()) {
        error = "the state file belongs to another system";
        return LoadResult::invalid;
    }

    const Value::ConstMemberIterator versionMember {parsed.FindMember("version")};
    if (versionMember == parsed.MemberEnd() || !isInteger(versionMember->value)) {
        error = "the state file has no integer version";
        return LoadResult::invalid;
    }

    long long versionNumber {0};
    if (!integerValue(versionMember->value, versionNumber)) {
        error = "the state file has an unsupported version";
        return LoadResult::invalid;
    }

    if (versionNumber < 1) {
        error = "the state file has an unsupported version " + std::to_string(versionNumber);
        return LoadResult::invalid;
    }

    if (versionNumber > sStateVersion) {
        document.version = static_cast<int>(versionNumber);
        document.unresolved = true;
        document.fileErrors.emplace_back("the state file has version " +
                                         std::to_string(versionNumber) + ", newer than " +
                                         std::to_string(sStateVersion));
        return LoadResult::loaded;
    }

    document.version = static_cast<int>(versionNumber);

    const Value::ConstMemberIterator preferences {parsed.FindMember("systemPreferences")};
    if (preferences != parsed.MemberEnd()) {
        if (!preferences->value.IsObject()) {
            error = "systemPreferences is not an object";
            return LoadResult::invalid;
        }

        if (ProfileJson::hasDuplicateKeys(preferences->value, duplicate)) {
            error = "duplicate key \"" + duplicate + "\" in systemPreferences";
            return LoadResult::invalid;
        }

        for (Value::ConstMemberIterator member {preferences->value.MemberBegin()};
             member != preferences->value.MemberEnd(); ++member) {
            const std::string key {member->name.GetString()};
            if (key == "alternativeEmulator") {
                document.hasAlternativeEmulator = true;
                if (member->value.IsString()) {
                    document.alternativeEmulatorValid = true;
                    document.alternativeEmulator = member->value.GetString();
                }
                else {
                    document.alternativeEmulatorRaw = ProfileJson::fragment(member->value);
                    document.fileErrors.emplace_back(
                        "systemPreferences.alternativeEmulator is not a string");
                }
            }
            else if (key == "launchOnOtherScreen") {
                document.hasLaunchOnOtherScreen = true;
                if (member->value.IsBool()) {
                    document.launchOnOtherScreenValid = true;
                    document.launchOnOtherScreen = member->value.GetBool();
                }
                else {
                    document.launchOnOtherScreenRaw = ProfileJson::fragment(member->value);
                    document.fileErrors.emplace_back(
                        "systemPreferences.launchOnOtherScreen is not a boolean");
                }
            }
            else {
                document.extraPreferences.push_back(
                    {key, ProfileJson::fragment(member->value), true});
            }
        }
    }

    const Value::ConstMemberIterator entries {parsed.FindMember("entries")};
    if (entries != parsed.MemberEnd()) {
        if (!entries->value.IsArray()) {
            error = "entries is not an array";
            return LoadResult::invalid;
        }

        std::set<std::pair<int, std::string>> seenKeys;

        for (const Value& value : entries->value.GetArray()) {
            if (!value.IsObject()) {
                error = "an entry is not an object";
                return LoadResult::invalid;
            }

            if (ProfileJson::hasDuplicateKeys(value, duplicate)) {
                error = "duplicate key \"" + duplicate + "\" in an entry";
                return LoadResult::invalid;
            }

            const Value::ConstMemberIterator typeMember {value.FindMember("type")};
            const Value::ConstMemberIterator pathMember {value.FindMember("path")};
            const Value::ConstMemberIterator fields {value.FindMember("fields")};
            if (typeMember == value.MemberEnd() || !typeMember->value.IsString() ||
                pathMember == value.MemberEnd() || !pathMember->value.IsString() ||
                fields == value.MemberEnd() || !fields->value.IsObject()) {
                error = "an entry is missing its type, path or fields";
                return LoadResult::invalid;
            }

            if (ProfileJson::hasDuplicateKeys(fields->value, duplicate)) {
                error = "duplicate key \"" + duplicate + "\" in an entry's fields";
                return LoadResult::invalid;
            }

            const std::string typeString {typeMember->value.GetString()};
            if (typeString != "game" && typeString != "folder") {
                error = "an entry has an unknown type \"" + typeString + "\"";
                return LoadResult::invalid;
            }

            JsonEntry entry;
            entry.type = typeString == "game" ? ProfileRecordType::game : ProfileRecordType::folder;
            entry.path = pathMember->value.GetString();
            entry.keyValid = ProfileRecordKey::fromPath(system, entry.type, entry.path).has_value();

            if (!seenKeys.insert({static_cast<int>(entry.type), entry.path}).second) {
                error = "duplicate entry for \"" + entry.path + "\"";
                return LoadResult::invalid;
            }

            for (Value::ConstMemberIterator member {value.MemberBegin()};
                 member != value.MemberEnd(); ++member) {
                const std::string key {member->name.GetString()};
                if (key == "type" || key == "path" || key == "fields")
                    continue;
                entry.extraKeys.push_back({key, ProfileJson::fragment(member->value), true});
            }

            for (Value::ConstMemberIterator member {fields->value.MemberBegin()};
                 member != fields->value.MemberEnd(); ++member) {
                const std::string key {member->name.GetString()};
                JsonField field {key, ProfileJson::fragment(member->value), true};

                const MetaDataDecl* decl {personalDecl(entry.type, key)};
                if (decl != nullptr && !jsonMatchesMetadataType(*decl, field.value))
                    field.typeValid = false;

                entry.fields.emplace_back(std::move(field));
            }

            document.entries.emplace_back(std::move(entry));
        }
    }

    for (Value::ConstMemberIterator member {parsed.MemberBegin()}; member != parsed.MemberEnd();
         ++member) {
        const std::string key {member->name.GetString()};
        if (key == "schema" || key == "version" || key == "system" || key == "systemPreferences" ||
            key == "entries")
            continue;
        document.extraTop.push_back({key, ProfileJson::fragment(member->value), true});
    }

    document.errors = recordErrors(document);
    return LoadResult::loaded;
}

std::vector<std::string> ProfileState::recordErrors(const JsonDocument& document)
{
    std::vector<std::string> errors;
    for (const JsonEntry& entry : document.entries) {
        if (!entry.keyValid)
            errors.emplace_back("record \"" + entry.path + "\" does not resolve");

        for (const JsonField& field : entry.fields) {
            if (!field.typeValid) {
                errors.emplace_back("record \"" + entry.path + "\": field \"" + field.key +
                                    "\" has the wrong JSON type");
            }
        }
    }

    return errors;
}

std::string ProfileState::serializeDocument(const JsonDocument& document)
{
    rapidjson::StringBuffer buffer;
    ProfileJson::Writer writer {buffer};
    writer.SetIndent(' ', 2);

    writer.StartObject();
    writer.Key("schema");
    writer.String(sStateSchema);
    writer.Key("version");
    writer.Int(sStateVersion);
    writer.Key("system");
    writer.String(document.system.c_str(),
                  static_cast<rapidjson::SizeType>(document.system.size()));

    writer.Key("systemPreferences");
    writer.StartObject();
    if (document.hasAlternativeEmulator) {
        writer.Key("alternativeEmulator");
        if (document.alternativeEmulatorValid)
            writer.String(document.alternativeEmulator.c_str(),
                          static_cast<rapidjson::SizeType>(document.alternativeEmulator.size()));
        else
            ProfileJson::emitFragment(writer, document.alternativeEmulatorRaw);
    }
    if (document.hasLaunchOnOtherScreen) {
        writer.Key("launchOnOtherScreen");
        if (document.launchOnOtherScreenValid)
            writer.Bool(document.launchOnOtherScreen);
        else
            ProfileJson::emitFragment(writer, document.launchOnOtherScreenRaw);
    }
    {
        std::vector<JsonField> extra {document.extraPreferences};
        std::sort(extra.begin(), extra.end(), [](const JsonField& left, const JsonField& right) {
            return left.key < right.key;
        });
        for (const JsonField& field : extra) {
            writer.Key(field.key.c_str(), static_cast<rapidjson::SizeType>(field.key.size()));
            ProfileJson::emitFragment(writer, field.value);
        }
    }
    writer.EndObject();

    writer.Key("entries");
    writer.StartArray();
    {
        std::vector<const JsonEntry*> entries;
        entries.reserve(document.entries.size());
        for (const JsonEntry& entry : document.entries)
            entries.push_back(&entry);

        std::stable_sort(entries.begin(), entries.end(),
                         [](const JsonEntry* left, const JsonEntry* right) {
                             if (left->path != right->path)
                                 return left->path < right->path;
                             return static_cast<int>(left->type) < static_cast<int>(right->type);
                         });

        for (const JsonEntry* entry : entries) {
            writer.StartObject();
            writer.Key("type");
            writer.String(entry->type == ProfileRecordType::game ? "game" : "folder");
            writer.Key("path");
            writer.String(entry->path.c_str(),
                          static_cast<rapidjson::SizeType>(entry->path.size()));

            writer.Key("fields");
            writer.StartObject();

            const MetaDataListType listType {
                entry->type == ProfileRecordType::game ? GAME_METADATA : FOLDER_METADATA};

            std::vector<std::string> written;
            for (const MetaDataDecl& decl : getMDDByType(listType)) {
                if (!isPersonalMetadata(decl))
                    continue;
                for (const JsonField& field : entry->fields) {
                    if (field.key != decl.key)
                        continue;
                    writer.Key(field.key.c_str(),
                               static_cast<rapidjson::SizeType>(field.key.size()));
                    ProfileJson::emitFragment(writer, field.value);
                    written.push_back(field.key);
                    break;
                }
            }

            std::vector<JsonField> extra;
            for (const JsonField& field : entry->fields) {
                if (std::find(written.cbegin(), written.cend(), field.key) == written.cend())
                    extra.push_back(field);
            }
            std::sort(
                extra.begin(), extra.end(),
                [](const JsonField& left, const JsonField& right) { return left.key < right.key; });
            for (const JsonField& field : extra) {
                writer.Key(field.key.c_str(), static_cast<rapidjson::SizeType>(field.key.size()));
                ProfileJson::emitFragment(writer, field.value);
            }

            writer.EndObject();

            std::vector<JsonField> extraKeys {entry->extraKeys};
            std::sort(
                extraKeys.begin(), extraKeys.end(),
                [](const JsonField& left, const JsonField& right) { return left.key < right.key; });
            for (const JsonField& field : extraKeys) {
                writer.Key(field.key.c_str(), static_cast<rapidjson::SizeType>(field.key.size()));
                ProfileJson::emitFragment(writer, field.value);
            }

            writer.EndObject();
        }
    }
    writer.EndArray();

    {
        std::vector<JsonField> extra {document.extraTop};
        std::sort(extra.begin(), extra.end(), [](const JsonField& left, const JsonField& right) {
            return left.key < right.key;
        });
        for (const JsonField& field : extra) {
            writer.Key(field.key.c_str(), static_cast<rapidjson::SizeType>(field.key.size()));
            ProfileJson::emitFragment(writer, field.value);
        }
    }

    writer.EndObject();
    buffer.Put('\n');

    return std::string {buffer.GetString(), buffer.GetSize()};
}
