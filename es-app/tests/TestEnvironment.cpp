//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  TestEnvironment.cpp
//

#include "TestEnvironment.h"

#include <cstdlib>

namespace TestEnvironment
{
    const std::filesystem::path& appDataDirectory()
    {
        static const std::filesystem::path path {[] {
            const std::filesystem::path directory {
                std::filesystem::temp_directory_path() / "es-de-appdata-test"};
            std::filesystem::remove_all(directory);
            std::filesystem::create_directories(directory);
            setenv("ESDE_APPDATA_DIR", directory.c_str(), 1);
            return directory;
        }()};

        return path;
    }
} // namespace TestEnvironment
