//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GamelistFileParser.h
//
//  Parses and updates the gamelist.xml files.
//

#ifndef ES_APP_GAMELIST_FILE_PARSER_H
#define ES_APP_GAMELIST_FILE_PARSER_H

#include <string>
#include <vector>

class FileData;
class SystemData;

namespace GamelistFileParser
{
    // Loads gamelist.xml data into a SystemData.
    void parseGamelist(SystemData* system);

    // Writes currently loaded metadata for a SystemData to gamelist.xml.
    void updateGamelist(SystemData* system,
                        const bool updateAlternativeEmulator = false,
                        const bool updateScreenLaunch = false);

    // The shared-only writer for the offline library split: rewrites the given records with their
    // shared fields only, drops the system-preference elements and keeps child elements that it
    // does not recognize. The legacy writer above is not routed through it and stays
    // byte-identical, including its loss of unrecognized child elements. Returns an empty string
    // when the existing document cannot be used. No frontend caller.
    std::string writeSharedDocument(const std::string& existingBytes,
                                    const SystemData& system,
                                    const std::vector<const FileData*>& records);

} // namespace GamelistFileParser

#endif // ES_APP_GAMELIST_FILE_PARSER_H
