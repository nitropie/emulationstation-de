//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileStateFile.h
//
//  File operations for the personal-state store.
//
//  The store takes them as a constructor argument so tests can inject a failure on rename or on
//  the directory sync without a global hook. The shipping implementation is
//  systemFileOperations().
//

#ifndef ES_APP_PROFILES_PROFILE_STATE_FILE_H
#define ES_APP_PROFILES_PROFILE_STATE_FILE_H

#include <string>

namespace ProfileState
{
    enum class ReadStatus {
        ok,
        // The file does not exist.
        missing,
        // The file exists but could not be read.
        error
    };

    class FileOperations
    {
    public:
        virtual ~FileOperations() = default;

        virtual ReadStatus readFile(const std::string& path, std::string& contents) = 0;
        virtual bool writeFile(const std::string& path, const std::string& contents) = 0;
        // Flushes one file to stable storage.
        virtual bool syncFile(const std::string& path) = 0;
        // Atomically replaces `to` with `from`.
        virtual bool renameFile(const std::string& from, const std::string& to) = 0;
        // Makes the directory entries themselves durable.
        virtual bool syncDirectory(const std::string& directory) = 0;
        virtual bool removeFile(const std::string& path) = 0;
    };

    FileOperations& systemFileOperations();

    struct PublishResult {
        bool success {false};
        // Why the publish failed. Empty on success.
        std::string reason;
    };

    // Publishes a file atomically: a temp file in the same directory, fsync, the previous file
    // as <path>.bak through its own temp file, an atomic rename and a directory sync. Shared by
    // the personal-state store and the profile metadata file.
    PublishResult publishFileAtomically(FileOperations& files,
                                        const std::string& path,
                                        const std::string& bytes,
                                        const std::string& previousBytes,
                                        bool hasPreviousFile);

    // Timestamps for the files a recovery preserves. Injected so tests are deterministic.
    class Clock
    {
    public:
        virtual ~Clock() = default;
        // UTC in the compact form YYYYMMDDTHHMMSS.
        virtual std::string utcTimestamp() = 0;
    };

    Clock& systemClock();
} // namespace ProfileState

#endif // ES_APP_PROFILES_PROFILE_STATE_FILE_H
