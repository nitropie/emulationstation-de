//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  MediaLookupTests.cpp
//
//  Test cases for the media file lookup: the subfolders below the system root are derived with a
//  directory boundary, so a repeated occurrence of the root in the path and a start path that is
//  only a string prefix behave correctly.
//

#include "TestHarness.h"

#include "FileData.h"
#include "Settings.h"
#include "SystemData.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    // One fixture directory per test process, recreated before the first test needs it.
    const fs::path& fixtureRoot()
    {
        static const fs::path root {[] {
            const fs::path path {fs::temp_directory_path() / "es-de-media-lookup"};
            fs::remove_all(path);
            fs::create_directories(path);
            return path;
        }()};

        return root;
    }

    void writeFile(const fs::path& path)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file {path, std::ios::binary | std::ios::trunc};
        file << "";
    }

    SystemEnvironmentData* environment(const fs::path& roms)
    {
        SystemEnvironmentData* env {new SystemEnvironmentData};
        env->mStartPath = roms.string();
        env->mSearchExtensions = {".sfc"};
        env->mLaunchCommands = {{"%EMULATOR% %ROM%", "Snes9x - Current"}};

        return env;
    }

    // The settings every case relies on, set before the SystemData is constructed.
    void setTestSettings(const fs::path& mediaDirectory)
    {
        Settings::getInstance()->setString("MediaDirectory", mediaDirectory.string());
        Settings::getInstance()->setString("SaveGamelistsMode", "never");
        Settings::getInstance()->setBool("ParseGamelistOnly", true);
        Settings::getInstance()->setBool("IgnoreGamelist", true);
    }
} // namespace

TEST_CASE(mediaLookupUsesTheWholeDirectoryBelowTheSystemRoot)
{
    const fs::path root {fixtureRoot() / "deep"};
    fs::remove_all(root);
    const fs::path roms {root / "roms" / "snes"};
    const std::string startPath {roms.string()};

    // The system directory appears a second time below the root, so the start path itself occurs
    // twice in the parent directory. Replacing every occurrence would leave "/sub"; removing only
    // the leading one leaves "/sub" plus the repeated start path.
    const std::string romPath {startPath + "/sub" + startPath + "/Game.sfc"};
    writeFile(romPath);

    const fs::path media {root / "media"};
    const std::string mediaRoot {media.string()};
    // The correct media location keeps the repeated system directory.
    writeFile(mediaRoot + "/snes/covers/sub" + startPath + "/Game.png");
    writeFile(mediaRoot + "/snes/videos/sub" + startPath + "/Game.mp4");
    writeFile(mediaRoot + "/snes/manuals/sub" + startPath + "/Game.pdf");
    // ...while replacing every occurrence of the start path would have produced this one.
    writeFile(mediaRoot + "/snes/covers/sub/Game.png");

    setTestSettings(media);

    SystemData system {"snes", "Snes", "Snes", environment(roms), "snes", false, false};
    FileData game {GAME, romPath, system.getSystemEnvData(), &system};

    EXPECT(game.getMediafilePath("covers") ==
           (mediaRoot + "/snes/covers/sub" + startPath + "/Game.png"));
    EXPECT(game.getVideoPath() ==
           (mediaRoot + "/snes/videos/sub" + startPath + "/Game.mp4"));
    EXPECT(game.getManualPath() ==
           (mediaRoot + "/snes/manuals/sub" + startPath + "/Game.pdf"));
}

TEST_CASE(mediaLookupForAGameDirectlyInTheSystemRoot)
{
    const fs::path root {fixtureRoot() / "direct"};
    fs::remove_all(root);
    const fs::path roms {root / "roms" / "snes"};
    const fs::path romPath {roms / "Direct.sfc"};
    writeFile(romPath);

    const fs::path media {root / "media"};
    writeFile(media / "snes" / "covers" / "Direct.png");

    setTestSettings(media);

    SystemData system {"snes", "Snes", "Snes", environment(roms), "snes", false, false};
    FileData game {GAME, romPath.string(), system.getSystemEnvData(), &system};

    // getParent() is the start path itself, so no subfolders are added.
    EXPECT(game.getMediafilePath("covers") ==
           (media / "snes" / "covers" / "Direct.png").string());
}

TEST_CASE(mediaLookupOfADirectoryAsGameKeepsTheWholeName)
{
    const fs::path root {fixtureRoot() / "dirasgame"};
    fs::remove_all(root);
    const fs::path roms {root / "roms" / "snes"};
    const fs::path romPath {roms / "Game.m3u"};
    // A directory whose name has a configured extension, holding a file of the same name.
    writeFile(romPath / "Game.m3u");

    const fs::path media {root / "media"};
    writeFile(media / "snes" / "covers" / "Game.m3u.png");

    setTestSettings(media);

    SystemData system {"snes", "Snes", "Snes", environment(roms), "snes", false, false};
    FileData game {GAME, romPath.string(), system.getSystemEnvData(), &system};

    // The media files on disk keep the whole directory name, while the metadata name is the stem.
    EXPECT(game.getDisplayName() == "Game.m3u");
    EXPECT(game.getName() == "Game");
    EXPECT(game.getMediafilePath("covers") ==
           (media / "snes" / "covers" / "Game.m3u.png").string());
}

TEST_CASE(mediaLookupIgnoresAStartPathThatIsOnlyAStringPrefix)
{
    const fs::path root {fixtureRoot() / "prefix"};
    fs::remove_all(root);
    const fs::path roms {root / "roms" / "snes"};
    const fs::path romPath {root / "roms" / "snes-other" / "Game.sfc"};
    writeFile(romPath);

    const fs::path media {root / "media"};
    // The directory of a sibling of the start path is not below the start path.
    writeFile(media / "snes" / "covers" / "Game.png");
    // A string replacement would have produced this one instead.
    writeFile(media / "snes" / "covers-other" / "Game.png");

    setTestSettings(media);

    SystemData system {"snes", "Snes", "Snes", environment(roms), "snes", false, false};
    FileData game {GAME, romPath.string(), system.getSystemEnvData(), &system};

    EXPECT(game.getMediafilePath("covers") ==
           (media / "snes" / "covers" / "Game.png").string());
}
