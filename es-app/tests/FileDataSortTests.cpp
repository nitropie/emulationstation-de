//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  FileDataSortTests.cpp
//
//  Characterization tests for the folder sorting. The exact child order, the game and favourite
//  counts, the folder flags and the recursion into subfolders are pinned for every combination of
//  FoldersOnTop, favourites-on-top, the sort type and ShowHiddenGames, so unifying the two sort
//  implementations cannot change any of them.
//

#include "TestHarness.h"

#include "FileData.h"
#include "FileSorts.h"
#include "Settings.h"
#include "SystemData.h"

#include <cstdio>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // The fixture tree. The creation order is deliberately neither the name order nor the rating
    // order, so the secondary name sort is visible: "Docs" is created after "Tools", and
    // "Hidden Game" before "Alpha Game" with the same rating. "Docs" is an empty folder that both
    // modes hide in different ways.
    //
    //   Beta Game    game   rating 5, favourite            (created first)
    //   Tools        folder
    //     Sub        folder
    //       Deep Game game  rating 1, favourite
    //     Tool A     game   rating 3, no game count
    //   Docs         folder (empty)
    //   Hidden Game  game   rating 4, hidden
    //   Hacks        folder, favourite
    //     Kaizo      game   rating 1
    //     Hidden Hack game  rating 2, hidden
    //   Alpha Game   game   rating 4
    //   Utility      game   rating 3, no game count, favourite
    struct Fixture {
        SystemEnvironmentData* env;
        SystemData* system;
        FileData* root;
        FileData* hacks;
        FileData* tools;
        FileData* sub;

        Fixture()
        {
            Settings::getInstance()->setString("SaveGamelistsMode", "never");
            Settings::getInstance()->setBool("ParseGamelistOnly", true);
            Settings::getInstance()->setBool("IgnoreGamelist", true);
            Settings::getInstance()->setBool("ForceKid", false);

            env = new SystemEnvironmentData;
            env->mStartPath = "/roms/sort";
            env->mSearchExtensions = {".sfc"};
            env->mLaunchCommands = {{"%EMULATOR% %ROM%", "Emulator"}};

            system = new SystemData("sort", "Sort", "Sort", env, "snes", false, false);
            root = system->getRootFolder();

            add(root, "Beta Game", GAME, "true", "false", "false", "5");
            tools = add(root, "Tools", FOLDER, "false", "false", "false", "0");
            sub = add(tools, "Sub", FOLDER, "false", "false", "false", "0");
            add(sub, "Deep Game", GAME, "true", "false", "false", "1");
            add(tools, "Tool A", GAME, "false", "false", "true", "3");
            add(root, "Docs", FOLDER, "false", "false", "false", "0");
            add(root, "Hidden Game", GAME, "false", "true", "false", "4");
            hacks = add(root, "Hacks", FOLDER, "true", "false", "false", "0");
            add(hacks, "Kaizo", GAME, "false", "false", "false", "1");
            add(hacks, "Hidden Hack", GAME, "false", "true", "false", "2");
            add(root, "Alpha Game", GAME, "false", "false", "false", "4");
            add(root, "Utility", GAME, "true", "false", "true", "3");
        }

        ~Fixture() { delete system; }

        FileData* add(FileData* parent,
                      const char* name,
                      const FileType type,
                      const char* favorite,
                      const char* hidden,
                      const char* noGameCount,
                      const char* rating)
        {
            FileData* node {
                new FileData(type, root->getPath() + "/" + name, env, system)};
            node->metadata.set("name", name);
            node->metadata.set("favorite", favorite);
            node->metadata.set("hidden", hidden);
            node->metadata.set("nogamecount", noGameCount);
            node->metadata.set("rating", rating);
            parent->addChild(node);

            return node;
        }
    };

    std::vector<std::string> names(const FileData& node)
    {
        std::vector<std::string> result;

        for (const FileData* child : node.getChildren())
            result.emplace_back(child->metadata.get("name"));

        return result;
    }

    void printNames(const std::vector<std::string>& values)
    {
        std::printf("{");
        for (std::size_t i {0}; i < values.size(); ++i)
            std::printf("\"%s\"%s", values[i].c_str(), i + 1 < values.size() ? ", " : "");
        std::printf("}");
    }

    const FileData::SortType& sortType(FileData& root, const std::string& description)
    {
        return root.getSortTypeFromString(description);
    }

    // The expected state after sorting the fixture tree. Captured from the unmodified
    // implementation, case by case, before the two sort bodies were unified.
    struct Expected {
        std::vector<std::string> root;
        std::vector<std::string> hacks;
        std::vector<std::string> tools;
        std::vector<std::string> sub;
        unsigned int games;
        unsigned int favorites;
        bool onlyFolders;
        bool hasFolders;
    };

    Expected expected(std::vector<std::string> root,
                      std::vector<std::string> hacks,
                      std::vector<std::string> tools,
                      std::vector<std::string> sub,
                      const unsigned int games,
                      const unsigned int favorites,
                      const bool onlyFolders,
                      const bool hasFolders)
    {
        return {std::move(root),  std::move(hacks),     std::move(tools),
                std::move(sub),   games,               favorites,
                onlyFolders,      hasFolders};
    }

    struct SortCase {
        const char* description;
        bool favoritesOnTop;
        bool foldersOnTop;
        bool showHiddenGames;
        Expected expected;
    };

    const std::vector<SortCase> sortCases {
        {"name, ascending", false, false, false,
         expected({"Alpha Game", "Beta Game", "Hacks", "Tools", "Utility"}, {"Kaizo"},
                  {"Sub", "Tool A"}, {"Deep Game"}, 4, 2, false, true)},
        {"name, ascending", false, false, true,
         expected({"Alpha Game", "Beta Game", "Docs", "Hacks", "Hidden Game", "Tools", "Utility"},
                  {"Hidden Hack", "Kaizo"}, {"Sub", "Tool A"}, {"Deep Game"}, 6, 2, false, true)},
        {"name, ascending", false, true, false,
         expected({"Hacks", "Tools", "Alpha Game", "Beta Game", "Utility"}, {"Kaizo"},
                  {"Sub", "Tool A"}, {"Deep Game"}, 4, 2, false, true)},
        {"name, ascending", false, true, true,
         expected({"Docs", "Hacks", "Tools", "Alpha Game", "Beta Game", "Hidden Game", "Utility"},
                  {"Hidden Hack", "Kaizo"}, {"Sub", "Tool A"}, {"Deep Game"}, 6, 2, false, true)},
        {"name, ascending", true, false, false,
         expected({"Beta Game", "Hacks", "Utility", "Alpha Game", "Tools"}, {"Kaizo"},
                  {"Sub", "Tool A"}, {"Deep Game"}, 4, 2, false, true)},
        {"name, ascending", true, false, true,
         expected({"Beta Game", "Hacks", "Utility", "Alpha Game", "Hidden Game", "Tools"},
                  {"Hidden Hack", "Kaizo"}, {"Sub", "Tool A"}, {"Deep Game"}, 6, 2, false, true)},
        {"name, ascending", true, true, false,
         expected({"Hacks", "Tools", "Beta Game", "Utility", "Alpha Game"}, {"Kaizo"},
                  {"Sub", "Tool A"}, {"Deep Game"}, 4, 2, false, true)},
        {"name, ascending", true, true, true,
         expected({"Hacks", "Tools", "Beta Game", "Utility", "Alpha Game", "Hidden Game"},
                  {"Hidden Hack", "Kaizo"}, {"Sub", "Tool A"}, {"Deep Game"}, 6, 2, false, true)},
        {"rating, descending", false, false, false,
         expected({"Beta Game", "Alpha Game", "Utility", "Hacks", "Tools"}, {"Kaizo"},
                  {"Tool A", "Sub"}, {"Deep Game"}, 4, 2, false, true)},
        {"rating, descending", false, false, true,
         expected({"Beta Game", "Alpha Game", "Hidden Game", "Utility", "Docs", "Hacks", "Tools"},
                  {"Hidden Hack", "Kaizo"}, {"Tool A", "Sub"}, {"Deep Game"}, 6, 2, false, true)},
        {"rating, descending", false, true, false,
         expected({"Hacks", "Tools", "Beta Game", "Alpha Game", "Utility"}, {"Kaizo"},
                  {"Sub", "Tool A"}, {"Deep Game"}, 4, 2, false, true)},
        {"rating, descending", false, true, true,
         expected({"Docs", "Hacks", "Tools", "Beta Game", "Alpha Game", "Hidden Game", "Utility"},
                  {"Hidden Hack", "Kaizo"}, {"Sub", "Tool A"}, {"Deep Game"}, 6, 2, false, true)},
        {"rating, descending", true, false, false,
         expected({"Beta Game", "Utility", "Hacks", "Alpha Game", "Tools"}, {"Kaizo"},
                  {"Tool A", "Sub"}, {"Deep Game"}, 4, 2, false, true)},
        {"rating, descending", true, false, true,
         expected({"Beta Game", "Utility", "Hacks", "Alpha Game", "Hidden Game", "Tools"},
                  {"Hidden Hack", "Kaizo"}, {"Tool A", "Sub"}, {"Deep Game"}, 6, 2, false, true)},
        {"rating, descending", true, true, false,
         expected({"Hacks", "Tools", "Beta Game", "Utility", "Alpha Game"}, {"Kaizo"},
                  {"Sub", "Tool A"}, {"Deep Game"}, 4, 2, false, true)},
        {"rating, descending", true, true, true,
         expected({"Hacks", "Tools", "Beta Game", "Utility", "Alpha Game", "Hidden Game"},
                  {"Hidden Hack", "Kaizo"}, {"Sub", "Tool A"}, {"Deep Game"}, 6, 2, false, true)},
    };
} // namespace

TEST_CASE(sortOrderIsPinnedForEveryModeAndComparator)
{
    const std::vector<std::string> descriptions {"name, ascending", "rating, descending"};
    const std::vector<bool> favoritesModes {false, true};
    const std::vector<bool> foldersModes {false, true};
    const std::vector<bool> hiddenModes {false, true};

    for (const std::string& description : descriptions) {
        for (const bool favoritesOnTop : favoritesModes) {
            for (const bool foldersOnTop : foldersModes) {
                for (const bool showHiddenGames : hiddenModes) {
                    const auto match = std::find_if(
                        sortCases.cbegin(), sortCases.cend(), [&](const SortCase& candidate) {
                            return description == candidate.description &&
                                   favoritesOnTop == candidate.favoritesOnTop &&
                                   foldersOnTop == candidate.foldersOnTop &&
                                   showHiddenGames == candidate.showHiddenGames;
                        });
                    EXPECT(match != sortCases.cend());
                    if (match == sortCases.cend())
                        continue;

                    Fixture fixture;
                    Settings::getInstance()->setBool("FoldersOnTop", foldersOnTop);
                    Settings::getInstance()->setBool("ShowHiddenGames", showHiddenGames);

                    fixture.root->sort(sortType(*fixture.root, description), favoritesOnTop);

                    const Expected& hit {match->expected};
                    const auto report = [&](const char* subject,
                                            const std::vector<std::string>& actual,
                                            const std::vector<std::string>& wanted) {
                        if (actual == wanted)
                            return;

                        std::printf("    %s mismatch for %s fav=%d tot=%d hidden=%d:",
                                    subject, description.c_str(), favoritesOnTop, foldersOnTop,
                                    showHiddenGames);
                        std::printf(" actual=");
                        printNames(actual);
                        std::printf(" expected=");
                        printNames(wanted);
                        std::printf("\n");
                    };

                    const std::vector<std::string> rootNames {names(*fixture.root)};
                    const std::vector<std::string> hacksNames {names(*fixture.hacks)};
                    const std::vector<std::string> toolsNames {names(*fixture.tools)};
                    const std::vector<std::string> subNames {names(*fixture.sub)};

                    report("root", rootNames, hit.root);
                    report("hacks", hacksNames, hit.hacks);
                    report("tools", toolsNames, hit.tools);
                    report("sub", subNames, hit.sub);

                    EXPECT(rootNames == hit.root);
                    EXPECT(hacksNames == hit.hacks);
                    EXPECT(toolsNames == hit.tools);
                    EXPECT(subNames == hit.sub);
                    EXPECT(fixture.root->getGameCount().first == hit.games);
                    EXPECT(fixture.root->getGameCount().second == hit.favorites);
                    EXPECT(fixture.root->getOnlyFoldersFlag() == hit.onlyFolders);
                    EXPECT(fixture.root->getHasFoldersFlag() == hit.hasFolders);
                }
            }
        }
    }
}

// A hidden folder with visible games, and no other visible folder. The favourites mode must skip it
// exactly like the other mode does: neither the folder nor its games appear in the result.
TEST_CASE(favouritesModeSkipsHiddenFoldersLikeTheOtherMode)
{
    struct Run {
        std::pair<unsigned int, unsigned int> count;
        std::vector<std::string> order;
    };

    // Builds the same tree every time: one hidden folder with two visible games.
    const auto run = [](const bool favoritesOnTop, const bool foldersOnTop) {
        Settings::getInstance()->setString("SaveGamelistsMode", "never");
        Settings::getInstance()->setBool("ParseGamelistOnly", true);
        Settings::getInstance()->setBool("IgnoreGamelist", true);
        Settings::getInstance()->setBool("ForceKid", false);
        Settings::getInstance()->setBool("ShowHiddenGames", false);
        Settings::getInstance()->setBool("FoldersOnTop", foldersOnTop);

        SystemEnvironmentData* env {new SystemEnvironmentData};
        env->mStartPath = "/roms/hidden";
        env->mSearchExtensions = {".sfc"};
        env->mLaunchCommands = {{"%EMULATOR% %ROM%", "Emulator"}};
        SystemData* system {
            new SystemData("hidden", "Hidden", "Hidden", env, "snes", false, false)};
        FileData* root {system->getRootFolder()};

        FileData* hiddenFolder {
            new FileData(FOLDER, root->getPath() + "/Hidden Folder", env, system)};
        hiddenFolder->metadata.set("name", "Hidden Folder");
        hiddenFolder->metadata.set("hidden", "true");
        hiddenFolder->metadata.set("favorite", "false");
        root->addChild(hiddenFolder);

        for (const char* name : {"Inner Game B.sfc", "Inner Game A.sfc"}) {
            FileData* game {new FileData(GAME, hiddenFolder->getPath() + "/" + name, env, system)};
            game->metadata.set("name", std::string {"Inner "} + name);
            game->metadata.set("hidden", "false");
            game->metadata.set("favorite", "false");
            game->metadata.set("rating", "1");
            hiddenFolder->addChild(game);
        }

        root->sort(root->getSortTypeFromString("name, ascending"), favoritesOnTop);

        const Run result {root->getGameCount(), names(*root)};
        delete system;
        return result;
    };

    for (const bool foldersOnTop : {false, true}) {
        const Run favourites {run(true, foldersOnTop)};
        const Run other {run(false, foldersOnTop)};

        if (favourites.count.first != 0 || favourites.count.second != 0 ||
            !favourites.order.empty() || favourites.count != other.count ||
            favourites.order != other.order) {
            std::printf("    hidden folder case tot=%d: favourites=%u/%u other=%u/%u "
                        "favourites children=%zu\n",
                        foldersOnTop, favourites.count.first, favourites.count.second,
                        other.count.first, other.count.second, favourites.order.size());
        }

        // Literal expectations: the hidden folder and its games are not part of the result.
        EXPECT(favourites.count.first == 0);
        EXPECT(favourites.count.second == 0);
        EXPECT(favourites.order.empty());

        // The favourites mode agrees with the other mode for the same tree.
        EXPECT(favourites.count == other.count);
        EXPECT(favourites.order == other.order);
    }
}

TEST_CASE(sortCountsOnlyKidGamesInKidMode)
{
    Settings::getInstance()->setBool("FoldersOnTop", false);
    Settings::getInstance()->setBool("ShowHiddenGames", false);

    Fixture fixture;
    Settings::getInstance()->setBool("ForceKid", true);

    for (FileData* child : fixture.root->getChildren()) {
        if (child->metadata.get("name") == "Beta Game")
            child->metadata.set("kidgame", "true");
    }

    fixture.root->sort(sortType(*fixture.root, "name, ascending"), false);

    // Beta Game is the only kid game that is not hidden and counts as a game.
    EXPECT(fixture.root->getGameCount().first == 1);
    EXPECT(fixture.root->getGameCount().second == 1);
}

TEST_CASE(sortFlagsDescribeTheKeptChildren)
{
    // A folders-only tree.
    {
        Settings::getInstance()->setBool("FoldersOnTop", true);
        Settings::getInstance()->setBool("ShowHiddenGames", false);

        Fixture fixture;
        for (FileData* child : std::vector<FileData*>(fixture.root->getChildren())) {
            if (child->getType() != FOLDER)
                fixture.root->removeChild(child);
        }

        fixture.root->sort(sortType(*fixture.root, "name, ascending"), false);
        EXPECT(fixture.root->getOnlyFoldersFlag());
        EXPECT(fixture.root->getHasFoldersFlag());
    }

    // A games-only tree.
    {
        Fixture fixture;
        for (FileData* child : std::vector<FileData*>(fixture.root->getChildren()))
            fixture.root->removeChild(child);

        fixture.add(fixture.root, "Only Game", GAME, "false", "false", "false", "1");
        fixture.root->sort(sortType(*fixture.root, "name, ascending"), true);
        EXPECT(!fixture.root->getOnlyFoldersFlag());
        EXPECT(!fixture.root->getHasFoldersFlag());
        EXPECT(fixture.root->getGameCount().first == 1);
    }

    // An empty root.
    {
        Fixture fixture;
        for (FileData* child : std::vector<FileData*>(fixture.root->getChildren()))
            fixture.root->removeChild(child);

        fixture.root->sort(sortType(*fixture.root, "name, ascending"), false);
        EXPECT(fixture.root->getOnlyFoldersFlag());
        EXPECT(!fixture.root->getHasFoldersFlag());
        EXPECT(fixture.root->getGameCount().first == 0);
    }
}
