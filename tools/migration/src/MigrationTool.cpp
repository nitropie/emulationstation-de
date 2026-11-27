//  SPDX-License-Identifier: MIT
//
//  es-de-migration: the 6B staging helper.
//
//  It reads a request and verified input copies under one inherited staging
//  directory descriptor, produces the shared gamelist and the initial
//  profile's state JSON through the fork serializers, re-reads them, and
//  proves the reconstruction. It writes only inside that staging descriptor
//  and never touches the migration receipt, the marker, or any authoritative
//  root.
//
//  The staging tree is disposable. A run that stops between creating a file
//  and finishing it leaves a short file; a rerun refuses it as different
//  bytes and the operator discards the staging root and starts again. No
//  repair is attempted.
//
//  The report it prints is a staging artefact and never an authority.
//

#include "MigrationSplit.h"
#include "StagingIo.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using rapidjson::Value;

    struct Request {
        std::vector<MigrationSplit::SystemRequest> systems;
        std::string profileId;
    };

    bool parseRequest(const std::string& bytes, Request& request, std::string& error)
    {
        rapidjson::Document document;
        if (document.Parse(bytes.c_str(), bytes.size()).HasParseError()) {
            error = "request is not valid JSON";
            return false;
        }
        if (!document.IsObject()) {
            error = "request root is not an object";
            return false;
        }
        const auto schema {document.FindMember("schema")};
        if (schema == document.MemberEnd() || !schema->value.IsString() ||
            std::string {schema->value.GetString()} != "nitropie.profile-split-request") {
            error = "request schema is not nitropie.profile-split-request";
            return false;
        }
        const auto version {document.FindMember("version")};
        if (version == document.MemberEnd() || !version->value.IsInt() ||
            version->value.GetInt() != 1) {
            error = "request version is not 1";
            return false;
        }
        const auto profile {document.FindMember("initialProfileId")};
        if (profile == document.MemberEnd() || !profile->value.IsString()) {
            error = "request has no initialProfileId";
            return false;
        }
        request.profileId = profile->value.GetString();
        const auto systems {document.FindMember("systems")};
        if (systems == document.MemberEnd() || !systems->value.IsArray() ||
            systems->value.Empty()) {
            error = "request has no systems";
            return false;
        }
        for (const Value& item : systems->value.GetArray()) {
            if (!item.IsObject()) {
                error = "a system request is not an object";
                return false;
            }
            MigrationSplit::SystemRequest system;
            const std::pair<const char*, std::string*> fields[] {
                {"system", &system.system},
                {"profileId", &system.profileId},
                {"romRoot", &system.romRoot},
                {"source", &system.source},
                {"gamelist", &system.gamelist},
                {"digest", &system.digest},
                {"shared", &system.shared},
                {"state", &system.state},
            };
            for (const auto& field : fields) {
                const auto member {item.FindMember(field.first)};
                if (member == item.MemberEnd() || !member->value.IsString()) {
                    error = std::string {"system request has no "} + field.first;
                    return false;
                }
                *field.second = member->value.GetString();
            }
            if (system.system.empty() || system.profileId != request.profileId ||
                system.romRoot.empty() || !MigrationSplit::validDigest(system.digest) ||
                !MigrationSplit::validRelative(system.gamelist) ||
                !MigrationSplit::validRelative(system.shared) ||
                !MigrationSplit::validRelative(system.state)) {
                error = "system request has an invalid system, profile, digest or staging name";
                return false;
            }
            if (system.source != "appdata" && system.source != "legacy") {
                error = "system request source is not appdata or legacy";
                return false;
            }
            request.systems.push_back(std::move(system));
        }
        return true;
    }

    struct MergeJob {
        std::vector<MigrationSplit::MergeRequest> systems;
        std::string profileId;
    };

    bool isMember(const std::set<std::string>& allowed,
                  const rapidjson::Value& object,
                  std::string& unknown)
    {
        for (rapidjson::Value::ConstMemberIterator member {object.MemberBegin()};
             member != object.MemberEnd(); ++member) {
            if (allowed.count(member->name.GetString()) == 0) {
                unknown = member->name.GetString();
                return false;
            }
        }
        return true;
    }

    bool stringOf(const rapidjson::Value& object,
                  const char* name,
                  std::string& value,
                  std::string& error)
    {
        const auto member {object.FindMember(name)};
        if (member == object.MemberEnd() || !member->value.IsString()) {
            error = std::string {"merge request has no "} + name;
            return false;
        }
        value = member->value.GetString();
        return true;
    }

    bool parseMergeRequest(const std::string& bytes, MergeJob& job, std::string& error)
    {
        rapidjson::Document document;
        if (document.Parse(bytes.c_str(), bytes.size()).HasParseError()) {
            error = "request is not valid JSON";
            return false;
        }
        if (!document.IsObject()) {
            error = "request root is not an object";
            return false;
        }
        std::string unknown;
        if (!isMember({"schema", "version", "profileId", "systems"}, document, unknown)) {
            error = "request has an unknown member: " + unknown;
            return false;
        }
        const auto schema {document.FindMember("schema")};
        if (schema == document.MemberEnd() || !schema->value.IsString() ||
            std::string {schema->value.GetString()} != "nitropie.profile-merge-request") {
            error = "request schema is not nitropie.profile-merge-request";
            return false;
        }
        const auto version {document.FindMember("version")};
        if (version == document.MemberEnd() || !version->value.IsInt() ||
            version->value.GetInt() != 1) {
            error = "request version is not 1";
            return false;
        }
        if (!stringOf(document, "profileId", job.profileId, error))
            return false;
        const auto systems {document.FindMember("systems")};
        if (systems == document.MemberEnd() || !systems->value.IsArray() ||
            systems->value.Empty()) {
            error = "request has no systems";
            return false;
        }
        for (const Value& item : systems->value.GetArray()) {
            if (!item.IsObject()) {
                error = "a system request is not an object";
                return false;
            }
            if (!isMember({"system", "profileId", "romRoot", "source", "shared", "sharedSha256",
                           "state", "stateSha256", "legacy", "legacySha256"},
                          item, unknown)) {
                error = "a system request has an unknown member: " + unknown;
                return false;
            }
            MigrationSplit::MergeRequest request;
            if (!stringOf(item, "system", request.system, error) ||
                !stringOf(item, "profileId", request.profileId, error) ||
                !stringOf(item, "romRoot", request.romRoot, error) ||
                !stringOf(item, "source", request.source, error) ||
                !stringOf(item, "shared", request.shared, error) ||
                !stringOf(item, "sharedSha256", request.sharedDigest, error) ||
                !stringOf(item, "state", request.state, error) ||
                !stringOf(item, "stateSha256", request.stateDigest, error) ||
                !stringOf(item, "legacy", request.legacy, error))
                return false;
            const auto legacyDigest {item.FindMember("legacySha256")};
            if (legacyDigest != item.MemberEnd()) {
                if (!legacyDigest->value.IsString()) {
                    error = "system request legacySha256 is not a string";
                    return false;
                }
                request.legacyDigest = legacyDigest->value.GetString();
            }
            if (request.system.empty() || request.profileId != job.profileId ||
                request.romRoot.empty() ||
                !MigrationSplit::validDigest(request.sharedDigest) ||
                !MigrationSplit::validDigest(request.stateDigest) ||
                !MigrationSplit::validRelative(request.shared) ||
                !MigrationSplit::validRelative(request.state) ||
                !MigrationSplit::validRelative(request.legacy)) {
                error = "system request has an invalid system, profile, digest or staging name";
                return false;
            }
            if (!request.legacyDigest.empty() &&
                !MigrationSplit::validDigest(request.legacyDigest)) {
                error = "system request has an invalid legacy digest";
                return false;
            }
            if (request.source != "legacy") {
                error = "system request source is not legacy";
                return false;
            }
            job.systems.push_back(std::move(request));
        }
        return true;
    }

    void emitReport(const std::string& status,
                    const std::string& refusal,
                    const std::vector<MigrationSplit::SystemResult>& systems)
    {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer {buffer};
        writer.StartObject();
        writer.Key("status");
        writer.String(status.c_str());
        if (!refusal.empty()) {
            writer.Key("refusal");
            writer.String(refusal.c_str());
        }
        writer.Key("systems");
        writer.StartArray();
        for (const MigrationSplit::SystemResult& system : systems) {
            writer.StartObject();
            writer.Key("system");
            writer.String(system.system.c_str());
            writer.Key("source");
            writer.String(system.source.c_str());
            writer.Key("sharedSha256");
            writer.String(system.sharedDigest.c_str());
            writer.Key("stateSha256");
            writer.String(system.stateDigest.c_str());
            writer.Key("records");
            writer.Int(system.records);
            writer.Key("personalEntries");
            writer.Int(system.personalEntries);
            writer.Key("items");
            writer.StartArray();
            for (const MigrationSplit::Item& item : system.items) {
                writer.StartObject();
                writer.Key("item");
                writer.String(item.item.c_str());
                writer.Key("path");
                writer.String(item.path.c_str());
                writer.Key("field");
                writer.String(item.field.c_str());
                writer.Key("reason");
                writer.String(item.reason.c_str());
                writer.Key("blocking");
                writer.Bool(item.blocking);
                writer.EndObject();
            }
            writer.EndArray();
            writer.EndObject();
        }
        writer.EndArray();
        writer.EndObject();
        std::printf("%s\n", buffer.GetString());
    }

    void emitMergeReport(const std::string& status,
                         const std::string& refusal,
                         const std::vector<MigrationSplit::SystemResult>& systems)
    {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer {buffer};
        writer.StartObject();
        writer.Key("status");
        writer.String(status.c_str());
        if (!refusal.empty()) {
            writer.Key("refusal");
            writer.String(refusal.c_str());
        }
        writer.Key("systems");
        writer.StartArray();
        for (const MigrationSplit::SystemResult& system : systems) {
            writer.StartObject();
            writer.Key("system");
            writer.String(system.system.c_str());
            writer.Key("source");
            writer.String(system.source.c_str());
            writer.Key("legacySha256");
            writer.String(system.legacyDigest.c_str());
            writer.Key("records");
            writer.Int(system.records);
            writer.Key("personalEntries");
            writer.Int(system.personalEntries);
            writer.Key("items");
            writer.StartArray();
            for (const MigrationSplit::Item& item : system.items) {
                writer.StartObject();
                writer.Key("item");
                writer.String(item.item.c_str());
                writer.Key("path");
                writer.String(item.path.c_str());
                writer.Key("field");
                writer.String(item.field.c_str());
                writer.Key("reason");
                writer.String(item.reason.c_str());
                writer.Key("blocking");
                writer.Bool(item.blocking);
                writer.EndObject();
            }
            writer.EndArray();
            writer.EndObject();
        }
        writer.EndArray();
        writer.EndObject();
        std::printf("%s\n", buffer.GetString());
    }

    struct Pending {
        MigrationSplit::SystemRequest request;
        std::string shared;
        std::string state;
        MigrationSplit::SystemResult result;
    };

    int runSplit(int rootFd, const Request& request)
    {
        // Split and verify every system before writing anything: one refusal
        // must leave no staged output at all.
        std::vector<Pending> pending;
        for (const MigrationSplit::SystemRequest& system : request.systems) {
            std::string gamelist;
            std::string error;
            if (!MigrationSplit::readStagingFile(rootFd, system.gamelist,
                                                 MigrationSplit::kMaxGamelistBytes, gamelist,
                                                 error)) {
                emitReport("refused", error, {});
                return 2;
            }
            if (!MigrationSplit::digestMatches(gamelist, system.digest)) {
                emitReport("refused", "gamelist bytes do not match the request digest", {});
                return 2;
            }
            Pending item;
            item.request = system;
            if (!MigrationSplit::splitSystem(system, gamelist, item.shared, item.state,
                                             item.result, error)) {
                emitReport("refused", system.system + ": " + error, {});
                return 2;
            }
            pending.push_back(std::move(item));
        }

        std::vector<MigrationSplit::SystemResult> results;
        for (Pending& item : pending) {
            std::string error;
            if (!MigrationSplit::writeNoClobber(rootFd, item.request.shared, item.shared, error) ||
                !MigrationSplit::writeNoClobber(rootFd, item.request.state, item.state, error)) {
                emitReport("refused", error, {});
                return 2;
            }
            std::string storedShared;
            std::string storedState;
            if (!MigrationSplit::readStagingFile(rootFd, item.request.shared,
                                                 MigrationSplit::kMaxGamelistBytes,
                                                 storedShared, error) ||
                !MigrationSplit::readStagingFile(rootFd, item.request.state,
                                                 MigrationSplit::kMaxGamelistBytes, storedState,
                                                 error) ||
                storedShared != item.shared || storedState != item.state) {
                emitReport("refused", "staged bytes changed after writing", {});
                return 2;
            }
            std::string gamelist;
            if (!MigrationSplit::readStagingFile(rootFd, item.request.gamelist,
                                                 MigrationSplit::kMaxGamelistBytes, gamelist,
                                                 error)) {
                emitReport("refused", error, {});
                return 2;
            }
            if (!MigrationSplit::verifySystem(item.request, gamelist, storedShared, storedState,
                                              item.result, error)) {
                emitReport("refused", item.request.system + ": " + error, {});
                return 2;
            }
            item.result.sharedDigest = MigrationSplit::hexDigest(item.shared);
            item.result.stateDigest = MigrationSplit::hexDigest(item.state);
            results.push_back(std::move(item.result));
        }

        bool blocked {false};
        for (const MigrationSplit::SystemResult& result : results) {
            for (const MigrationSplit::Item& item : result.items) {
                if (item.blocking)
                    blocked = true;
            }
        }
        emitReport(blocked ? "blocked" : "staged", "", results);
        return blocked ? 1 : 0;
    }

    int runVerify(int rootFd, const Request& request)
    {
        std::vector<MigrationSplit::SystemResult> results;
        for (const MigrationSplit::SystemRequest& system : request.systems) {
            std::string gamelist;
            std::string shared;
            std::string state;
            std::string error;
            if (!MigrationSplit::readStagingFile(rootFd, system.gamelist,
                                                 MigrationSplit::kMaxGamelistBytes, gamelist,
                                                 error) ||
                !MigrationSplit::readStagingFile(rootFd, system.shared,
                                                 MigrationSplit::kMaxGamelistBytes, shared,
                                                 error) ||
                !MigrationSplit::readStagingFile(rootFd, system.state,
                                                 MigrationSplit::kMaxGamelistBytes, state,
                                                 error)) {
                emitReport("refused", error, {});
                return 2;
            }
            if (!MigrationSplit::digestMatches(gamelist, system.digest)) {
                emitReport("refused", "gamelist bytes do not match the request digest", {});
                return 2;
            }
            MigrationSplit::SystemResult result;
            result.system = system.system;
            result.source = system.source;
            if (!MigrationSplit::verifySystem(system, gamelist, shared, state, result, error)) {
                emitReport("refused", system.system + ": " + error, {});
                return 2;
            }
            result.sharedDigest = MigrationSplit::hexDigest(shared);
            result.stateDigest = MigrationSplit::hexDigest(state);
            results.push_back(std::move(result));
        }
        emitReport("verified", "", results);
        return 0;
    }

    struct MergePending {
        MigrationSplit::MergeRequest request;
        std::string shared;
        std::string state;
        std::string legacy;
        MigrationSplit::SystemResult result;
    };

    int runMerge(int rootFd, const MergeJob& job)
    {
        std::vector<MergePending> pending;
        for (const MigrationSplit::MergeRequest& system : job.systems) {
            std::string error;
            MergePending item;
            item.request = system;
            if (!MigrationSplit::readStagingFile(rootFd, system.shared,
                                                 MigrationSplit::kMaxGamelistBytes,
                                                 item.shared, error)) {
                emitMergeReport("refused", error, {});
                return 2;
            }
            if (!MigrationSplit::digestMatches(item.shared, system.sharedDigest)) {
                emitMergeReport("refused", "shared XML does not match the request digest", {});
                return 2;
            }
            if (!MigrationSplit::readStagingFile(rootFd, system.state,
                                                 MigrationSplit::kMaxGamelistBytes,
                                                 item.state, error)) {
                emitMergeReport("refused", error, {});
                return 2;
            }
            if (!MigrationSplit::digestMatches(item.state, system.stateDigest)) {
                emitMergeReport("refused", "profile state does not match the request digest", {});
                return 2;
            }
            if (!MigrationSplit::mergeSystem(system, item.shared, item.state, item.legacy,
                                             item.result, error)) {
                emitMergeReport("refused", system.system + ": " + error, {});
                return 2;
            }
            pending.push_back(std::move(item));
        }

        std::vector<MigrationSplit::SystemResult> results;
        bool blocked {false};
        for (MergePending& item : pending) {
            item.result.legacyDigest = MigrationSplit::hexDigest(item.legacy);
            for (const MigrationSplit::Item& entry : item.result.items) {
                if (entry.blocking)
                    blocked = true;
            }
            results.push_back(item.result);
        }
        if (blocked) {
            emitMergeReport("blocked", "", results);
            return 1;
        }

        // Only a proven merge is written, and the stored bytes are verified
        // again so publication never carries an unproven document.
        results.clear();
        for (MergePending& item : pending) {
            std::string error;
            if (!MigrationSplit::writeNoClobber(rootFd, item.request.legacy, item.legacy, error)) {
                emitMergeReport("refused", error, {});
                return 2;
            }
            std::string stored;
            if (!MigrationSplit::readStagingFile(rootFd, item.request.legacy,
                                                 MigrationSplit::kMaxGamelistBytes, stored,
                                                 error) ||
                stored != item.legacy) {
                emitMergeReport("refused", "staged bytes changed after writing", {});
                return 2;
            }
            MigrationSplit::SystemResult checked;
            if (!MigrationSplit::verifyMergedSystem(item.request, item.shared, item.state, stored,
                                                    checked, error)) {
                emitMergeReport("refused", error, {});
                return 2;
            }
            checked.legacyDigest = MigrationSplit::hexDigest(stored);
            results.push_back(checked);
        }
        emitMergeReport("merged", "", results);
        return 0;
    }

    int runVerifyMerge(int rootFd, const MergeJob& job)
    {
        std::vector<MigrationSplit::SystemResult> results;
        for (const MigrationSplit::MergeRequest& system : job.systems) {
            std::string shared;
            std::string state;
            std::string legacy;
            std::string error;
            if (!MigrationSplit::readStagingFile(rootFd, system.shared,
                                                 MigrationSplit::kMaxGamelistBytes, shared,
                                                 error) ||
                !MigrationSplit::readStagingFile(rootFd, system.state,
                                                 MigrationSplit::kMaxGamelistBytes, state, error) ||
                !MigrationSplit::readStagingFile(rootFd, system.legacy,
                                                 MigrationSplit::kMaxGamelistBytes, legacy,
                                                 error)) {
                emitMergeReport("refused", error, {});
                return 2;
            }
            if (!MigrationSplit::digestMatches(shared, system.sharedDigest)) {
                emitMergeReport("refused", "shared XML does not match the request digest", {});
                return 2;
            }
            if (!MigrationSplit::digestMatches(state, system.stateDigest)) {
                emitMergeReport("refused", "profile state does not match the request digest", {});
                return 2;
            }
            if (system.legacyDigest.empty() ||
                !MigrationSplit::digestMatches(legacy, system.legacyDigest)) {
                emitMergeReport("refused", "legacy gamelist does not match the request digest", {});
                return 2;
            }
            MigrationSplit::SystemResult result;
            if (!MigrationSplit::verifyMergedSystem(system, shared, state, legacy, result,
                                                    error)) {
                emitMergeReport("refused", error, {});
                return 2;
            }
            // The frozen inputs must be byte-identical after the proof.
            std::string again;
            if (!MigrationSplit::readStagingFile(rootFd, system.shared,
                                                 MigrationSplit::kMaxGamelistBytes, again,
                                                 error) ||
                again != shared ||
                !MigrationSplit::readStagingFile(rootFd, system.state,
                                                 MigrationSplit::kMaxGamelistBytes, again,
                                                 error) ||
                again != state) {
                emitMergeReport("refused", "a frozen input changed during verification", {});
                return 2;
            }
            result.legacyDigest = MigrationSplit::hexDigest(legacy);
            results.push_back(result);
        }
        emitMergeReport("verified", "", results);
        return 0;
    }
} // namespace

int main(int argc, char** argv)
{
    const std::string command {argc > 1 ? argv[1] : ""};
    const bool splitCommand {command == "split" || command == "verify"};
    const bool mergeCommand {command == "merge" || command == "verify-merge"};
    if (argc != 6 || (!splitCommand && !mergeCommand) ||
        std::string {argv[2]} != "--staging-fd" || std::string {argv[4]} != "--request") {
        std::fprintf(stderr,
                     "usage: es-de-migration {split|verify|merge|verify-merge} "
                     "--staging-fd N --request REL\n");
        return 2;
    }
    char* end {nullptr};
    const long fdValue {std::strtol(argv[3], &end, 10)};
    if (end == argv[3] || *end != '\0' || fdValue < 0)
        return 2;
    const int rootFd {static_cast<int>(fdValue)};
    struct stat info {};
    if (::fstat(rootFd, &info) != 0 || !S_ISDIR(info.st_mode))
        return 2;

    std::string requestBytes;
    std::string error;
    if (!MigrationSplit::readStagingFile(rootFd, argv[5], MigrationSplit::kMaxRequestBytes,
                                         requestBytes, error)) {
        emitReport("refused", error, {});
        return 2;
    }
    Request request;
    if (splitCommand) {
        if (!parseRequest(requestBytes, request, error)) {
            emitReport("refused", error, {});
            return 2;
        }
        return command == "split" ? runSplit(rootFd, request) : runVerify(rootFd, request);
    }
    MergeJob job;
    if (!parseMergeRequest(requestBytes, job, error)) {
        emitMergeReport("refused", error, {});
        return 2;
    }
    return command == "merge" ? runMerge(rootFd, job) : runVerifyMerge(rootFd, job);
}
