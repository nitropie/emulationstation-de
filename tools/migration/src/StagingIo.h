//  SPDX-License-Identifier: MIT
//
//  Descriptor-relative staging I/O for the 6B helper.
//
//  Every name is relative to one inherited staging descriptor, every component
//  is opened with O_NOFOLLOW, reads are O_NONBLOCK and checked with S_ISREG,
//  and writes use O_EXCL with an identical-bytes idempotent path. The staging
//  tree is disposable: a run that stops between creating a file and finishing
//  it leaves a short file, a rerun refuses it as different bytes, and the
//  operator discards the staging root and starts again. No repair is
//  attempted.
//

#ifndef ES_TOOLS_MIGRATION_STAGING_IO_H
#define ES_TOOLS_MIGRATION_STAGING_IO_H

#include <sys/types.h>

#include <cstddef>
#include <string>

namespace MigrationSplit
{
    constexpr std::size_t kMaxRequestBytes {4u << 20};
    constexpr std::size_t kMaxGamelistBytes {256u << 20};

    bool validRelative(const std::string& path);

    // Opens `relative` with no-follow components below `rootFd`. `create`
    // creates missing intermediate directories with mode 0700. When
    // `parentResult` is not null it receives a descriptor for the containing
    // directory, which the caller closes.
    bool openRelative(int rootFd,
                      const std::string& relative,
                      int flags,
                      mode_t mode,
                      bool create,
                      int* parentResult,
                      int& result,
                      std::string& error);

    bool readStagingFile(int rootFd,
                         const std::string& relative,
                         std::size_t limit,
                         std::string& bytes,
                         std::string& error);

    // Writes `bytes` with mode 0600, fsyncs the file and its directory. An
    // existing file with identical bytes is accepted; any other existing file
    // is refused and left untouched.
    bool writeNoClobber(int rootFd,
                        const std::string& relative,
                        const std::string& bytes,
                        std::string& error);

    bool digestMatches(const std::string& bytes, const std::string& expected);
    std::string hexDigest(const std::string& bytes);
    bool validDigest(const std::string& value);
} // namespace MigrationSplit

#endif // ES_TOOLS_MIGRATION_STAGING_IO_H
