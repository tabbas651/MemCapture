/*
* If not stated otherwise in this file or this component's LICENSE file the
* following copyright and licenses apply:
*
* Copyright 2023 Sky UK
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <unistd.h>
#include <getopt.h>
#include <csignal>
#include <fstream>
#include <optional>
#include <filesystem>

#include "Platform.h"
#include "Log.h"
#include "ProcessMetric.h"
#include "MemoryMetric.h"
#include "Metadata.h"
#include "GroupManager.h"
#include "ConditionVariable.h"

#ifdef ENABLE_CPU_IDLE_METRICS
#include "CpuIdleMetric.h"
#endif

#include "inja/inja.hpp"

#ifdef USE_BREAKPAD
#include "breakpad_wrapper.h"
#endif

#define INCBIN_STYLE INCBIN_STYLE_SNAKE
#define INCBIN_PREFIX g_

#include <incbin.h>

#include "JsonReportGenerator.h"

INCBIN(templateHtml, "./templates/template.html");

static int gDuration = 30;
static Platform gPlatform = Platform::AMLOGIC;

// Default to save in current directory if not specified
static std::filesystem::path gOutputDirectory = std::filesystem::current_path() / "MemCaptureReport";

static bool gJson = false;
static bool gCpuIdle = false;

bool gEnableGroups = false;
static std::filesystem::path gGroupsFile;

ConditionVariable gStop;
std::mutex gLock;
bool gEarlyTermination = false;

static void displayUsage()
{
    printf("Usage: MemCapture <option(s)>\n");
    printf("    Utility to capture memory statistics\n\n");
    printf("    -h, --help          Print this help and exit\n");
    printf("    -o, --output-dir    Directory to save results in\n");
    printf("    -j, --json          Save data as JSON in addition to HTML report\n");
    printf("    -d, --duration      Amount of time (in seconds) to capture data for. Default 30 seconds\n");
    printf("    -p, --platform      Platform we're running on. Supported options = ['AMLOGIC', 'AMLOGIC_950D4', 'REALTEK', 'REALTEK64', 'BROADCOM', 'MEDIATEK']. Defaults to Amlogic\n");
    printf("    -g, --groups        Path to JSON file containing the group mappings (optional)\n");
    printf("    -c, --cpuidle       Enable CPU Idle metrics (default to false, requires kernel support)\n");
}

static void parseArgs(const int argc, char **argv)
{
    struct option longopts[] = {
            {"help",       no_argument,       nullptr, (int) 'h'},
            {"duration",   required_argument, nullptr, (int) 'd'},
            {"platform",   required_argument, nullptr, (int) 'p'},
            {"output-dir", required_argument, nullptr, (int) 'o'},
            {"json",       no_argument,       nullptr, (int) 'j'},
            {"groups",     required_argument, nullptr, (int) 'g'},
            {"cpuidle",     no_argument, nullptr, (int) 'c'},
            {nullptr, 0,                      nullptr, 0}
    };

    opterr = 0;

    int option;
    int longindex;

    while ((option = getopt_long(argc, argv, "hd:p:o:jg:c", longopts, &longindex)) != -1) {
        switch (option) {
            case 'h':
                displayUsage();
                exit(EXIT_SUCCESS);
                break;
            case 'd':
                gDuration = std::atoi(optarg);
                if (gDuration < 0) {
                    fprintf(stderr, "Error: duration (s) must be > 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'p': {
                std::string platform(optarg);

                if (platform == "AMLOGIC") {
                    gPlatform = Platform::AMLOGIC;
                } else if (platform == "AMLOGIC_950D4") {
                    gPlatform = Platform::AMLOGIC_950D4;
                } else if (platform == "REALTEK") {
                    gPlatform = Platform::REALTEK;
                } else if (platform == "REALTEK64") {
                    gPlatform = Platform::REALTEK64;
                } else if (platform == "BROADCOM") {
                    gPlatform = Platform::BROADCOM;
                } else if (platform == "MEDIATEK") {
                    gPlatform = Platform::MEDIATEK;
                } else {
                    fprintf(stderr, "Warning: Unsupported platform %s\n", platform.c_str());
                    exit(EXIT_FAILURE);
                }
                break;
            }
            case 'o': {
                gOutputDirectory = std::filesystem::path(optarg);
                break;
            }
            case 'j': {
                gJson = true;
                break;
            }
            case 'g': {
                gEnableGroups = true;
                gGroupsFile = std::filesystem::path(optarg);
                break;
            }
            case 'c': {
                gCpuIdle = true;
                break;
            }
            case '?':
                if (optopt == 'c')
                    fprintf(stderr, "Warning: Option -%c requires an argument.\n", optopt);
                else if (isprint(optopt))
                    fprintf(stderr, "Warning: Unknown option `-%c'.\n", optopt);
                else
                    fprintf(stderr, "Warning: Unknown option character `\\x%x'.\n", optopt);

                exit(EXIT_FAILURE);
                break;
            default:
                exit(EXIT_FAILURE);
                break;
        }
    }
}

void signalHandler(int signal)
{
    // On SIGTERM, we should stop capturing and save the report
    LOG_INFO("Signal %d (%s) received. Stopping and saving report!", signal, strsignal(signal));
    gEarlyTermination = true;
    gStop.notify_all();
    LOG_INFO("Waiting for in-progress data collection to complete");
}


int main(int argc, char *argv[])
{
    parseArgs(argc, argv);

    // Get start time
    auto start = std::chrono::steady_clock::now();

    // Configure signals to stop and clean up
#ifdef USE_BREAKPAD
    // Breakpad will handle SIGILL, SIGABRT, SIGFPE and SIGSEGV
    LOG_INFO("Breakpad support enabled");
    breakpad_ExceptionHandler();
#endif

    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT, signalHandler);

    // Lower our priority to avoid getting in the way
    if (nice(10) < 0) {
        LOG_WARN("Failed to set nice value");
    }

    try {
        std::filesystem::create_directories(gOutputDirectory);
    } catch (std::filesystem::filesystem_error &e) {
        LOG_ERROR("Failed to create directory %s to save results in: '%s'", gOutputDirectory.string().c_str(),
                  e.what());
        return EXIT_FAILURE;
    }

    LOG_INFO("** About to start memory capture for %d seconds **", gDuration);
    LOG_INFO("Will save report to %s", gOutputDirectory.string().c_str());

    // Load groups JSON if provided
    std::optional<std::shared_ptr<GroupManager>> groupManager = std::nullopt;
    if (gEnableGroups) {
        LOG_INFO("Loading groups from %s", std::filesystem::absolute(gGroupsFile).string().c_str());
        std::ifstream groupsFile(gGroupsFile);
        if (!groupsFile) {
            LOG_ERROR("Invalid groups file %s", gGroupsFile.string().c_str());
            return EXIT_FAILURE;
        } else {
            try {
                auto groupsJson = nlohmann::json::parse(groupsFile);
                groupManager = std::make_shared<GroupManager>(groupsJson);
            } catch (nlohmann::json::exception &e) {
                LOG_ERROR("Failed to parse groups JSON with error %s", e.what());
                return EXIT_FAILURE;
            }
        }
    }

    auto metadata = std::make_shared<Metadata>();
    auto reportGenerator = std::make_shared<JsonReportGenerator>(metadata, groupManager);

    // Create all our metrics
    ProcessMetric processMetric(reportGenerator);
    MemoryMetric memoryMetric(gPlatform, reportGenerator);

#ifdef ENABLE_CPU_IDLE_METRICS
    CpuIdleMetric cpuIdleMetric(reportGenerator);
#endif

    // Start data collection
    processMetric.StartCollection(std::chrono::seconds(3));
    memoryMetric.StartCollection(std::chrono::seconds(3));

    if (gCpuIdle) {
#ifdef ENABLE_CPU_IDLE_METRICS
        // The frequency does not affect this metric
        cpuIdleMetric.StartCollection(std::chrono::seconds(0));
#else
        LOG_ERROR("Cannot retrieve CPU idle stats - not built with ENABLE_CPU_IDLE_METRICS set");
#endif
    }

    // Block main thread for the collection duration or until SIGTERM
    std::unique_lock<std::mutex> locker(gLock);
    gStop.wait_for(locker, std::chrono::seconds(gDuration));

    if (!gEarlyTermination) {
        LOG_INFO("Stopping after %d seconds - completed full capture", gDuration);
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start).count();
    metadata->SetDuration(duration);

    // Done! Stop data collection
    processMetric.StopCollection();
    memoryMetric.StopCollection();
#ifdef ENABLE_CPU_IDLE_METRICS
    if (gCpuIdle) {
        cpuIdleMetric.StopCollection();
    }
#endif

    // Save results
    processMetric.SaveResults();
    memoryMetric.SaveResults();
#ifdef ENABLE_CPU_IDLE_METRICS
    if (gCpuIdle) {
        cpuIdleMetric.SaveResults();
    }
#endif

    // Build report
    inja::Environment env;
    // Make the output a bit tidier
    env.set_trim_blocks(true);
    env.set_lstrip_blocks(true);

    // Convert the values in an object into an array that we can loop over (used for generating rows)
    // Order the values using the _columnOrder data given in the second argument
    env.add_callback("objectToArray", 2, [](inja::Arguments &args)
    {
        std::vector<nlohmann::json> values;

        assert(args.at(0)->is_object());
        assert(args.at(1)->is_array());

        std::map<std::string, nlohmann::json> flattenedData;

        // Flatten the data (one level deep only)
        for (const auto &element: args.at(0)->items()) {
            if (element.value().is_object()) {
                // This is a very janky way to handle min/max/average & column ordering. The key name corresponds to "<Measurement Name> (Min/Max/Average)"
                // which matches the _columnOrder values set in JsonReportGenerator
                for (const auto &child: element.value().items()) {
                    std::string keyName = element.key() + " (" + child.key() + ")";
                    flattenedData.emplace(keyName, child.value());
                }
            } else {
                values.emplace_back(element.value());
                flattenedData.emplace(element.key(), element.value());
            }
        }

        // Put the data into the order specified by _columnOrder
        std::vector<nlohmann::json> ordered;
        for (const auto &column: args.at(1)->items()) {
            auto item = flattenedData.at(column.value());
            ordered.emplace_back(item);
        }

        return ordered;
    });

    // Write the JSON first - this is safer and is the report automation need, so if we crash
    // after this point we'll still get some data
    if (gJson) {
        std::filesystem::path jsonFilepath = gOutputDirectory / "report.json";
        std::ofstream outputJson(jsonFilepath, std::ios::trunc | std::ios::binary);
        outputJson << reportGenerator->getJson().dump(4);

        LOG_INFO("Saved JSON data to %s", jsonFilepath.string().c_str());
    }

    try {
        auto htmlTemplateString = std::string(g_templateHtml_data, g_templateHtml_data + g_templateHtml_size);
        std::string result = env.render(htmlTemplateString, reportGenerator->getJson());

        std::filesystem::path htmlFilepath = gOutputDirectory / "report.html";
        std::ofstream outputHtml(htmlFilepath, std::ios::trunc | std::ios::binary);
        outputHtml << result;

        LOG_INFO("Saved report to %s", htmlFilepath.string().c_str());
    } catch (const std::exception &e) {
        LOG_ERROR("Failed to save HTML report with exception %s", e.what());
        throw;
    }

    return EXIT_SUCCESS;
}
