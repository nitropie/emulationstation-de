//  SPDX-License-Identifier: MIT
//
//  bad-penguin 6B: existing-fork gamelist split and reconstruction.
//
//  See MigrationSplit.h. This translation unit never opens a file: it works on
//  bytes supplied by the caller, so the command can keep every source open
//  descriptor-relative and no-follow.
//

#include "MigrationSplit.h"

#include "FileData.h"
#include "GamelistFileParser.h"
#include "MetaData.h"
#include "Settings.h"
#include "SystemData.h"
#include "profiles/ProfileRecordKey.h"
#include "profiles/ProfileStateJson.h"

#include <pugixml.hpp>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    using rapidjson::StringBuffer;
    using rapidjson::Writer;

    const char* tagFor(const ProfileRecordType type)
    {
        return type == ProfileRecordType::game ? "game" : "folder";
    }

    MetaDataListType listTypeFor(const ProfileRecordType type)
    {
        return type == ProfileRecordType::game ? GAME_METADATA : FOLDER_METADATA;
    }

    // Compact JSON string text, validating UTF-8 like the state writer does.
    std::string jsonString(const std::string& value)
    {
        StringBuffer buffer;
        Writer<StringBuffer, rapidjson::UTF8<>, rapidjson::UTF8<>, rapidjson::CrtAllocator,
               rapidjson::kWriteValidateEncodingFlag>
            writer {buffer};
        writer.String(value.c_str(), static_cast<rapidjson::SizeType>(value.size()));
        if (!writer.IsComplete())
            return {};
        return std::string {buffer.GetString(), buffer.GetSize()};
    }

    // The value the declared type can represent exactly. MD_BOOL is stricter
    // than jsonValueFromMetadata, which would map any other text to false and
    // so coerce the legacy value.
    bool typedValue(const MetaDataDecl& decl, const std::string& raw, std::string& json)
    {
        if (decl.type == MD_BOOL) {
            if (raw != "true" && raw != "false")
                return false;
            json = raw;
            return true;
        }
        if (!ProfileState::isEncodableValue(decl, raw))
            return false;
        return ProfileState::jsonValueFromMetadata(decl, raw, json);
    }

    struct Entry {
        Entry(ProfileRecordType recordType,
              std::string relativePath,
              std::string absolutePath,
              pugi::xml_node xmlNode)
            : type(recordType)
            , relative(std::move(relativePath))
            , absolute(std::move(absolutePath))
            , node(xmlNode)
            , metadata(listTypeFor(recordType))
        {
        }

        ProfileRecordType type;
        std::string relative;
        std::string absolute;
        pugi::xml_node node;
        MetaDataList metadata;
        bool keyValid {false};
    };

    bool parseGameList(const std::string& bytes, pugi::xml_document& document, std::string& error)
    {
        if (bytes.empty()) {
            error = "gamelist is empty";
            return false;
        }
        const pugi::xml_parse_result parsed {
            document.load_buffer(bytes.data(), bytes.size())};
        if (!parsed) {
            error = std::string {"gamelist is not valid XML: "} + parsed.description();
            return false;
        }
        if (!document.child("gameList")) {
            error = "gamelist has no gameList root";
            return false;
        }
        return true;
    }

    // True when a `..` segment would leave the system root. The record key
    // already rejects it; the split refuses instead, so a gamelist entry that
    // escapes the root never produces staged output.
    bool escapesRoot(const std::string& relative)
    {
        std::size_t start {0};
        while (start <= relative.size()) {
            const std::size_t end {relative.find('/', start)};
            const std::string part {
                relative.substr(start, end == std::string::npos ? std::string::npos : end - start)};
            if (part == "..")
                return true;
            if (end == std::string::npos)
                return false;
            start = end + 1;
        }
        return false;
    }

    // Resolve a <path> the way the legacy reader does, and keep the result for
    // path maths only. Returns false when the text cannot be placed.
    bool resolveRecordPath(const std::string& text,
                           const std::string& romRoot,
                           std::string& relative,
                           std::string& absolute)
    {
        if (text.empty())
            return false;
        if (text.rfind("./", 0) == 0) {
            relative = text.substr(2);
            if (escapesRoot(relative))
                return false;
            absolute = romRoot + "/" + relative;
            return true;
        }
        if (text.front() == '/') {
            absolute = text;
            if (text == romRoot)
                return false;
            const std::string prefix {romRoot + "/"};
            if (text.rfind(prefix, 0) != 0)
                return false;
            relative = text.substr(prefix.size());
            return true;
        }
        relative = text;
        if (escapesRoot(relative))
            return false;
        absolute = romRoot + "/" + text;
        return true;
    }

    bool collectEntries(const std::string& system,
                        const std::string& romRoot,
                        pugi::xml_node root,
                        std::vector<Entry>& entries,
                        std::string& error)
    {
        std::set<std::tuple<int, std::string>> seen;
        for (pugi::xml_node child : root.children()) {
            const std::string tag {child.name()};
            if (tag != "game" && tag != "folder")
                continue;
            const ProfileRecordType type {
                tag == "game" ? ProfileRecordType::game : ProfileRecordType::folder};
            const pugi::xml_node pathNode {child.child("path")};
            if (!pathNode)
                return error = "a " + tag + " entry has no path", false;

            std::string relative;
            std::string absolute;
            if (!resolveRecordPath(pathNode.text().get(), romRoot, relative, absolute))
                return error = "a " + tag + " entry path is outside the system root", false;

            const auto key {ProfileRecordKey::fromPath(system, type, relative)};
            if (!seen.insert({static_cast<int>(type), relative}).second)
                return error = "duplicate record key: " + relative, false;

            Entry entry {type, relative, absolute, child};
            entry.keyValid = key.has_value();
            entry.metadata = MetaDataList::createFromXML(listTypeFor(type), child, romRoot);
            entries.push_back(std::move(entry));
        }
        return true;
    }

    std::vector<std::string> unknownChildren(pugi::xml_node node, MetaDataListType type)
    {
        std::vector<std::string> unknown;
        for (pugi::xml_node child : node.children()) {
            const std::string name {child.name()};
            if (name == "path")
                continue;
            bool declared {false};
            for (const MetaDataDecl& decl : getMDDByType(type)) {
                if (decl.key == name) {
                    declared = true;
                    break;
                }
            }
            if (declared)
                continue;
            std::ostringstream stream;
            child.print(stream, "", pugi::format_default);
            unknown.push_back(stream.str());
        }
        return unknown;
    }

    void prepareSettings()
    {
        Settings* settings {Settings::getInstance()};
        // The tool must never scan ROMs or read the operator's gamelist through
        // the loader; it reads the bytes it was given and builds records itself.
        settings->setBool("ParseGamelistOnly", true);
        settings->setBool("IgnoreGamelist", true);
        settings->setBool("ShowHiddenGames", true);
        settings->setBool("ShowHiddenFiles", true);
        settings->setString("SaveGamelistsMode", "never");
    }

    void readPreferences(pugi::xml_document& document,
                         ProfileState::JsonDocument& state,
                         MigrationSplit::SystemResult& result)
    {
        pugi::xml_node labelNode {document.child("alternativeEmulator")};
        if (!labelNode)
            labelNode = document.child("gameList").child("alternativeEmulator");
        if (labelNode) {
            const std::string label {labelNode.child("label").text().get()};
            if (!label.empty()) {
                if (!isValidUtf8(label)) {
                    result.items.push_back({result.system, "preference",
                                            "alternativeEmulator", "",
                                            "label is not valid UTF-8; not persisted", true});
                }
                else {
                    state.hasAlternativeEmulator = true;
                    state.alternativeEmulatorValid = true;
                    state.alternativeEmulator = label;
                }
            }
        }

        const pugi::xml_node screenNode {
            document.child("gameList").child("launchOnOtherScreen")};
        if (screenNode) {
            const std::string value {screenNode.text().get()};
            if (value == "false") {
                state.hasLaunchOnOtherScreen = true;
                state.launchOnOtherScreenValid = true;
                state.launchOnOtherScreen = false;
            }
            else if (value == "true" || value.empty()) {
                // The default is true and is never written.
            }
            else if (!isValidUtf8(value)) {
                result.items.push_back({result.system, "preference", "launchOnOtherScreen", "",
                                        "value is not valid UTF-8; not persisted", true});
            }
            else {
                state.hasLaunchOnOtherScreen = true;
                state.launchOnOtherScreenValid = false;
                state.launchOnOtherScreenRaw = jsonString(value);
                result.items.push_back({result.system, "preference", "launchOnOtherScreen", "",
                                        "wrong JSON type; kept verbatim", false});
            }
        }
    }

    bool collectPersonal(const Entry& entry,
                         ProfileState::JsonEntry& json,
                         MigrationSplit::SystemResult& result)
    {
        for (const MetaDataDecl& decl : getMDDByType(listTypeFor(entry.type))) {
            if (!isPersonalMetadata(decl))
                continue;
            const pugi::xml_node child {entry.node.child(decl.key.c_str())};
            if (!child)
                continue;
            const std::string raw {child.text().get()};
            if (raw.empty())
                continue;

            std::string jsonValue;
            if (typedValue(decl, raw, jsonValue)) {
                if (raw == decl.defaultValue)
                    continue;
                json.fields.push_back({decl.key, jsonValue, true});
                continue;
            }

            if (!isValidUtf8(raw)) {
                result.items.push_back({result.system, "entry", entry.relative, decl.key,
                                        "value is not valid UTF-8; not persisted", true});
                continue;
            }
            json.fields.push_back({decl.key, jsonString(raw), false});
            result.items.push_back({result.system, "entry", entry.relative, decl.key,
                                    "wrong JSON type; kept verbatim", false});
        }
        return true;
    }

    // Apply an already validated overlay field to a metadata copy, using the
    // fork's own conversion so the effective value matches the runtime.
    bool applyOverlayField(const Entry& entry,
                           const std::string& key,
                           const std::string& json,
                           MetaDataList& target)
    {
        for (const MetaDataDecl& decl : getMDDByType(listTypeFor(entry.type))) {
            if (decl.key != key)
                continue;
            std::string value;
            if (!ProfileState::metadataFromJsonValue(decl, json, value))
                return false;
            target.setApplied(decl.key, value);
            return true;
        }
        return false;
    }

    const ProfileState::JsonEntry* findStateEntry(const ProfileState::JsonDocument& state,
                                                  ProfileRecordType type,
                                                  const std::string& path)
    {
        for (const ProfileState::JsonEntry& entry : state.entries) {
            if (entry.type == type && entry.path == path)
                return &entry;
        }
        return nullptr;
    }

    bool compareShared(const Entry& original,
                       const Entry& shared,
                       std::string& error)
    {
        for (const MetaDataDecl& decl : getMDDByType(listTypeFor(original.type))) {
            if (isPersonalMetadata(decl))
                continue;
            if (original.metadata.get(decl.key) != shared.metadata.get(decl.key)) {
                error = "shared field \"" + decl.key + "\" differs for \"" + original.relative +
                        "\"";
                return false;
            }
        }
        const std::vector<std::string> before {
            unknownChildren(original.node, listTypeFor(original.type))};
        const std::vector<std::string> after {
            unknownChildren(shared.node, listTypeFor(shared.type))};
        if (before != after) {
            error = "unknown children differ for \"" + original.relative + "\"";
            return false;
        }
        return true;
    }

    bool compareRecordKey(const std::string& system,
                          const Entry& entry,
                          std::string& error)
    {
        const auto key {ProfileRecordKey::fromPath(system, entry.type, entry.relative)};
        if (!key.has_value()) {
            error = "record key no longer resolves: \"" + entry.relative + "\"";
            return false;
        }
        return true;
    }
} // namespace

bool MigrationSplit::Outcome::hasBlocking() const
{
    for (const SystemResult& system : systems) {
        for (const Item& item : system.items) {
            if (item.blocking)
                return true;
        }
    }
    return false;
}

bool MigrationSplit::splitSystem(const SystemRequest& request,
                                 const std::string& gamelistBytes,
                                 std::string& sharedXml,
                                 std::string& stateJson,
                                 SystemResult& result,
                                 std::string& error)
{
    prepareSettings();

    pugi::xml_document document;
    if (!parseGameList(gamelistBytes, document, error))
        return false;
    const pugi::xml_node root {document.child("gameList")};

    std::vector<Entry> entries;
    if (!collectEntries(request.system, request.romRoot, root, entries, error))
        return false;

    result.system = request.system;
    result.source = request.source;

    ProfileState::JsonDocument state;
    state.system = request.system;
    readPreferences(document, state, result);

    for (const Entry& entry : entries) {
        ProfileState::JsonEntry json;
        json.type = entry.type;
        json.path = entry.relative;
        json.keyValid = entry.keyValid;
        collectPersonal(entry, json, result);
        if (!entry.keyValid) {
            result.items.push_back({result.system, "entry", entry.relative, "",
                                    "record key does not resolve below the system root", true});
        }
        if (json.fields.empty())
            continue;
        if (!isValidUtf8(json.path)) {
            result.items.push_back({result.system, "entry", entry.relative, "",
                                    "record path is not valid UTF-8; not persisted", true});
            continue;
        }
        state.entries.push_back(std::move(json));
    }

    auto* environment {new SystemEnvironmentData};
    environment->mStartPath = request.romRoot;
    SystemData system {request.system, request.system, request.system, environment,
                       request.system, false, false};

    std::vector<std::unique_ptr<FileData>> records;
    std::vector<const FileData*> recordPointers;
    for (const Entry& entry : entries) {
        auto record {std::make_unique<FileData>(
            entry.type == ProfileRecordType::game ? GAME : FOLDER, entry.absolute, environment,
            &system)};
        record->metadata = entry.metadata;
        recordPointers.push_back(record.get());
        records.push_back(std::move(record));
    }

    sharedXml = GamelistFileParser::writeSharedDocument(gamelistBytes, system, recordPointers);
    if (sharedXml.empty()) {
        error = "the shared-only writer could not use the legacy gamelist";
        return false;
    }
    stateJson = ProfileState::serializeDocument(state);
    result.records = static_cast<int>(entries.size());
    result.personalEntries = static_cast<int>(state.entries.size());

    // Prove the staged bytes before the caller writes anything.
    return verifySystem(request, gamelistBytes, sharedXml, stateJson, result, error);
}

bool MigrationSplit::verifySystem(const SystemRequest& request,
                                  const std::string& gamelistBytes,
                                  const std::string& sharedXml,
                                  const std::string& stateJson,
                                  SystemResult& result,
                                  std::string& error)
{
    pugi::xml_document originalDocument;
    pugi::xml_document sharedDocument;
    if (!parseGameList(gamelistBytes, originalDocument, error))
        return false;
    if (!parseGameList(sharedXml, sharedDocument, error))
        return false;

    std::vector<Entry> originalEntries;
    std::vector<Entry> sharedEntries;
    if (!collectEntries(request.system, request.romRoot, originalDocument.child("gameList"),
                        originalEntries, error))
        return false;
    if (!collectEntries(request.system, request.romRoot, sharedDocument.child("gameList"),
                        sharedEntries, error))
        return false;

    if (originalEntries.size() != sharedEntries.size()) {
        error = "record count changed: " + std::to_string(originalEntries.size()) + " -> " +
                std::to_string(sharedEntries.size());
        return false;
    }

    std::map<std::pair<int, std::string>, const Entry*> sharedByKey;
    for (const Entry& entry : sharedEntries) {
        sharedByKey[{static_cast<int>(entry.type), entry.relative}] = &entry;
    }

    ProfileState::JsonDocument state;
    std::string loadError;
    const ProfileState::LoadResult loaded {
        ProfileState::loadDocument(stateJson, request.system, state, loadError)};
    if (loaded != ProfileState::LoadResult::loaded) {
        error = "state document is not loadable: " + loadError;
        return false;
    }
    if (state.unresolved) {
        error = "state document is too new for this reader";
        return false;
    }

    for (const Entry& original : originalEntries) {
        const auto found {sharedByKey.find({static_cast<int>(original.type), original.relative})};
        if (found == sharedByKey.end()) {
            error = "record disappeared from the shared gamelist: \"" + original.relative + "\"";
            return false;
        }
        const Entry& shared {*found->second};
        if (!compareShared(original, shared, error))
            return false;

        if (!original.keyValid)
            continue;
        if (!compareRecordKey(request.system, original, error))
            return false;

        MetaDataList rebuilt {shared.metadata};
        const ProfileState::JsonEntry* stateEntry {
            findStateEntry(state, original.type, original.relative)};

        std::set<std::string> handled;
        for (const MetaDataDecl& decl : getMDDByType(listTypeFor(original.type))) {
            if (!isPersonalMetadata(decl))
                continue;
            const pugi::xml_node child {original.node.child(decl.key.c_str())};
            const std::string raw {child ? std::string {child.text().get()} : std::string {}};

            if (raw.empty()) {
                if (stateEntry != nullptr) {
                    for (const ProfileState::JsonField& field : stateEntry->fields) {
                        if (field.key == decl.key) {
                            error = "unexpected personal field \"" + decl.key + "\" for \"" +
                                    original.relative + "\"";
                            return false;
                        }
                    }
                }
                continue;
            }

            std::string expected;
            if (typedValue(decl, raw, expected)) {
                if (raw == decl.defaultValue)
                    continue;
                bool foundField {false};
                if (stateEntry != nullptr) {
                    for (const ProfileState::JsonField& field : stateEntry->fields) {
                        if (field.key != decl.key)
                            continue;
                        foundField = true;
                        if (!field.typeValid || field.value != expected) {
                            error = "personal field \"" + decl.key + "\" is not the legacy value for \"" +
                                    original.relative + "\"";
                            return false;
                        }
                        if (!applyOverlayField(original, field.key, field.value, rebuilt)) {
                            error = "overlay field \"" + decl.key + "\" cannot be applied for \"" +
                                    original.relative + "\"";
                            return false;
                        }
                        handled.insert(field.key);
                    }
                }
                if (!foundField) {
                    error = "personal field \"" + decl.key + "\" is missing for \"" +
                            original.relative + "\"";
                    return false;
                }
                if (rebuilt.get(decl.key) != original.metadata.get(decl.key)) {
                    error = "personal field \"" + decl.key + "\" does not reconstruct for \"" +
                            original.relative + "\"";
                    return false;
                }
            }
            else if (isValidUtf8(raw)) {
                bool foundField {false};
                if (stateEntry != nullptr) {
                    for (const ProfileState::JsonField& field : stateEntry->fields) {
                        if (field.key != decl.key)
                            continue;
                        foundField = true;
                        if (field.typeValid || field.value != jsonString(raw)) {
                            error = "wrong-typed field \"" + decl.key + "\" was not kept verbatim for \"" +
                                    original.relative + "\"";
                            return false;
                        }
                        handled.insert(field.key);
                    }
                }
                if (!foundField) {
                    error = "wrong-typed field \"" + decl.key + "\" disappeared for \"" +
                            original.relative + "\"";
                    return false;
                }
            }
        }

        if (stateEntry != nullptr) {
            for (const ProfileState::JsonField& field : stateEntry->fields) {
                if (handled.count(field.key) != 0)
                    continue;
                error = "state has an unreconciled field \"" + field.key + "\"";
                return false;
            }
        }
    }

    for (const ProfileState::JsonEntry& entry : state.entries) {
        if (!entry.keyValid)
            continue;
        bool found {false};
        for (const Entry& original : originalEntries) {
            if (original.type == entry.type && original.relative == entry.path &&
                original.keyValid) {
                found = true;
                break;
            }
        }
        if (!found) {
            error = "state has an entry with no source record: \"" + entry.path + "\"";
            return false;
        }
    }

    // Preferences: the legacy effective values must agree with the overlay.
    const pugi::xml_node labelNode {
        originalDocument.child("alternativeEmulator")
            ? originalDocument.child("alternativeEmulator")
            : originalDocument.child("gameList").child("alternativeEmulator")};
    const std::string label {labelNode && labelNode.child("label")
                                 ? labelNode.child("label").text().get()
                                 : std::string {}};
    if (isValidUtf8(label) && label != state.alternativeEmulator) {
        error = "alternative emulator preference did not reconstruct";
        return false;
    }

    const pugi::xml_node screenNode {
        originalDocument.child("gameList").child("launchOnOtherScreen")};
    const std::string screenValue {screenNode ? screenNode.text().get() : std::string {}};
    if (screenValue == "false") {
        if (!state.hasLaunchOnOtherScreen || !state.launchOnOtherScreenValid ||
            state.launchOnOtherScreen)
            return error = "launch-on-other-screen preference did not reconstruct", false;
    }
    else if (screenValue == "true" || screenValue.empty()) {
        if (state.hasLaunchOnOtherScreen &&
            (state.launchOnOtherScreenValid || state.launchOnOtherScreenRaw.empty()))
            return error = "default launch-on-other-screen preference was persisted", false;
    }

    return true;
}
