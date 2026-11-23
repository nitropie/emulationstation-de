#  SPDX-License-Identifier: MIT
#
#  bad-penguin 6B staging helper: its own module so no new file lands under
#  es-app/. It links the existing es-app-objects library for the two
#  serializers and never initializes the frontend.

add_executable(es-de-migration
               src/MigrationTool.cpp
               src/MigrationSplit.cpp
               src/Sha256.cpp
               src/StagingIo.cpp)
target_include_directories(es-de-migration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(es-de-migration ${COMMON_LIBRARIES} ${CMAKE_DL_LIBS} es-app-objects
                                      es-core lunasvg::lunasvg)

if(BUILD_TESTS)
    add_executable(es-migration-tests
                   src/MigrationSplit.cpp
                   src/Sha256.cpp
                   src/StagingIo.cpp
                   tests/MigrationSplitTests.cpp
                   ${CMAKE_SOURCE_DIR}/es-core/tests/TestMain.cpp
                   ${CMAKE_SOURCE_DIR}/es-core/tests/TestHarness.h
                   ${CMAKE_SOURCE_DIR}/es-app/tests/TestEnvironment.cpp)
    target_include_directories(es-migration-tests PRIVATE
                               ${CMAKE_CURRENT_SOURCE_DIR}/src
                               ${CMAKE_SOURCE_DIR}/es-core/tests
                               ${CMAKE_SOURCE_DIR}/es-app/tests
                               ${CMAKE_SOURCE_DIR}/es-app/src)
    target_compile_definitions(es-migration-tests PRIVATE
                               MIGRATION_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    target_link_libraries(es-migration-tests ${COMMON_LIBRARIES} ${CMAKE_DL_LIBS} es-app-objects
                                             es-core lunasvg::lunasvg)
    add_test(NAME es-migration-tests COMMAND es-migration-tests)
    set_tests_properties(es-migration-tests PROPERTIES TIMEOUT 120)
endif()
