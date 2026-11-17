//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ProfileRecordKey.cpp
//
//  Identity of one personal-state record.
//

#include "profiles/ProfileRecordKey.h"

#include <utility>

namespace
{
    // One segment of a relative path: non-empty and neither "." nor "..".
    bool isNormalizedPath(const std::string& path)
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

} // namespace

ProfileRecordKey::ProfileRecordKey(std::string system,
                                   const ProfileRecordType type,
                                   std::string path)
    : mSystem {std::move(system)}
    , mPath {std::move(path)}
    , mType {type}
{
}

std::optional<ProfileRecordKey> ProfileRecordKey::fromPath(std::string system,
                                                           const ProfileRecordType type,
                                                           std::string path)
{
    if (system.empty() || !isNormalizedPath(path))
        return std::nullopt;

    return ProfileRecordKey {std::move(system), type, std::move(path)};
}

bool ProfileRecordKey::operator==(const ProfileRecordKey& other) const
{
    return mSystem == other.mSystem && mType == other.mType && mPath == other.mPath;
}

bool ProfileRecordKey::operator<(const ProfileRecordKey& other) const
{
    if (mSystem != other.mSystem)
        return mSystem < other.mSystem;

    if (mType != other.mType)
        return static_cast<int>(mType) < static_cast<int>(other.mType);

    return mPath < other.mPath;
}

std::string ProfileRecordKey::describe() const
{
    return mSystem + "/" + (mType == ProfileRecordType::game ? "game" : "folder") + ":" + mPath;
}
