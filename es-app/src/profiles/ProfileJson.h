//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileJson.h
//
//  Shared JSON helpers for the profile files: validated parsing, duplicate-key detection,
//  compact fragments and UTF-8 validated output. Both the personal-state store and the profile
//  metadata file use these so the rules live in one place.
//

#ifndef ES_APP_PROFILES_PROFILE_JSON_H
#define ES_APP_PROFILES_PROFILE_JSON_H

#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <string>

namespace ProfileJson
{
    // The writer used for every profile file: two-space indentation, UTF-8 validated output.
    using Writer = rapidjson::PrettyWriter<rapidjson::StringBuffer,
                                           rapidjson::UTF8<>,
                                           rapidjson::UTF8<>,
                                           rapidjson::CrtAllocator,
                                           rapidjson::kWriteValidateEncodingFlag>;

    // Parses a JSON object with UTF-8 validation. Returns false and sets `error` for invalid
    // JSON, a non-object root or trailing data.
    bool parseObject(const std::string& bytes, rapidjson::Document& document, std::string& error);

    // True when the object has a duplicate member name; `duplicate` is set to that name.
    bool hasDuplicateKeys(const rapidjson::Value& object, std::string& duplicate);

    // Compact JSON text of a value.
    std::string fragment(const rapidjson::Value& value);

    // Re-emits a compact fragment through a pretty writer, preserving unknown values.
    bool emitFragment(Writer& writer, const std::string& text);
} // namespace ProfileJson

#endif // ES_APP_PROFILES_PROFILE_JSON_H
