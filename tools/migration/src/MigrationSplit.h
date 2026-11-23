//  SPDX-License-Identifier: MIT
//
//  bad-penguin 6B: existing-fork gamelist split and reconstruction.
//
//  This module owns the two serializers used by the offline migration split:
//  GamelistFileParser::writeSharedDocument for the shared XML and
//  ProfileState::serializeDocument for the per-profile JSON. It is a library
//  for the es-de-migration command and its tests; it never opens a source file
//  or writes anything itself.
//
//  The report produced from these structures is a staging artefact. It is not
//  the migration receipt, it carries no authority, and 6C must refuse a
//  blocked tree.
//

#ifndef ES_TOOLS_MIGRATION_MIGRATION_SPLIT_H
#define ES_TOOLS_MIGRATION_MIGRATION_SPLIT_H

#include <string>
#include <vector>

namespace MigrationSplit
{
    // One system to split. All destination and input names are relative to the
    // staging directory; `romRoot` is used for path maths only and is never
    // opened here.
    struct SystemRequest {
        std::string system;
        std::string profileId;
        std::string romRoot;
        // "appdata" or "legacy"; recorded, never used to pick a source.
        std::string source;
        std::string gamelist;
        std::string digest;
        std::string shared;
        std::string state;
    };

    // One diagnostic item. `blocking` items prevent 6C publication; the rest
    // are warnings (wrong-typed values are warnings, as the state format
    // already treats them).
    struct Item {
        std::string system;
        std::string item;
        std::string path;
        std::string field;
        std::string reason;
        bool blocking {false};
    };

    struct SystemResult {
        std::string system;
        std::string source;
        std::string sharedDigest;
        std::string stateDigest;
        int records {0};
        int personalEntries {0};
        std::vector<Item> items;
    };

    struct Outcome {
        bool refused {false};
        std::string refusal;
        std::vector<SystemResult> systems;

        bool hasBlocking() const;
    };

    // Split one system from verified legacy gamelist bytes. Produces the shared
    // XML and the initial profile's state JSON through the fork's own
    // serializers, then verifies the result by reconstruction. Returns false
    // only for a refusal (unusable or ambiguous document); a successful split
    // may still carry blocking items.
    bool splitSystem(const SystemRequest& request,
                     const std::string& gamelistBytes,
                     std::string& sharedXml,
                     std::string& stateJson,
                     SystemResult& result,
                     std::string& error);

    // Re-read staged bytes and prove that shared XML plus the state overlay
    // reconstruct the original legacy metadata on every record and field,
    // including unknown shared children and the unresolved entries. Returns
    // false with `error` naming the first mismatch.
    bool verifySystem(const SystemRequest& request,
                      const std::string& gamelistBytes,
                      const std::string& sharedXml,
                      const std::string& stateJson,
                      SystemResult& result,
                      std::string& error);
} // namespace MigrationSplit

#endif // ES_TOOLS_MIGRATION_MIGRATION_SPLIT_H
