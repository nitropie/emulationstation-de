//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  TestEnvironment.h
//
//  Shared environment for the es-app test cases that need an application data directory.
//

#ifndef ES_APP_TESTS_TEST_ENVIRONMENT_H
#define ES_APP_TESTS_TEST_ENVIRONMENT_H

#include <filesystem>

namespace TestEnvironment
{
    // The application data directory for the test process. The first call sets ESDE_APPDATA_DIR,
    // before any settings or file-system reader caches the path, and returns the same directory to
    // every later caller.
    const std::filesystem::path& appDataDirectory();
} // namespace TestEnvironment

#endif // ES_APP_TESTS_TEST_ENVIRONMENT_H
