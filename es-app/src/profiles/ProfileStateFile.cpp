//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileStateFile.cpp
//
//  File operations for the personal-state store.
//

#include "profiles/ProfileStateFile.h"

#include <cerrno>
#include <cstdio>
#include <ctime>

#if !defined(_WIN64)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
    class SystemFileOperations final : public ProfileState::FileOperations
    {
    public:
        ProfileState::ReadStatus readFile(const std::string& path, std::string& contents) override
        {
            contents.clear();

#if defined(_WIN64)
            FILE* file {fopen(path.c_str(), "rb")};
            if (file == nullptr)
                return ProfileState::ReadStatus::missing;

            char buffer[16384];
            std::size_t read {0};
            while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0)
                contents.append(buffer, read);
            const bool failed {ferror(file) != 0};
            fclose(file);
            return failed ? ProfileState::ReadStatus::error : ProfileState::ReadStatus::ok;
#else
            const int descriptor {open(path.c_str(), O_RDONLY)};
            if (descriptor < 0)
                return errno == ENOENT ? ProfileState::ReadStatus::missing :
                                         ProfileState::ReadStatus::error;

            char buffer[16384];
            while (true) {
                const ssize_t read {::read(descriptor, buffer, sizeof(buffer))};
                if (read < 0) {
                    close(descriptor);
                    return ProfileState::ReadStatus::error;
                }
                if (read == 0)
                    break;
                contents.append(buffer, static_cast<std::size_t>(read));
            }

            close(descriptor);
            return ProfileState::ReadStatus::ok;
#endif
        }

        bool writeFile(const std::string& path, const std::string& contents) override
        {
#if defined(_WIN64)
            FILE* file {fopen(path.c_str(), "wb")};
            if (file == nullptr)
                return false;

            const bool written {fwrite(contents.data(), 1, contents.size(), file) ==
                                contents.size()};
            const bool flushed {fflush(file) == 0};
            fclose(file);
            return written && flushed;
#else
            const int descriptor {open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644)};
            if (descriptor < 0)
                return false;

            std::size_t written {0};
            while (written < contents.size()) {
                const ssize_t result {
                    ::write(descriptor, contents.data() + written, contents.size() - written)};
                if (result <= 0) {
                    close(descriptor);
                    return false;
                }
                written += static_cast<std::size_t>(result);
            }

            close(descriptor);
            return true;
#endif
        }

        bool syncFile(const std::string& path) override
        {
#if defined(_WIN64)
            (void)path;
            return true;
#else
            const int descriptor {open(path.c_str(), O_RDONLY)};
            if (descriptor < 0)
                return false;
            const bool synced {fsync(descriptor) == 0};
            close(descriptor);
            return synced;
#endif
        }

        bool renameFile(const std::string& from, const std::string& to) override
        {
            return std::rename(from.c_str(), to.c_str()) == 0;
        }

        bool syncDirectory(const std::string& directory) override
        {
#if defined(_WIN64)
            (void)directory;
            return true;
#else
#if defined(O_DIRECTORY)
            const int flags {O_RDONLY | O_DIRECTORY};
#else
            const int flags {O_RDONLY};
#endif
            const int descriptor {open(directory.c_str(), flags)};
            if (descriptor < 0)
                return false;
            const bool synced {fsync(descriptor) == 0};
            close(descriptor);
            return synced;
#endif
        }

        bool removeFile(const std::string& path) override { return std::remove(path.c_str()) == 0; }
    };
} // namespace

namespace
{
    std::string parentDirectory(const std::string& path)
    {
        const std::size_t separator {path.find_last_of('/')};
        return separator == std::string::npos ? std::string {"."} : path.substr(0, separator);
    }

    class SystemClock final : public ProfileState::Clock
    {
    public:
        std::string utcTimestamp() override
        {
            const std::time_t now {std::time(nullptr)};
            std::tm utc {};
#if defined(_WIN64)
            gmtime_s(&utc, &now);
#else
            gmtime_r(&now, &utc);
#endif
            char buffer[32];
            std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%S", &utc);
            return buffer;
        }
    };
} // namespace

ProfileState::FileOperations& ProfileState::systemFileOperations()
{
    static SystemFileOperations operations;
    return operations;
}

ProfileState::Clock& ProfileState::systemClock()
{
    static SystemClock clock;
    return clock;
}

ProfileState::PublishResult ProfileState::publishFileAtomically(FileOperations& files,
                                                                const std::string& path,
                                                                const std::string& bytes,
                                                                const std::string& previousBytes,
                                                                const bool hasPreviousFile)
{
    const std::string tempPath {path + ".tmp"};
    if (!files.writeFile(tempPath, bytes))
        return {false, "could not write " + tempPath};
    if (!files.syncFile(tempPath))
        return {false, "could not sync " + tempPath};

    if (hasPreviousFile) {
        // The backup is published atomically too, so a failure cannot leave a truncated .bak.
        const std::string backupPath {path + ".bak"};
        const std::string backupTempPath {backupPath + ".tmp"};
        if (!files.writeFile(backupTempPath, previousBytes))
            return {false, "could not write " + backupTempPath};
        if (!files.syncFile(backupTempPath))
            return {false, "could not sync " + backupTempPath};
        if (!files.renameFile(backupTempPath, backupPath))
            return {false, "could not rename " + backupTempPath + " to " + backupPath};
    }

    if (!files.renameFile(tempPath, path))
        return {false, "could not rename " + tempPath + " to " + path};

    const std::string directory {parentDirectory(path)};
    if (!files.syncDirectory(directory))
        return {false, "could not sync the directory " + directory};

    return {true, ""};
}
