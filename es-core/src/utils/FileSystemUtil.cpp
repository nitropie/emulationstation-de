//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  FileSystemUtil.cpp
//
//  Low-level filesystem functions.
//  Resolve relative paths, resolve symlinks, create directories,
//  remove files etc.
//

#if defined(__ANDROID__)
#include "utils/PlatformUtilAndroid.h"
#include <SDL2/SDL_system.h>
#endif

#if defined(__APPLE__)
#define _DARWIN_USE_64_BIT_INODE
#endif

#include "utils/FileSystemUtil.h"

#include "Log.h"
#include "utils/PlatformUtil.h"
#include "utils/StringUtil.h"

#include <fstream>
#include <regex>
#include <string>
#include <sys/stat.h>

#if defined(_WIN64)
#include <Windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

// For Unix systems, set the install prefix as defined via CMAKE_INSTALL_PREFIX when CMake was run.
// If not defined, the default prefix "/usr" will be used on Linux and "/usr/local" on FreeBSD.
// This fallback should not be required though unless the build environment is broken.
#if defined(__unix__)
#if defined(ES_INSTALL_PREFIX)
const std::string installPrefix {ES_INSTALL_PREFIX};
#else
#if defined(__linux__)
const std::string installPrefix {"/usr"};
#else
const std::string installPrefix {"/usr/local"};
#endif
#endif
#endif

namespace Utils
{
    namespace FileSystem
    {
        static std::string homePath;
        static std::string exePath;
        static std::string esBinary;

        StringList getDirContent(const std::string& path, const bool recursive)
        {
            const std::string& genericPath {getGenericPath(path)};
            StringList contentList;

            if (!isDirectory(genericPath))
                return contentList;

            try {
                if (recursive) {
#if defined(_WIN64)
                    for (auto& entry : std::filesystem::recursive_directory_iterator(
                             Utils::String::stringToWideString(genericPath))) {
                        contentList.emplace_back(
                            Utils::String::wideStringToString(entry.path().generic_wstring()));
#else
                    for (auto& entry : std::filesystem::recursive_directory_iterator(genericPath)) {
                        contentList.emplace_back(entry.path().generic_string());
#endif
                    }
                }
                else {
#if defined(_WIN64)
                    for (auto& entry : std::filesystem::directory_iterator(
                             Utils::String::stringToWideString(genericPath))) {
                        contentList.emplace_back(
                            Utils::String::wideStringToString(entry.path().generic_wstring()));
#else
                    for (auto& entry : std::filesystem::directory_iterator(genericPath)) {
                        contentList.emplace_back(entry.path().generic_string());
#endif
                    }
                }
            }
            catch (...) {
#if defined(_WIN64)
                LOG(LogError) << "FileSystemUtil::getDirContent(): Couldn't read directory " << "\""
                              << Utils::String::replace(path, "/", "\\")
                              << "\", permission problems?";
#else
                LOG(LogError) << "FileSystemUtil::getDirContent(): Couldn't read directory " << "\""
                              << path << "\", permission problems?";
#endif
                return contentList;
            }

            contentList.sort();
            return contentList;
        }

        StringList getMatchingFiles(const std::string& pattern)
        {
            StringList files;
            size_t entry {pattern.find('*')};

            if (entry == std::string::npos)
                return files;

            const std::string& parent {getParent(pattern)};

            // Don't allow wildcard matching for the parent directory.
            if (entry <= parent.size())
                return files;

            const StringList& dirContent {getDirContent(parent)};

            if (dirContent.size() == 0)
                return files;

            // Some characters need to be escaped in order for the regular expression to work.
            std::string escPattern {Utils::String::replace(pattern, "*", ".*")};
            escPattern = Utils::String::replace(escPattern, ")", "\\)");
            escPattern = Utils::String::replace(escPattern, "(", "\\(");
            escPattern = Utils::String::replace(escPattern, "]", "\\]");
            escPattern = Utils::String::replace(escPattern, "[", "\\[");

            std::regex expression;

            try {
                expression = escPattern;
            }
            catch (...) {
                LOG(LogError) << "FileSystemUtil::getMatchingFiles(): Invalid regular expression "
                              << "\"" << pattern << "\"";
                return files;
            }

            for (auto& entry : dirContent) {
                if (std::regex_match(entry, expression))
                    files.emplace_back(entry);
            }

            return files;
        }

        StringList getPathList(const std::string& path)
        {
            StringList pathList;
            const std::string& genericPath {getGenericPath(path)};
            size_t start {0};
            size_t end {0};

            // Split at '/'
            while ((end = genericPath.find("/", start)) != std::string::npos) {
                if (end != start)
                    pathList.emplace_back(std::string {genericPath, start, end - start});
                start = end + 1;
            }
            // Add last folder / file to pathList.
            if (start != genericPath.size())
                pathList.emplace_back(std::string {genericPath, start, genericPath.size() - start});

            return pathList;
        }

        void setHomePath(const std::string& path)
        {
            // Set home path.
            homePath = getGenericPath(path);
        }

        std::string getHomePath()
        {
            // Only construct the homepath once.
            if (homePath.length())
                return homePath;

#if defined(__ANDROID__)
            homePath = FileSystemVariables::sAppDataDirectory;
            return homePath;
#endif

#if defined(_WIN64)
            // On Windows we need to check HOMEDRIVE and HOMEPATH.
            if (!homePath.length()) {
                std::wstring envHomeDrive;
                std::wstring envHomePath;
#if defined(_MSC_VER) // MSVC compiler.
                wchar_t* buffer;
                if (!_wdupenv_s(&buffer, nullptr, L"HOMEDRIVE"))
                    envHomeDrive = buffer;
                if (!_wdupenv_s(&buffer, nullptr, L"HOMEPATH"))
                    envHomePath = buffer;
#else
                envHomeDrive = _wgetenv(L"HOMEDRIVE");
                envHomePath = _wgetenv(L"HOMEPATH");
#endif
                if (envHomeDrive.length() && envHomePath.length())
                    homePath = getGenericPath(Utils::String::wideStringToString(envHomeDrive) +
                                              "/" + Utils::String::wideStringToString(envHomePath));
            }
#else

            if (!homePath.length()) {
                std::string envHome;
                if (getenv("HOME") != nullptr)
                    envHome = getenv("HOME");
                if (envHome.length())
                    homePath = getGenericPath(envHome);
            }
#endif

            // No homepath found, fall back to current working directory.
            if (!homePath.length())
                homePath = getCWDPath();

            return homePath;
        }

        std::string getSystemHomeDirectory()
        {
#if defined(_WIN64)
            // On Windows we need to check HOMEDRIVE and HOMEPATH.
            std::wstring envHomeDrive;
            std::wstring envHomePath;
#if defined(_MSC_VER) // MSVC compiler.
            wchar_t* buffer;
            if (!_wdupenv_s(&buffer, nullptr, L"HOMEDRIVE"))
                envHomeDrive = buffer;
            if (!_wdupenv_s(&buffer, nullptr, L"HOMEPATH"))
                envHomePath = buffer;
#else
            envHomeDrive = _wgetenv(L"HOMEDRIVE");
            envHomePath = _wgetenv(L"HOMEPATH");
#endif
            if (envHomeDrive.length() && envHomePath.length())
                return getGenericPath(Utils::String::wideStringToString(envHomeDrive) + "/" +
                                      Utils::String::wideStringToString(envHomePath));
#else
            std::string envHome;
            if (getenv("HOME") != nullptr)
                envHome = getenv("HOME");
            return envHome;
#endif
            return "";
        }

        std::string getAppDataDirectory()
        {
#if defined(__ANDROID__)
            return getHomePath();
#elif defined(__IOS__)
            return getHomePath() + "/Documents/ES-DE";
#endif

            if (FileSystemVariables::sAppDataDirectory.empty()) {
#if !defined(_WIN64)
                if (getenv("ESDE_APPDATA_DIR") != nullptr) {
                    const std::string envAppDataDir {getenv("ESDE_APPDATA_DIR")};
                    FileSystemVariables::sAppDataDirectory = expandHomePath(envAppDataDir);
                }
                else if (Utils::FileSystem::exists(getHomePath() + "/ES-DE")) {
#else
                if (Utils::FileSystem::exists(getHomePath() + "/ES-DE")) {
#endif
                    FileSystemVariables::sAppDataDirectory = getHomePath() + "/ES-DE";
                }
                else {
                    FileSystemVariables::sAppDataDirectory = getHomePath() + "/ES-DE";
                }
            }

            return FileSystemVariables::sAppDataDirectory;
        }

        std::string getInternalAppDataDirectory()
        {
#if defined(__ANDROID__)
            return AndroidVariables::sExternalDataDirectory;
#else
            return "";
#endif
        }

        std::string getCWDPath()
        {
            // Return current working directory.

#if defined(_WIN64)
            wchar_t tempWide[512];
            return (_wgetcwd(tempWide, 512) ?
                        getGenericPath(Utils::String::wideStringToString(tempWide)) :
                        "");
#else
            char temp[512];
            return (getcwd(temp, 512) ? getGenericPath(temp) : "");
#endif
        }

        std::string getPathToBinary(const std::string& executable)
        {
#if defined(_WIN64)
            return "";
#else
#if defined(FLATPAK_BUILD)
            // Ugly hack to compensate for the Flatpak sandbox restrictions. We traverse
            // this hardcoded list of paths and use the "which" command to check outside the
            // sandbox if the emulator binary exists.
            const std::string& pathVariable {
                "/var/lib/flatpak/exports/bin:/usr/bin:/usr/local/"
                "bin:/usr/local/sbin:/usr/sbin:/sbin:/bin:/usr/games:/usr/"
                "local/games:/snap/bin:/var/lib/snapd/snap/bin"};

            const std::vector<std::string>& pathList {
                Utils::String::delimitedStringToVector(pathVariable, ":")};

            // Using a temporary file is the only viable solution I've found to communicate
            // between the sandbox and the outside world.
            const std::string& tempFile {Utils::FileSystem::getAppDataDirectory() +
                                         "/.flatpak_emulator_binary_path.tmp"};

            std::string emulatorPath;

            for (auto it = pathList.cbegin(); it != pathList.cend(); ++it) {
                Utils::Platform::runSystemCommand("flatpak-spawn --host which " + *it + "/" +
                                                      executable + " > " + tempFile +
                                                      " 2>/dev/null",
                                                  true);
                std::ifstream tempFileStream;
                tempFileStream.open(tempFile, std::ios::binary);
                getline(tempFileStream, emulatorPath);
                tempFileStream.close();

                if (emulatorPath != "") {
                    emulatorPath = getParent(emulatorPath);
                    break;
                }
            }

            if (Utils::FileSystem::exists(tempFile))
                removeFile(tempFile);

            return emulatorPath;
#else
            std::string envPath;
            if (getenv("PATH") != nullptr)
                envPath = getenv("PATH");

            const std::vector<std::string>& pathList {
                Utils::String::delimitedStringToVector(envPath, ":")};

            std::string pathTest;

            for (auto it = pathList.cbegin(); it != pathList.cend(); ++it) {
                pathTest = it->c_str() + ("/" + executable);
                if (Utils::FileSystem::isRegularFile(pathTest) ||
                    Utils::FileSystem::isSymlink(pathTest))
                    return it->c_str();
            }
            return "";
#endif
#endif
        }

        void setExePath(const std::string& path)
        {
            constexpr int pathMax {32767};
#if defined(_WIN64)
            std::wstring result(pathMax, 0);
            if (GetModuleFileNameW(nullptr, &result[0], pathMax) != 0)
                exePath = Utils::String::wideStringToString(result);
#else
            std::string result(pathMax, 0);
            if (readlink("/proc/self/exe", &result[0], pathMax) != -1)
                exePath = result;
#endif
            exePath.erase(std::find(exePath.begin(), exePath.end(), '\0'), exePath.end());
            esBinary = exePath;
            exePath = getCanonicalPath(exePath);

#if defined(__FreeBSD__) || defined(__HAIKU__)
            // Fallback to getPathToBinary(argv[0]), needed as FreeBSD and Haiku do not
            // provide /proc/self/exe.
            if (exePath.empty()) {
                esBinary = getPathToBinary(path);
                exePath = getCanonicalPath(esBinary);
            }
#endif
            // Fallback to argv[0] if everything else fails, which is always the case on macOS.
            if (exePath.empty()) {
                esBinary = path;
                exePath = getCanonicalPath(path);
            }
            if (isRegularFile(exePath))
                exePath = getParent(exePath);

#if defined(APPIMAGE_BUILD)
            // We need to check that the APPIMAGE variable is available as the APPIMAGE_BUILD
            // build flag could have been passed without running as an actual AppImage.
            if (getenv("APPIMAGE") != nullptr)
                esBinary = getenv("APPIMAGE");
#endif
        }

        std::string getExePath()
        {
            // Return executable path.
            return exePath;
        }

        std::string getEsBinary()
        {
            // Return the absolute path to the ES-DE binary.
            return esBinary;
        }

        std::string getProgramDataPath()
        {
#if defined(__ANDROID__)
            return AndroidVariables::sInternalDataDirectory;
#elif defined(__HAIKU__)
            return "/boot/system/data/es-de";
#elif defined(__unix__)
    return installPrefix + "/share/es-de";
#else
    return "";
#endif
        }

        std::string getPreferredPath(const std::string& path)
        {
#if defined(_WIN64)
            std::string preferredPath {path};
            size_t offset {std::string::npos};
            // Convert '/' to '\\'
            while ((offset = preferredPath.find('/')) != std::string::npos)
                preferredPath.replace(offset, 1, "\\");
#else
            const std::string& preferredPath {path};
#endif
            return preferredPath;
        }

        std::string getGenericPath(const std::string& path)
        {
            std::string genericPath {path};
            size_t offset {std::string::npos};

            // Remove "\\\\?\\"
            if ((genericPath.find("\\\\?\\")) == 0)
                genericPath.erase(0, 4);

            // Convert '\\' to '/'
            while ((offset = genericPath.find('\\')) != std::string::npos)
                genericPath.replace(offset, 1, "/");

            // Remove double '/'
            while ((offset = genericPath.find("//")) != std::string::npos)
                genericPath.erase(offset, 1);

            // Remove trailing '/' when the path is more than a simple '/'
            while (genericPath.length() > 1 &&
                   ((offset = genericPath.find_last_of('/')) == (genericPath.length() - 1)))
                genericPath.erase(offset, 1);

            return genericPath;
        }

        namespace
        {
            // The std::filesystem path for the spelling the helpers use, and back. On Windows the
            // native wide string is required, as getFileSize() already does.
            std::filesystem::path toPath(const std::string& path)
            {
#if defined(_WIN64)
                return std::filesystem::path {
                    Utils::String::stringToWideString(getGenericPath(path))};
#else
                return std::filesystem::path {getGenericPath(path)};
#endif
            }

            std::string fromPath(const std::filesystem::path& path)
            {
#if defined(_WIN64)
                return getGenericPath(Utils::String::wideStringToString(path.generic_wstring()));
#else
                return getGenericPath(path.generic_string());
#endif
            }
        } // namespace

        std::string getEscapedPath(const std::string& path)
        {
            std::string escapedPath {getGenericPath(path)};

#if defined(_WIN64)
            // Windows escapes stuff by just putting everything in quotes.
            if (escapedPath.find(" ") != std::string::npos)
                return '"' + getPreferredPath(escapedPath) + '"';
            else
                return getPreferredPath(escapedPath);
#else
            // Insert a backslash before most characters that would mess up a bash path.
            const char* invalidChars {"\\ '\"!$^&*(){}[]?;<>|`#~\t"};
            const char* invalidChar {invalidChars};

            while (*invalidChar) {
                size_t start {0};
                size_t offset {0};

                while ((offset = escapedPath.find(*invalidChar, start)) != std::string::npos) {
                    start = offset + 1;

                    if ((offset == 0) || (escapedPath[offset - 1] != '\\')) {
                        escapedPath.insert(offset, 1, '\\');
                        ++start;
                    }
                }
                ++invalidChar;
            }

            // A newline or a carriage return cannot be escaped with a backslash in a portable
            // way, so each one is handed to the shell as a quoted literal instead.
            std::string quotedPath;
            for (char character : escapedPath) {
                if (character == '\n' || character == '\r') {
                    quotedPath += '\'';
                    quotedPath += character;
                    quotedPath += '\'';
                }
                else {
                    quotedPath += character;
                }
            }
            return quotedPath;
#endif
        }

        std::string getCanonicalPath(const std::string& path)
        {
            // Hack for builtin resources.
            if ((path.size() > 1) && (path[0] == ':') && (path[1] == '/'))
                return path;

            const std::string& genericPath {getGenericPath(path)};

            if (genericPath.empty())
                return genericPath;

            // weakly_canonical() resolves the part of the path that exists and normalizes the
            // remainder lexically. Unlike the hand-written rescan loop it replaces, it gives up
            // on a symlink cycle instead of following it forever.
            std::error_code errorCode;
#if defined(_WIN64)
            const std::filesystem::path widePath {Utils::String::stringToWideString(genericPath)};
            const std::filesystem::path canonicalPath {
                std::filesystem::weakly_canonical(widePath, errorCode)};

            if (errorCode)
                return getGenericPath(Utils::String::wideStringToString(
                    widePath.lexically_normal().generic_wstring()));

            return getGenericPath(
                Utils::String::wideStringToString(canonicalPath.generic_wstring()));
#else
            const std::filesystem::path canonicalPath {
                std::filesystem::weakly_canonical(genericPath, errorCode)};

            if (errorCode)
                return std::filesystem::path(genericPath).lexically_normal().generic_string();

            return getGenericPath(canonicalPath.generic_string());
#endif
        }

        std::string getAbsolutePath(const std::string& path, const std::string& base)
        {
            const std::string& absolutePath {getGenericPath(path)};
            const std::string& baseVar {isAbsolute(base) ? getGenericPath(base) :
                                                           getAbsolutePath(base)};

            return isAbsolute(absolutePath) ? absolutePath :
                                              getGenericPath(baseVar + "/" + absolutePath);
        }

        std::string getParent(const std::string& path)
        {
            return fromPath(toPath(path).parent_path());
        }

        std::string getFileName(const std::string& path)
        {
            return fromPath(toPath(path).filename());
        }

        std::string getStem(const std::string& path)
        {
            return fromPath(toPath(path).stem());
        }

        std::string getExtension(const std::string& path)
        {
            return fromPath(toPath(path).extension());
        }

        long getFileSize(const std::filesystem::path& path)
        {
            try {
#if defined(_WIN64)
                return static_cast<long>(std::filesystem::file_size(
                    Utils::String::stringToWideString(path.generic_string())));
#else
                return static_cast<long>(std::filesystem::file_size(path));
#endif
            }
            catch (std::filesystem::filesystem_error& error) {
                LOG(LogError) << "FileSystemUtil::getFileSize(): " << error.what();
                return -1;
            }
        }

        std::string expandHomePath(const std::string& path, const bool systemHome)
        {
            // Expand home path if ~ is used.
#if defined(__ANDROID__)
            return Utils::String::replace(path, "~", Utils::FileSystem::getHomePath());
#else
            if (systemHome)
                return Utils::String::replace(path, "~",
                                              Utils::FileSystem::getSystemHomeDirectory());
            else
                return Utils::String::replace(path, "~", Utils::FileSystem::getHomePath());
#endif
        }

        std::string resolveRelativePath(const std::string& path,
                                        const std::string& relativeTo,
                                        const bool allowHome)
        {
            const std::string& genericPath {getGenericPath(path)};
            const std::string& relativeToVar {getGenericPath(relativeTo)};

            // Nothing to resolve.
            if (!genericPath.length())
                return genericPath;

            // Replace './' with relativeToVar, which is always treated as a directory path.
            if ((genericPath[0] == '.') && (genericPath[1] == '/')) {
                // The filesystem root already ends with its separator.
                if (relativeToVar == "/")
                    return (relativeToVar + &(genericPath[2]));

                return (relativeToVar + &(genericPath[1]));
            }

            // Replace '~' with homePath.
            if (allowHome && (genericPath[0] == '~') && (genericPath[1] == '/'))
                return (getHomePath() + &(genericPath[1]));

            // Nothing to resolve.
            return genericPath;
        }

        std::string createRelativePath(const std::string& path,
                                       const std::string& relativeTo,
                                       const bool allowHome)
        {
            bool contains {false};
            std::string relativePath {removeCommonPath(path, relativeTo, contains)};

            if (contains)
                return ("./" + relativePath);

            if (allowHome) {
                relativePath = removeCommonPath(path, getHomePath(), contains);

                if (contains)
                    return ("~/" + relativePath);
            }
            return relativePath;
        }

        bool isNormalizedRelativePath(const std::string& path)
        {
            if (path.empty() || path.front() == '/' || path.back() == '/')
                return false;

            // A NUL cannot appear in a file name and would truncate the path in C interfaces.
            if (path.find('\0') != std::string::npos)
                return false;

            std::size_t start {0};
            while (start <= path.size()) {
                const std::size_t separator {path.find('/', start)};
                const std::size_t end {separator == std::string::npos ? path.size() : separator};
                const std::size_t length {end - start};

                // Consecutive separators produce an empty segment, which is never normalized.
                if (length == 0)
                    return false;

                if (length == 1 && path[start] == '.')
                    return false;

                if (length == 2 && path[start] == '.' && path[start + 1] == '.')
                    return false;

                if (separator == std::string::npos)
                    break;

                start = separator + 1;
            }

            return true;
        }

        std::string removeCommonPath(const std::string& path,
                                     const std::string& commonArg,
                                     bool& contains)
        {
            const std::string& genericPath {getGenericPath(path)};
            const std::string& common {getGenericPath(commonArg)};
            std::string relative;

            contains = false;

            // The common argument is always treated as a directory path, and the root itself
            // has no relative form.
            if (common.empty() || genericPath.empty() || genericPath == common)
                return genericPath;

            if (common == "/") {
                // A root of "/" already ends in the separator; the relative path starts after it.
                if (genericPath.size() < 2 || genericPath.front() != '/')
                    return genericPath;

                relative = genericPath.substr(1);
            }
            else {
                // A sibling that merely shares the string prefix is outside the directory.
                if (genericPath.size() <= common.size() ||
                    genericPath.compare(0, common.size(), common) != 0 ||
                    genericPath[common.size()] != '/')
                    return genericPath;

                relative = genericPath.substr(common.size() + 1);
            }

            if (!isNormalizedRelativePath(relative))
                return genericPath;

            contains = true;
            return relative;
        }

        std::string resolveSymlink(const std::string& path)
        {
            const std::string& genericPath {getGenericPath(path)};
            std::string resolved;

#if defined(_WIN64)
            std::wstring resolvedW;
            HANDLE hFile = CreateFileW(Utils::String::stringToWideString(genericPath).c_str(),
                                       FILE_READ_ATTRIBUTES, FILE_SHARE_READ, 0, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS, 0);
            if (hFile != INVALID_HANDLE_VALUE) {
                resolvedW.resize(
                    GetFinalPathNameByHandleW(hFile, nullptr, 0, FILE_NAME_NORMALIZED) + 1);
                if (GetFinalPathNameByHandleW(hFile, const_cast<LPWSTR>(resolvedW.data()),
                                              static_cast<DWORD>(resolvedW.size()),
                                              FILE_NAME_NORMALIZED) > 0) {
                    resolvedW.resize(resolvedW.size() - 2);
                    resolved = getGenericPath(Utils::String::wideStringToString(resolvedW));
                }
                CloseHandle(hFile);
            }
#else
            std::error_code errorCode;
            const std::filesystem::path target {
                std::filesystem::read_symlink(genericPath, errorCode)};

            if (!errorCode)
                resolved = getGenericPath(target.generic_string());
#endif
            return resolved;
        }

        bool copyFile(const std::string& sourcePath,
                      const std::string& destinationPath,
                      bool overwrite)
        {
            std::error_code error;
            const std::filesystem::copy_options options {
                overwrite ? std::filesystem::copy_options::overwrite_existing :
                            std::filesystem::copy_options::none};

            std::filesystem::copy_file(toPath(sourcePath), toPath(destinationPath), options,
                                       error);

            if (error) {
                LOG(LogError) << "Couldn't copy \"" << sourcePath << "\" to \""
                              << destinationPath << "\": " << error.message();
                return false;
            }

            return true;
        }

        bool renameFile(const std::string& sourcePath,
                        const std::string& destinationPath,
                        bool overwrite)
        {
            // Don't print any error message for a missing source file as Log will use this
            // function when initializing the logging. It would always generate an error in
            // case it's the first application start (as an old log file would then not exist).
            if (!Utils::FileSystem::exists(sourcePath)) {
                return true;
            }

            if (isDirectory(destinationPath)) {
                LOG(LogError) << "Destination file is actually a directory:";
                LOG(LogError) << destinationPath;
                return true;
            }

            if (!overwrite && Utils::FileSystem::exists(destinationPath)) {
                LOG(LogError) << "Destination file exists and the overwrite flag has not been set";
                return true;
            }

#if defined(_WIN64)
            return _wrename(Utils::String::stringToWideString(sourcePath).c_str(),
                            Utils::String::stringToWideString(destinationPath).c_str());
#else
            return std::rename(sourcePath.c_str(), destinationPath.c_str());
#endif
        }

        bool createEmptyFile(const std::filesystem::path& path)
        {
            const std::filesystem::path cleanPath {path.lexically_normal().make_preferred()};
            if (Utils::FileSystem::exists(path.string())) {
                LOG(LogError) << "Couldn't create target file \"" << cleanPath.string()
                              << "\" as it already exists";
                return false;
            }
#if defined(_WIN64)
            std::ofstream targetFile {Utils::String::stringToWideString(cleanPath.string()).c_str(),
                                      std::ios::binary};
#else
            std::ofstream targetFile {cleanPath, std::ios::binary};
#endif
            if (targetFile.fail()) {
                LOG(LogError) << "Couldn't create target file \"" << cleanPath.string()
                              << "\", permission problems?";
                targetFile.close();
                return false;
            }

            targetFile.close();
            return true;
        }

        bool createSymlink(const std::filesystem::path& source, const std::filesystem::path& target)
        {
            try {
                if (std::filesystem::is_directory(source))
                    std::filesystem::create_directory_symlink(source, target);
                else
                    std::filesystem::create_symlink(source, target);
            }
            catch (std::filesystem::filesystem_error& error) {
                LOG(LogError) << "FileSystemUtil::createSymlink(): " << error.what();
                return false;
            }
            return true;
        }

        bool removeFile(const std::string& path)
        {
            const std::string& genericPath {getGenericPath(path)};
            try {
#if defined(_WIN64)
                return std::filesystem::remove(Utils::String::stringToWideString(genericPath));
#else
                return std::filesystem::remove(genericPath);
#endif
            }
            catch (std::filesystem::filesystem_error& error) {
                LOG(LogError) << "FileSystemUtil::removeFile(): " << error.what();
                return false;
            }
        }

        bool removeDirectory(const std::string& path, bool recursive)
        {
            const std::string& genericPath {getGenericPath(path)};
            try {
#if defined(_WIN64)
                if (recursive)
                    return std::filesystem::remove_all(
                        Utils::String::stringToWideString(genericPath));
                else
                    return std::filesystem::remove(Utils::String::stringToWideString(genericPath));
#else
                if (recursive)
                    return std::filesystem::remove_all(genericPath);
                else
                    return std::filesystem::remove(genericPath);
#endif
            }
            catch (std::filesystem::filesystem_error& error) {
                LOG(LogError) << "FileSystemUtil::removeDirectory(): " << error.what();
                return false;
            }
        }

        bool createDirectory(const std::string& path)
        {
            std::error_code error;
            std::filesystem::create_directories(toPath(path), error);

            return !error;
        }

        bool exists(const std::string& path)
        {
            const std::string& genericPath {getGenericPath(path)};
            try {
#if defined(_WIN64)
                return std::filesystem::exists(Utils::String::stringToWideString(genericPath));
#else
                return std::filesystem::exists(genericPath);
#endif
            }
            catch (std::filesystem::filesystem_error& error) {
                LOG(LogError) << "FileSystemUtil::exists(): " << error.what();
                return false;
            }
        }

        bool driveExists(const std::string& path)
        {
#if defined(_WIN64)
            std::string genericPath {getGenericPath(path)};
            // Try to add a dot or a backslash and a dot depending on how the drive
            // letter was defined by the user.
            if (genericPath.length() == 2 && genericPath.at(1) == ':')
                genericPath += "\\.";
            else if (genericPath.length() == 3 && genericPath.at(1) == ':')
                genericPath += ".";

            return Utils::FileSystem::exists(genericPath);
#else
            return false;
#endif
        }

        bool isAbsolute(const std::string& path)
        {
            const std::string& genericPath {getGenericPath(path)};
            try {
#if defined(_WIN64)
                return ((genericPath.size() > 1) && (genericPath[1] == ':'));
#else
                return ((genericPath.size() > 0) && (genericPath[0] == '/'));
#endif
            }
            catch (...) {
                return false;
            }
        }

        bool isRegularFile(const std::string& path)
        {
            const std::string& genericPath {getGenericPath(path)};
            try {
#if defined(_WIN64)
                return std::filesystem::is_regular_file(
                    Utils::String::stringToWideString(genericPath));
#else
                return std::filesystem::is_regular_file(genericPath);
#endif
            }
            catch (std::filesystem::filesystem_error& error) {
                LOG(LogError) << "FileSystemUtil::isRegularFile(): " << error.what();
                return false;
            }
        }

        bool isDirectory(const std::string& path)
        {
            const std::string& genericPath {getGenericPath(path)};
            try {
#if defined(_WIN64)
                return std::filesystem::is_directory(
                    Utils::String::stringToWideString(genericPath));
#else
                return std::filesystem::is_directory(genericPath);
#endif
            }
            catch (std::filesystem::filesystem_error& error) {
                LOG(LogError) << "FileSystemUtil::isDirectory(): " << error.what();
                return false;
            }
        }

        bool isSymlink(const std::string& path)
        {
#if defined(__ANDROID__)
            // Symlinks are generally not supported on Android due to the Storage Access Framework
            // and the use of FAT/exFAT and NTFS filesystems.
            return false;
#endif
            const std::string& genericPath {getGenericPath(path)};
            try {
#if defined(_WIN64)
                return std::filesystem::is_symlink(Utils::String::stringToWideString(genericPath));
#else
                return std::filesystem::is_symlink(genericPath);
#endif
            }
            catch (std::filesystem::filesystem_error& error) {
                LOG(LogError) << "FileSystemUtil::isSymlink(): " << error.what();
                return false;
            }
        }

        bool isHidden(const std::string& path)
        {
            const std::string& genericPath {getGenericPath(path)};
#if defined(_WIN64)
            // Check for hidden attribute.
            const DWORD Attributes {
                GetFileAttributesW(Utils::String::stringToWideString(genericPath).c_str())};
            if ((Attributes != INVALID_FILE_ATTRIBUTES) && (Attributes & FILE_ATTRIBUTE_HIDDEN))
                return true;
#endif

            // Filenames starting with . are hidden in Linux, but
            // we do this check for windows as well.
            if (getFileName(genericPath)[0] == '.')
                return true;

            return false;
        }

    } // namespace FileSystem

} // namespace Utils
