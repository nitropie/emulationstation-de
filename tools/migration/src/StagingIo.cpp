//  SPDX-License-Identifier: MIT
//
//  Descriptor-relative staging I/O for the 6B helper. See StagingIo.h.
//

#include "StagingIo.h"

#include "Sha256.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>

namespace
{
    bool openDirectoryAt(int parent, const std::string& name, int& result)
    {
        result = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        return result >= 0;
    }

    bool readAll(int fd, std::size_t limit, std::string& bytes, std::string& error)
    {
        bytes.clear();
        std::array<char, 65536> buffer {};
        while (true) {
            const ssize_t count {::read(fd, buffer.data(), buffer.size())};
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                error = std::string {"cannot read the file: "} + std::strerror(errno);
                return false;
            }
            if (count == 0)
                break;
            bytes.append(buffer.data(), static_cast<std::size_t>(count));
            if (bytes.size() > limit) {
                error = "staging input is larger than the accepted bound";
                return false;
            }
        }
        return true;
    }
} // namespace

bool MigrationSplit::validRelative(const std::string& path)
{
    if (path.empty() || path.front() == '/')
        return false;
    if (path.find('\0') != std::string::npos)
        return false;
    std::size_t start {0};
    while (start <= path.size()) {
        const std::size_t end {path.find('/', start)};
        const std::string part {
            path.substr(start, end == std::string::npos ? std::string::npos : end - start)};
        if (part.empty() || part == "." || part == "..")
            return false;
        if (end == std::string::npos)
            return true;
        start = end + 1;
    }
    return false;
}

bool MigrationSplit::openRelative(int rootFd,
                                  const std::string& relative,
                                  int flags,
                                  mode_t mode,
                                  bool create,
                                  int* parentResult,
                                  int& result,
                                  std::string& error)
{
    if (!validRelative(relative)) {
        error = "invalid staging-relative name";
        return false;
    }
    int current {::dup(rootFd)};
    if (current < 0) {
        error = "cannot duplicate the staging descriptor";
        return false;
    }
    std::size_t start {0};
    while (true) {
        const std::size_t end {relative.find('/', start)};
        const std::string part {
            relative.substr(start, end == std::string::npos ? std::string::npos : end - start)};
        if (end == std::string::npos) {
            int parent {parentResult != nullptr ? ::dup(current) : -1};
            if (parentResult != nullptr && parent < 0) {
                error = "cannot duplicate the containing directory";
                ::close(current);
                return false;
            }
            result = ::openat(current, part.c_str(), flags | O_NOFOLLOW | O_CLOEXEC, mode);
            const int saved {errno};
            ::close(current);
            if (result < 0) {
                if (parentResult != nullptr)
                    ::close(parent);
                errno = saved;
                error = std::string {"cannot open "} + part + ": " + std::strerror(saved);
                return false;
            }
            if (parentResult != nullptr)
                *parentResult = parent;
            return true;
        }
        int next {-1};
        if (!openDirectoryAt(current, part, next)) {
            if (!create || errno != ENOENT) {
                const int saved {errno};
                ::close(current);
                errno = saved;
                error = std::string {"cannot open directory "} + part + ": " +
                        std::strerror(saved);
                return false;
            }
            if (::mkdirat(current, part.c_str(), 0700) != 0 && errno != EEXIST) {
                const int saved {errno};
                ::close(current);
                errno = saved;
                error = std::string {"cannot create directory "} + part + ": " +
                        std::strerror(saved);
                return false;
            }
            if (!openDirectoryAt(current, part, next)) {
                const int saved {errno};
                ::close(current);
                errno = saved;
                error = std::string {"cannot reopen directory "} + part + ": " +
                        std::strerror(saved);
                return false;
            }
            ::fchmod(next, 0700);
        }
        ::close(current);
        current = next;
        start = end + 1;
    }
}

bool MigrationSplit::readStagingFile(int rootFd,
                                     const std::string& relative,
                                     std::size_t limit,
                                     std::string& bytes,
                                     std::string& error)
{
    int fd {-1};
    if (!openRelative(rootFd, relative, O_RDONLY | O_NONBLOCK, 0, false, nullptr, fd, error))
        return false;
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(fd);
        error = "staging input is not a regular file";
        return false;
    }
    const bool ok {readAll(fd, limit, bytes, error)};
    ::close(fd);
    return ok;
}

bool MigrationSplit::writeNoClobber(int rootFd,
                                    const std::string& relative,
                                    const std::string& bytes,
                                    std::string& error)
{
    error.clear();
    int fd {-1};
    int directory {-1};
    if (openRelative(rootFd, relative, O_WRONLY | O_CREAT | O_EXCL, 0600, true, &directory, fd,
                     error)) {
        std::size_t written {0};
        while (written < bytes.size()) {
            const ssize_t count {::write(fd, bytes.data() + written, bytes.size() - written)};
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                error = std::string {"cannot write the staged file: "} + std::strerror(errno);
                ::close(fd);
                ::close(directory);
                return false;
            }
            written += static_cast<std::size_t>(count);
        }
        ::fchmod(fd, 0600);
        ::fsync(fd);
        ::close(fd);
        // The containing directory entry must be durable too; a short file is
        // what a crash before this point leaves for the disposable-tree rule.
        ::fsync(directory);
        ::close(directory);
        return true;
    }
    if (errno != EEXIST)
        return false;

    std::string existing;
    if (!readStagingFile(rootFd, relative, kMaxGamelistBytes, existing, error))
        return false;
    if (existing != bytes) {
        error = "an existing staged file has different bytes: " + relative;
        return false;
    }
    error.clear();
    return true;
}

bool MigrationSplit::digestMatches(const std::string& bytes, const std::string& expected)
{
    return hexDigest(bytes) == expected;
}

std::string MigrationSplit::hexDigest(const std::string& bytes)
{
    Sha256 hash;
    hash.update(bytes);
    return hash.hex();
}

bool MigrationSplit::validDigest(const std::string& value)
{
    if (value.size() != 64)
        return false;
    for (char character : value) {
        const bool digit {character >= '0' && character <= '9'};
        const bool lower {character >= 'a' && character <= 'f'};
        if (!digit && !lower)
            return false;
    }
    return true;
}
