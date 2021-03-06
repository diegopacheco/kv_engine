/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include <mcbp/mcbp.h>
#include <mcbp/protocol/opcode.h>

#include <cJSON_utils.h>
#include <platform/dirutils.h>
#include <platform/memorymap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>

namespace cb {
namespace mcbp {
namespace sla {

static std::chrono::nanoseconds parseThresholdEntry(const cJSON& doc);

/**
 * Merge the content of document 2 into document 1 by overwriting
 * all values in document 1 with the value found in document 2.
 *
 * @param doc1 the resulting document
 * @param doc2 the document to remove the values from
 */
static void merge_docs(cJSON& doc1, const cJSON& doc2);

/**
 * The backing store for all of the thresholds. In order to make it easy
 * for ourself without any locking, just create a fixed array of atomics
 * and read out of it. It means that during "reinitializaiton" we might
 * return incorrect values, but let's just ignore that. In a deployed
 * system we'll initialize this during startup, and run with that
 * configuration until we stop.
 */
static std::array<std::atomic<std::chrono::nanoseconds>, 0x100> threshold;

unique_cJSON_ptr to_json() {
    unique_cJSON_ptr ret(cJSON_CreateObject());
    cJSON_AddNumberToObject(ret.get(), "version", 1);
    cJSON_AddStringToObject(
            ret.get(), "comment", "Current MCBP SLA configuration");

    for (unsigned int ii = 0; ii < threshold.size(); ++ii) {
        try {
            auto opcode = cb::mcbp::ClientOpcode(ii);
            std::string cmd = ::to_string(opcode);
            unique_cJSON_ptr obj(cJSON_CreateObject());
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    threshold[ii].load(std::memory_order_relaxed));
            cJSON_AddNumberToObject(obj.get(), "slow", ms.count());
            cJSON_AddItemToObject(ret.get(), cmd.c_str(), obj.release());
        } catch (const std::exception&) {
            // unknown command. ignore
        }
    }

    return ret;
}

std::chrono::nanoseconds getSlowOpThreshold(cb::mcbp::ClientOpcode opcode) {
    // This isn't really safe, but we don't want to use proper synchronization
    // in this case as it is part of the command execution for _all_ commands.
    // The _worst case_ scenario is that our reporting is incorrect while
    // we're reconfiguring the system.
    //
    // During reconfiguration we'll first try to look up the default value,
    // then initialize all of the entries with the default value. We'll then
    // apply the value for each of the individual entries.
    return threshold[uint8_t(opcode)].load(std::memory_order_relaxed);
}

/**
 * Read and merge all of the files specified in the system default locations:
 *
 *     /etc/couchbase/kv/opcode-attributes.json
 *     /etc/couchbase/kv/opcode-attributes.d/<*.json>
 *
 * @param root the root directory (prepend to the paths above)
 * @return The merged all of the on-disk files
 */
static unique_cJSON_ptr mergeFilesOnDisk(const std::string& root) {
    // First try to read the system default
    std::string system = root + "/etc/couchbase/kv/opcode-attributes.json";
    cb::io::sanitizePath(system);

    unique_cJSON_ptr configuration;

    if (cb::io::isFile(system)) {
        cb::MemoryMappedFile map(system.c_str(),
                                 cb::MemoryMappedFile::Mode::RDONLY);
        map.open();
        std::string string{static_cast<char*>(map.getRoot()), map.getSize()};
        unique_cJSON_ptr doc(cJSON_Parse(string.c_str()));
        if (!doc) {
            throw std::invalid_argument(
                    "cb::mcbp::sla::reconfigure: Invalid json in '" + system +
                    "'");
        }
        reconfigure(*doc, false);
        std::swap(configuration, doc);
    }

    // Replace .json with .d
    system.resize(system.size() - 4);
    system.push_back('d');

    if (cb::io::isDirectory(system)) {
        auto files = cb::io::findFilesWithPrefix(system, "");
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            // Skip files which don't end with ".json"
            if (file.find(".json") != file.size() - 5) {
                continue;
            }

            cb::MemoryMappedFile map(file.c_str(),
                                     cb::MemoryMappedFile::Mode::RDONLY);
            map.open();
            std::string string{static_cast<char*>(map.getRoot()),
                               map.getSize()};
            unique_cJSON_ptr doc(cJSON_Parse(string.c_str()));
            if (!doc) {
                throw std::invalid_argument(
                        "cb::mcbp::sla::reconfigure: Invalid json in '" + file +
                        "'");
            }
            reconfigure(*doc, false);
            if (!configuration) {
                std::swap(configuration, doc);
            } else {
                merge_docs(*configuration, *doc);
            }
        }
    }

    return configuration;
}

void reconfigure(const std::string& root) {
    auto configuration = mergeFilesOnDisk(root);

    if (configuration) {
        reconfigure(*configuration);
    }
}

void reconfigure(const std::string& root, const cJSON& override) {
    auto configuration = mergeFilesOnDisk(root);

    if (configuration) {
        merge_docs(*configuration, override);
        reconfigure(*configuration);
    } else {
        reconfigure(override);
    }
}

/**
 * Reconfigure the system with the provided JSON document by first
 * trying to look up the default entry. If found we'll be setting
 * all of the entries in our map to that value before iterating over
 * all of the entries in the JSON document and try to update that
 * single command.
 *
 * The format of the JSON document looks like:
 *
 *     {
 *       "version": 1,
 *       "default": {
 *         "slow": 500
 *       },
 *       "get": {
 *         "slow": 100
 *       },
 *       "compact_db": {
 *         "slow": "30 m"
 *       }
 *     }
 */
void reconfigure(const cJSON& doc, bool apply) {
    cJSON* root = const_cast<cJSON*>(&doc);

    // Check the version!
    const cJSON* version = cJSON_GetObjectItem(root, "version");
    if (version == nullptr) {
        throw std::invalid_argument(
                "cb::mcbp::sla::reconfigure: Missing mandatory element "
                "'version'");
    }

    if (version->type != cJSON_Number) {
        throw std::invalid_argument(
                "cb::mcbp::sla::reconfigure: 'version' should be a number");
    }

    if (version->valueint != 1) {
        throw std::invalid_argument(
                "cb::mcbp::sla::reconfigure: Unsupported version: " +
                std::to_string(version->valueint));
    }

    // Check if we've got a default entry:
    cJSON* obj = cJSON_GetObjectItem(root, "default");
    if (obj != nullptr) {
        // Handle default entry
        auto val = parseThresholdEntry(*obj);
        if (apply) {
            for (auto& t : threshold) {
                t.store(val, std::memory_order_relaxed);
            }
        }
    }

    // Time to look at each of the individual entries:
    for (obj = root->child; obj != nullptr; obj = obj->next) {
        if (strcmp(obj->string, "version") == 0 ||
            strcmp(obj->string, "default") == 0 ||
            strcmp(obj->string, "comment") == 0) {
            // Ignore these entries
            continue;
        }

        cb::mcbp::ClientOpcode opcode;
        try {
            opcode = to_opcode(obj->string);
        } catch (const std::invalid_argument&) {
            throw std::invalid_argument(
                    std::string{
                            "cb::mcbp::sla::reconfigure: Unknown command '"} +
                    obj->string + "'");
        }
        auto value = parseThresholdEntry(*obj);
        if (apply) {
            threshold[uint8_t(opcode)].store(value, std::memory_order_relaxed);
        }
    }
}

static std::chrono::nanoseconds parseThresholdEntry(const cJSON& doc) {
    if (doc.type != cJSON_Object) {
        throw std::invalid_argument(
                "cb::mcbp::sla::parseThresholdEntry: Entry '" +
                std::string{doc.string} + "' is not an object");
    }

    cJSON* root = const_cast<cJSON*>(&doc);
    auto* val = cJSON_GetObjectItem(root, "slow");
    if (val == nullptr) {
        throw std::invalid_argument(
                "cb::mcbp::sla::parseThresholdEntry: Entry '" +
                std::string{doc.string} +
                "' does not contain a mandatory 'slow' entry");
    }

    if (val->type == cJSON_Number) {
        return std::chrono::milliseconds(val->valueint);
    }

    if (val->type != cJSON_String) {
        throw std::invalid_argument(
                "cb::mcbp::sla::parseThresholdEntry: Entry '" +
                std::string{doc.string} + "' is not a value or a string");
    }

    // Try to parse the string. It should be of the following format:
    //
    // value [specifier]
    //
    // where the specifier may be:
    //    ns / nanoseconds
    //    us / microseconds
    //    ms / milliseconds
    //    s / seconds
    //    m / minutes
    //    h / hours
    std::size_t pos = 0;
    auto value = std::stoi(std::string(val->valuestring), &pos);

    // trim off whitespace
    while (std::isspace(val->valuestring[pos])) {
        ++pos;
    }

    std::string specifier{val->valuestring + pos};
    // Trim off trailing whitespace
    pos = specifier.find(' ');
    if (pos != std::string::npos) {
        specifier.resize(pos);
    }

    if (specifier == "ns" || specifier == "nanoseconds") {
        return std::chrono::nanoseconds(value);
    }

    if (specifier == "us" || specifier == "microseconds") {
        return std::chrono::microseconds(value);
    }

    if (specifier.empty() || specifier == "ms" || specifier == "milliseconds") {
        return std::chrono::milliseconds(value);
    }

    if (specifier == "s" || specifier == "seconds") {
        return std::chrono::seconds(value);
    }

    if (specifier == "m" || specifier == "minutes") {
        return std::chrono::minutes(value);
    }

    if (specifier == "h" || specifier == "hours") {
        return std::chrono::hours(value);
    }

    throw std::invalid_argument("cb::mcbp::sla::parseThresholdEntry: Entry '" +
                                std::string{doc.valuestring} +
                                "' contains an unknown specifier: '" +
                                specifier + "'");
}

static void merge_docs(cJSON& doc1, const cJSON& doc2) {
    for (auto* obj = doc2.child; obj != nullptr; obj = obj->next) {
        if (strcmp(obj->string, "version") == 0 ||
            strcmp(obj->string, "comment") == 0) {
            // Ignore these entries
            continue;
        }

        // For some reason we don't have a slow entry!
        auto* slow = cJSON_GetObjectItem(obj, "slow");
        if (slow == nullptr) {
            continue;
        }

        // Try to nuke it from the first one.
        auto* to_nuke = cJSON_DetachItemFromObject(&doc1, obj->string);
        if (to_nuke) {
            cJSON_Delete(to_nuke);
        }

        unique_cJSON_ptr entry(cJSON_CreateObject());
        if (slow->type == cJSON_Number) {
            cJSON_AddNumberToObject(entry.get(), "slow", slow->valueint);
        } else {
            cJSON_AddStringToObject(entry.get(), "slow", slow->valuestring);
        }
        cJSON_AddItemToObject(&doc1, obj->string, entry.release());
    }
}

} // namespace sla
} // namespace mcbp
} // namespace cb
