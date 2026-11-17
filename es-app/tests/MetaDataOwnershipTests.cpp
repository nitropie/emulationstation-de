//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  MetaDataOwnershipTests.cpp
//
//  Test cases for the metadata ownership contract and for the unchanged legacy gamelist
//  serialisation. Every key has to declare an owner; a key without one is a test failure rather
//  than a silent default.
//

#include "TestHarness.h"

#include "MetaData.h"

#include <pugixml.hpp>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace
{
    std::set<std::string> personalKeys(const MetaDataListType type)
    {
        std::set<std::string> keys;
        for (const MetaDataDecl& decl : getMDDByType(type)) {
            if (isPersonalMetadata(decl))
                keys.insert(decl.key);
        }

        return keys;
    }

    std::set<std::string> allKeys(const MetaDataListType type)
    {
        std::set<std::string> keys;
        for (const MetaDataDecl& decl : getMDDByType(type))
            keys.insert(decl.key);

        return keys;
    }

} // namespace

TEST_CASE(everyGameKeyDeclaresItsOwner)
{
    const std::set<std::string> expectedPersonal {
        "altemulator",  "collectionsortname", "completed", "controller", "favorite", "hidden",
        "hidemetadata", "lastplayed",         "playcount", "playtime",   "screen",
    };

    const std::vector<MetaDataDecl>& decls {getMDDByType(GAME_METADATA)};
    EXPECT(decls.size() == 24);

    for (const MetaDataDecl& decl : decls) {
        if (decl.ownership == MD_OWNER_UNCLASSIFIED) {
            std::printf("unclassified game metadata key: %s\n", decl.key.c_str());
            EXPECT(false);
        }

        EXPECT(decl.ownership == MD_OWNER_SHARED || decl.ownership == MD_OWNER_PERSONAL);
    }

    // The personal set is pinned to section 6.1; a wrong owner also fails, not only a missing one.
    EXPECT(personalKeys(GAME_METADATA) == expectedPersonal);
    EXPECT(allKeys(GAME_METADATA).size() == 24);
}

TEST_CASE(everyFolderKeyDeclaresItsOwner)
{
    const std::set<std::string> expectedPersonal {
        "completed", "controller", "favorite", "hidden", "hidemetadata", "lastplayed",
    };

    const std::vector<MetaDataDecl>& decls {getMDDByType(FOLDER_METADATA)};
    EXPECT(decls.size() == 18);

    for (const MetaDataDecl& decl : decls) {
        if (decl.ownership == MD_OWNER_UNCLASSIFIED) {
            std::printf("unclassified folder metadata key: %s\n", decl.key.c_str());
            EXPECT(false);
        }

        EXPECT(decl.ownership == MD_OWNER_SHARED || decl.ownership == MD_OWNER_PERSONAL);
    }

    EXPECT(personalKeys(FOLDER_METADATA) == expectedPersonal);
    EXPECT(allKeys(FOLDER_METADATA).size() == 18);
}

TEST_CASE(libraryFieldsStayShared)
{
    // The two easy-to-misread cases from section 6.1: rating is the scraped rating and kidgame is
    // a library classification, so neither may become personal.
    const std::set<std::string> personal {personalKeys(GAME_METADATA)};

    EXPECT(personal.count("name") == 0);
    EXPECT(personal.count("desc") == 0);
    EXPECT(personal.count("rating") == 0);
    EXPECT(personal.count("releasedate") == 0);
    EXPECT(personal.count("developer") == 0);
    EXPECT(personal.count("publisher") == 0);
    EXPECT(personal.count("genre") == 0);
    EXPECT(personal.count("players") == 0);
    EXPECT(personal.count("kidgame") == 0);
    EXPECT(personal.count("broken") == 0);
    EXPECT(personal.count("nogamecount") == 0);
    EXPECT(personal.count("nomultiscrape") == 0);

    EXPECT(personal.count("favorite") == 1);
    EXPECT(personal.count("playcount") == 1);
    EXPECT(personal.count("lastplayed") == 1);
}

TEST_CASE(legacyGamelistWriterIsUnchanged)
{
    pugi::xml_document document;
    pugi::xml_node record {document.append_child("game")};

    MetaDataList metadata {GAME_METADATA};
    metadata.set("favorite", "true");
    metadata.set("playcount", "4");

    // Only the non-default values are written, exactly as before the ownership column was added.
    metadata.appendToXML(record, true, "");

    std::vector<std::string> written;
    for (pugi::xml_node child {record.first_child()}; child; child = child.next_sibling())
        written.emplace_back(child.name());

    const std::vector<std::string> expected {"favorite", "playcount"};
    EXPECT(written == expected);
    EXPECT(std::string {record.child("favorite").text().get()} == "true");
    EXPECT(std::string {record.child("playcount").text().get()} == "4");
    EXPECT(!record.child("ownership"));
    EXPECT(!record.child("owner"));

    // The legacy reader still returns the personal tags from the shared XML; without a profile
    // store there is no overlay and no fallback to invent.
    const MetaDataList read {MetaDataList::createFromXML(GAME_METADATA, record, "")};
    EXPECT(read.get("favorite") == "true");
    EXPECT(read.get("playcount") == "4");
    EXPECT(read.get("completed") == "false");
}

TEST_CASE(legacyGamelistWritesEveryDeclaredKeyOnce)
{
    pugi::xml_document document;
    pugi::xml_node record {document.append_child("game")};
    MetaDataList metadata {GAME_METADATA};
    metadata.appendToXML(record, false, "");

    const std::vector<MetaDataDecl>& decls {getMDDByType(GAME_METADATA)};
    for (const MetaDataDecl& decl : decls)
        EXPECT(record.child(decl.key.c_str()));

    int children {0};
    for (pugi::xml_node child {record.first_child()}; child; child = child.next_sibling())
        ++children;

    EXPECT(children == static_cast<int>(decls.size()));
    EXPECT(!record.child("ownership"));
    EXPECT(!record.child("owner"));
}
