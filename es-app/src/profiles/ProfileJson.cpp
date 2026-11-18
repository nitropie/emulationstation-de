//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileJson.cpp
//
//  Shared JSON helpers for the profile files.
//

#include "profiles/ProfileJson.h"

#include "rapidjson/error/en.h"
#include "rapidjson/writer.h"

#include <set>

bool ProfileJson::parseObject(const std::string& bytes,
                              rapidjson::Document& document,
                              std::string& error)
{
    error.clear();

    if (bytes.empty()) {
        error = "the file is empty";
        return false;
    }

    const rapidjson::ParseResult result {
        document.Parse<rapidjson::kParseValidateEncodingFlag>(bytes.c_str(), bytes.size())};
    if (result.IsError()) {
        error = std::string {"invalid JSON at offset "} + std::to_string(result.Offset()) + ": " +
                rapidjson::GetParseError_En(result.Code());
        return false;
    }

    if (!document.IsObject()) {
        error = "the file root is not an object";
        return false;
    }

    return true;
}

bool ProfileJson::hasDuplicateKeys(const rapidjson::Value& object, std::string& duplicate)
{
    if (!object.IsObject())
        return false;

    std::set<std::string> keys;
    for (rapidjson::Value::ConstMemberIterator member {object.MemberBegin()};
         member != object.MemberEnd(); ++member) {
        const std::string key {member->name.GetString(), member->name.GetStringLength()};
        if (!keys.insert(key).second) {
            duplicate = key;
            return true;
        }
    }

    return false;
}

std::string ProfileJson::fragment(const rapidjson::Value& value)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer, rapidjson::UTF8<>, rapidjson::UTF8<>,
                      rapidjson::CrtAllocator, rapidjson::kWriteValidateEncodingFlag>
        writer {buffer};
    value.Accept(writer);
    return std::string {buffer.GetString(), buffer.GetSize()};
}

bool ProfileJson::emitFragment(Writer& writer, const std::string& text)
{
    rapidjson::Document parsed;
    if (parsed.Parse<rapidjson::kParseValidateEncodingFlag>(text.c_str(), text.size())
            .HasParseError())
        return false;

    parsed.Accept(writer);
    return true;
}
