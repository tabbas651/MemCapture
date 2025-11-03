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

#include "MemoryMetric.h"
#include "FileParsers/MemInfo.h"
#include <thread>
#include <fstream>
#include <filesystem>
#include <unistd.h>
#include <cmath>
#include <regex>

MemoryMetric::MemoryMetric(Platform platform, std::shared_ptr<JsonReportGenerator> reportGenerator)
        : mQuit(false),
          mCv(),
          mLinuxMemoryMeasurements{},
          mCmaFree("Value_KB"),
          mCmaBorrowed("Value_KB"),
          mMemoryBandwidth("Memory_Bandwidth_kbps"),
          mMemoryBandwidthSupported(false),
          mMemoryFragmentation{},
          mPlatform(platform),
          mReportGenerator(std::move(reportGenerator))
{

    // Some metrics are returned as a number of pages instead of bytes, so get page size to be able to calculate
    // human-readable values
    mPageSize = sysconf(_SC_PAGESIZE);

    // Create a map of CMA regions that converts the directories in /sys/kernel/debug/cma/ to a human-readable name
    // based on the kernel DTS file
    // *** This will likely need updating for your particular device ***
    if (platform == Platform::AMLOGIC) {
        mCmaNames = {
                std::make_pair("cma-0", "secmon_reserved"),
                std::make_pair("cma-1", "logo_reserved"),
                std::make_pair("cma-2", "codec_mm_cma"),
                std::make_pair("cma-3", "ion_cma_reserved"),
                std::make_pair("cma-4", "vdin1_cma_reserved"),
                std::make_pair("cma-5", "demod_cma_reserved"),
                std::make_pair("cma-6", "kernel_reserved"),
        };
    } else if (platform == Platform::AMLOGIC_950D4) {
        mCmaNames = {
                std::make_pair("cma-linux,secmo", "secmon_reserved"),
                std::make_pair("cma-reserved", "reserved"),
                std::make_pair("cma-linux,codec", "codec_mm_cma"),
                std::make_pair("cma-linux,ion-d", "ion_cma_reserved"),
                std::make_pair("cma-linux,vdin1", "vdin1_cma_reserved"),
                std::make_pair("cma-linux,meson", "kernel_reserved")
        };
    } else if (platform == Platform::REALTEK) {
        mCmaNames = {
                std::make_pair("cma-0", "cma-0"),
                std::make_pair("cma-1", "cma-1"),
                std::make_pair("cma-2", "cma-2"),
                std::make_pair("cma-3", "cma-3"),
                std::make_pair("cma-4", "cma-4"),
                std::make_pair("cma-5", "cma-5"),
                std::make_pair("cma-6", "cma-6"),
                std::make_pair("cma-7", "cma-7"),
                std::make_pair("cma-8", "cma-8"),
        };
    } else if (platform == Platform::REALTEK64) {
        mCmaNames = {
                std::make_pair("cma-linux,defau", "default_dma_pool"),
                std::make_pair("cma-linux,cma_1", "video_output_pool_2"),
                std::make_pair("cma-linux,cma_3", "audio_pool"),
                std::make_pair("cma-linux,cma_4", "svp_video_pool"),
                std::make_pair("cma-linux,cma_5", "audio_output_pool"),
                std::make_pair("cma-linux,cma_6", "ota_pool"),
                std::make_pair("cma-linux,cma_7", "audio_fw_pool"),
                std::make_pair("cma-linux,cma_8", "audio_hifi_pool"),
                std::make_pair("cma-linux,cma_9", "video_output_pool_1"),
        };
    } else if (platform == Platform::BROADCOM) {
        mCmaNames = {
                std::make_pair("cma-WiFi@4C0000", "cma-WiFi@4C0000"),
                std::make_pair("cma-reserved", "cma-reserved")
        };
    }

    // Create static measurements for linux memory usage - store in KB
    const std::vector<std::string> usageCategories{"Total", "Used", "Buffered", "Cached", "Free", "Available",
                                                   "Slab Total", "Slab Reclaimable", "Slab Unreclaimable", "Swap Used"};

    for (const auto& category : usageCategories) {
        Measurement value("Value_KB");
        mLinuxMemoryMeasurements.insert(std::make_pair(category, value));
    }

    switch (platform) {
        case Platform::AMLOGIC: 
        case Platform::AMLOGIC_950D4: 
        {
            // Amlogic should allow reporting memory bandwidth
            if (std::filesystem::exists("/sys/class/aml_ddr/mode")) {
                mMemoryBandwidthSupported = true;
                std::ofstream ddrMode("/sys/class/aml_ddr/mode", std::ios::binary);
                ddrMode << "1";
            }

            // Amlogic reports GPU memory allocations
            mGPUMemorySupported = true;
            break;
        }

        case Platform::REALTEK:
        case Platform::REALTEK64:
            // Realtek does not report memory bandwidth
            mMemoryBandwidthSupported = false;
            // Realtek reports GPU memory allocations
            mGPUMemorySupported = true;
            break;

        case Platform::BROADCOM:
            // Line of enquiry open with Broadcom as to whether there is a way to get this info.
            // TODO: Complete investigation.
            mMemoryBandwidthSupported = false;
            mGPUMemorySupported = true;
            break;
    }

}

MemoryMetric::~MemoryMetric()
{
    if (!mQuit) {
        StopCollection();
    }

    // Disable memory bandwidth monitoring
    std::ofstream ddrMode("/sys/class/aml_ddr/mode", std::ios::binary);
    ddrMode << "0";
}

void MemoryMetric::StartCollection(const std::chrono::seconds frequency)
{
    mQuit = false;
    mCollectionThread = std::thread(&MemoryMetric::CollectData, this, frequency);
}

void MemoryMetric::StopCollection()
{
    std::unique_lock<std::mutex> locker(mLock);
    mQuit = true;
    mCv.notify_all();
    locker.unlock();

    if (mCollectionThread.joinable()) {
        LOG_INFO("Waiting for MemoryMetric collection thread to terminate");
        mCollectionThread.join();
    }
}

void MemoryMetric::CollectData(std::chrono::seconds frequency)
{
    std::unique_lock<std::mutex> lock(mLock);

    do {
        auto start = std::chrono::high_resolution_clock::now();

        GetLinuxMemoryUsage();
        GetCmaMemoryUsage();
        GetGpuMemoryUsage();
        GetContainerMemoryUsage();
        GetMemoryBandwidth();
        CalculateFragmentation();

        if (mPlatform == Platform::BROADCOM) {
            GetBroadcomBmemUsage();
        }

        auto end = std::chrono::high_resolution_clock::now();
        LOG_INFO("MemoryMetric completed in %lld ms",
                 (long long) std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

        // Wait for period before doing collection again, or until cancelled
        mCv.wait_for(lock, frequency);
    } while (!mQuit);

    LOG_INFO("Collection thread quit");
}

void MemoryMetric::SaveResults()
{
    std::vector<JsonReportGenerator::dataItems> data{};

    for (const auto &result: mLinuxMemoryMeasurements) {
        data.emplace_back(JsonReportGenerator::dataItems{
                std::make_pair("Value", result.first),
                result.second
        });
    }
    mReportGenerator->addDataset("Linux Memory", data);

    // Set the average Used memory value
    auto it = mLinuxMemoryMeasurements.find("Used");
    if (it != mLinuxMemoryMeasurements.end()) {
        mReportGenerator->setAverageLinuxMemoryUsage(it->second.GetAverageRounded());
    }
    data.clear();

    // *** GPU Memory Usage ***
    if (mGPUMemorySupported) {
        for (const auto &result: mGpuMeasurements) {
            data.emplace_back(JsonReportGenerator::dataItems{
                    std::make_pair("PID", std::to_string(result.first)),
                    std::make_pair("Process", result.second.ProcessInfo.name()),
                    std::make_pair("Container", result.second.ProcessInfo.container().has_value()
                                                ? result.second.ProcessInfo.container().value()
                                                : "-"),
                    std::make_pair("Cmdline", result.second.ProcessInfo.cmdline()),
                    result.second.Used
            });
        }
        mReportGenerator->addDataset("GPU Memory", data);

        // Add all GPU memory to accumulated total
        long double gpuSum = 0;
        std::for_each(mGpuMeasurements.begin(), mGpuMeasurements.end(), [&](const std::pair<pid_t, gpuMeasurement> &m)
        {
            gpuSum += m.second.Used.GetAverage();
        });
        mReportGenerator->addToAccumulatedMemoryUsage(gpuSum);

        data.clear();
    }

    // *** CMA Memory Usage and breakdown ***
    for (const auto &result: mCmaMeasurements) {
        data.emplace_back(JsonReportGenerator::dataItems{
                std::make_pair("Region", result.first),
                std::make_pair("Size_KB", std::to_string(result.second.sizeKb)),
                result.second.Used,
                result.second.Unused
        });
    }
    mReportGenerator->addDataset("CMA Regions", data);

    // Add all CMA memory to accumulated total
    long double cmaSum = 0;
    std::for_each(mCmaMeasurements.begin(), mCmaMeasurements.end(), [&](const std::pair<std::string, cmaMeasurement> &m)
    {
        cmaSum += m.second.Used.GetAverage();
    });
    mReportGenerator->addToAccumulatedMemoryUsage(cmaSum);

    data.clear();


    // *** CMA Summary ***
    data.emplace_back(JsonReportGenerator::dataItems{
            std::make_pair("Value", "CMA Free"),
            mCmaFree
    });
    data.emplace_back(JsonReportGenerator::dataItems{
            std::make_pair("Value", "CMA Borrowed by Kernel"),
            mCmaBorrowed
    });
    mReportGenerator->addDataset("CMA Summary", data);
    data.clear();

    // *** Per-container memory usage ***
    for (const auto &result: mContainerMeasurements) {
        data.emplace_back(JsonReportGenerator::dataItems{
                std::make_pair("Container", result.first),
                result.second
        });
    }
    mReportGenerator->addDataset("Containers", data);
    data.clear();

    // *** Memory bandwidth (if supported) ***
    if (mMemoryBandwidthSupported) {

        data.emplace_back(JsonReportGenerator::dataItems{
                mMemoryBandwidth
        });
        mReportGenerator->addDataset("Memory Bandwidth", data);
        data.clear();
    }

    // *** Memory fragmentation - break down per zone ***
    for (const auto &memoryZone: mMemoryFragmentation) {
        std::string reportName = "Memory Fragmentation - Zone " + memoryZone.first;

        int i = 0;
        for (const auto &measurement: memoryZone.second) {
            data.emplace_back(JsonReportGenerator::dataItems{
                    std::make_pair("Order", std::to_string(i)),
                    measurement.FreePages,
                    measurement.Fragmentation
            });
            i++;
        }
        mReportGenerator->addDataset(reportName, data);
        data.clear();
    }

    // *** Broadcom BMEM (if applicable) ***
    if (mPlatform == Platform::BROADCOM) {
        for (const auto &measurement: mBroadcomBmemMeasurements) {
            data.emplace_back(JsonReportGenerator::dataItems{
                    std::make_pair("Region", measurement.first),
                    measurement.second});
        }
        mReportGenerator->addDataset("BMEM", data);

        // Add all BMEM memory to accumulated total
        long double bmemSum = 0;
        std::for_each(mBroadcomBmemMeasurements.begin(), mBroadcomBmemMeasurements.end(), [&](const auto &m)
        {
            bmemSum += m.second.GetAverage();
        });
        mReportGenerator->addToAccumulatedMemoryUsage(bmemSum);
    }
}

void MemoryMetric::GetLinuxMemoryUsage()
{
    //LOG_INFO("Getting memory usage");

    MemInfo memInfoFile;
    mLinuxMemoryMeasurements.at("Total").AddDataPoint(memInfoFile.MemTotalKb());
    mLinuxMemoryMeasurements.at("Used").AddDataPoint(memInfoFile.MemUsedKb());
    mLinuxMemoryMeasurements.at("Buffered").AddDataPoint(memInfoFile.BuffersKb());
    mLinuxMemoryMeasurements.at("Cached").AddDataPoint(memInfoFile.CachedKb());
    mLinuxMemoryMeasurements.at("Free").AddDataPoint(memInfoFile.MemFreeKb());
    mLinuxMemoryMeasurements.at("Available").AddDataPoint(memInfoFile.MemAvailableKb());
    mLinuxMemoryMeasurements.at("Slab Total").AddDataPoint(memInfoFile.SlabKb());
    mLinuxMemoryMeasurements.at("Slab Reclaimable").AddDataPoint(memInfoFile.SlabReclaimable());
    mLinuxMemoryMeasurements.at("Slab Unreclaimable").AddDataPoint(memInfoFile.SlabUnreclaimable());
    mLinuxMemoryMeasurements.at("Swap Used").AddDataPoint(memInfoFile.SwapUsed());
}

void MemoryMetric::GetCmaMemoryUsage()
{
    //LOG_INFO("Getting CMA memory usage");

    long double countKb = 0;
    long double usedKb = 0;
    long double unusedKb = 0;

    long double cmaTotalKb = 0;
    long double cmaTotalUsed = 0;

    // Start by getting CMA breakdown
    try {
        for (const auto &dirEntry: std::filesystem::directory_iterator(
                "/sys/kernel/debug/cma")) {

            // Read CMA metrics
            // Total size of the CMA region
            auto countFile = std::ifstream(dirEntry.path() / "count");
            countFile >> countKb;
            countKb = (countKb * mPageSize) / (long double) 1024;

            // Amount of pages used
            auto usedPagesFile = std::ifstream(dirEntry.path() / "used");
            usedPagesFile >> usedKb;
            usedKb = (usedKb * mPageSize) / (long double) 1024;

            // Calculate how much of that region is unused
            unusedKb = countKb - usedKb;

            // Calculate some totals
            cmaTotalKb += countKb;
            cmaTotalUsed += usedKb;

            std::string cmaName;
            try {
                cmaName = mCmaNames.at(dirEntry.path().filename());
            }
            catch (const std::exception &ex) {
                LOG_WARN("Could not find CMA name for directory %s, path=%s, cmaName=%s", dirEntry.path().filename().string().c_str(), dirEntry.path().string().c_str(), cmaName.c_str());
                cmaName = dirEntry.path().filename();
            }

            // Add to measurements
            auto itr = mCmaMeasurements.find(cmaName);

            if (itr != mCmaMeasurements.end()) {
                // If we have previous measurements for this region, add new data points
                auto &measurement = itr->second;

                measurement.sizeKb = countKb;
                measurement.Used.AddDataPoint(usedKb);
                measurement.Unused.AddDataPoint(unusedKb);
            } else {
                // New CMA region, create measurements
                auto used = Measurement("Used_KB");
                used.AddDataPoint(usedKb);

                auto unused = Measurement("Unused_KB");
                unused.AddDataPoint(unusedKb);

                auto measurement = cmaMeasurement(countKb, used, unused);
                mCmaMeasurements.insert(std::make_pair(cmaName, measurement));
            }
        }

        // Work out how much CMA is borrowed by the kernel (this can occur under memory pressure scenarios where
        // there is not enough memory elsewhere for userspace processes)
        MemInfo memInfoFile;
        mCmaFree.AddDataPoint(memInfoFile.CmaFree());

        long double totalUnused = cmaTotalKb - cmaTotalUsed;
        long double borrowed = totalUnused - memInfoFile.CmaFree();
        mCmaBorrowed.AddDataPoint(borrowed);
    } catch (std::filesystem::filesystem_error &error) {
        LOG_WARN("Failed to open CMA debug file with error %s", error.what());
    }
}

void MemoryMetric::GetGpuMemoryUsage()
{
    if (mGPUMemorySupported) {
        //LOG_INFO("Getting GPU memory usage");

        switch (mPlatform) {
            case (Platform::AMLOGIC): 
            case (Platform::AMLOGIC_950D4): 
            {
                GetGpuMemoryUsageAmlogic();
                break;
            }
            case (Platform::REALTEK): 
            case (Platform::REALTEK64): 
	    {
                GetGpuMemoryUsageRealtek();
                break;
            }
            case (Platform::BROADCOM): {
                GetGpuMemoryUsageBroadcom();
                break;
            }
        }
    }
}

void MemoryMetric::GetContainerMemoryUsage()
{
    //LOG_INFO("Getting Container memory usage");

    long double memoryUsageKb = 0;

    // List of systemd-created cgroups which we are not interested in.
    static const std::regex ignoreRegex = std::regex("(init.scope)|(.*.slice)|(.*.mount)|(.*.scope)");

    std::string memoryCgroupDir = "/sys/fs/cgroup/memory";

    if (!std::filesystem::exists(memoryCgroupDir)) {
        return;
    }

    // Simplest way is to report memory usage by each cgroup, although this can result in some results that don't
    // correspond to a container if something else created that cgroup
    for (const auto &dirEntry: std::filesystem::directory_iterator(
            "/sys/fs/cgroup/memory")) {
        if (!dirEntry.is_directory()) {
            continue;
        }

        auto containerName = dirEntry.path().filename().string();
        if (!std::regex_match(containerName, ignoreRegex)) {
            auto memoryUsageFile = std::ifstream(dirEntry.path() / "memory.usage_in_bytes");
            memoryUsageFile >> memoryUsageKb;
            memoryUsageKb /= (long double) 1024.0;

            auto itr = mContainerMeasurements.find(containerName);

            if (itr != mContainerMeasurements.end()) {
                auto &measurement = itr->second;
                measurement.AddDataPoint(memoryUsageKb);
            } else {
                Measurement measurement("Memory_Used_KB");
                measurement.AddDataPoint(memoryUsageKb);
                mContainerMeasurements.insert(std::make_pair(containerName, measurement));
            }
        }
    }
}

void MemoryMetric::GetMemoryBandwidth()
{
    // Only supported on Amlogic
    if (mMemoryBandwidthSupported) {
        //LOG_INFO("Getting memory bandwidth usage");

        if (mPlatform == Platform::AMLOGIC || mPlatform == Platform::AMLOGIC_950D4) {
            std::ifstream memBandwidthFile("/sys/class/aml_ddr/bandwidth");

            if (!memBandwidthFile) {
                LOG_WARN("Cannot get DDR usage");
                return;
            }

            std::string line;
            int kbps = 0;
            double percent = 0;

            while (std::getline(memBandwidthFile, line)) {
                if (sscanf(line.c_str(), "Total bandwidth: %8d KB/s, usage:  %lf%%", &kbps, &percent) != 0) {
                    if (kbps != 0) {
                        mMemoryBandwidth.AddDataPoint(kbps);
                    }
                }
            }
        }
    }
}

void MemoryMetric::GetBroadcomBmemUsage()
{
    // LOG_INFO("Getting BMEM Usage");

    std::ifstream broadcomCoreInfo("/proc/brcm/core");

    if (!broadcomCoreInfo) {
        LOG_WARN("Could not open /proc/brcm/core");
        return;
    }

    std::string line;

    char regionName[128];
    int regionSize;
    int regionUsage;
    while (std::getline(broadcomCoreInfo, line)) {
        if (sscanf(line.c_str(), "%*d  %*s %*d %*s   %d %*s %d%% %*d%% %s", &regionSize, &regionUsage,
                   regionName) == 3) {
            // Calculate how many MB we're using since Bcom in their infinite wisdom only give us a percentage
            // Use KB for consistency with everything else
            double usageKb = (regionSize * (regionUsage / 100.0)) * 1024;

            auto itr = mBroadcomBmemMeasurements.find(std::string(regionName));

            if (itr == mBroadcomBmemMeasurements.end()) {
                // New region
                Measurement measurement("Memory_Usage_KB");
                measurement.AddDataPoint(usageKb);
                mBroadcomBmemMeasurements.insert(std::make_pair(std::string(regionName), measurement));
            } else {
                auto &measurement = itr->second;
                measurement.AddDataPoint(usageKb);
            }
        }
    }
}


void MemoryMetric::CalculateFragmentation()
{
    //LOG_INFO("Getting memory fragmentation");

    std::ifstream buddyInfo("/proc/buddyinfo");

    if (!buddyInfo) {
        LOG_WARN("Could not open buddyinfo");
        return;
    }

    std::string line;
    std::string segment;
    // Get fragmentation for all zones
    while (std::getline(buddyInfo, line)) {
        std::stringstream lineStream(line);
        std::vector<std::string> segments;
        // Split line on space
        while (std::getline(lineStream, segment, ' ')) {
            if (!segment.empty()) {
                segments.emplace_back(segment);
            }
        }

        std::string zoneName = segments[3];
        std::map<int, int> freePages;
        std::map<int, double> fragmentationPercent;

        size_t columnCount = 0;
        if (mPlatform == Platform::AMLOGIC || mPlatform == Platform::AMLOGIC_950D4) {
            columnCount = 15;
        } else if (mPlatform == Platform::REALTEK) {
            columnCount = 17;
        } else if (mPlatform == Platform::REALTEK64) {
		/* ES1 has 15 columns */
            columnCount = 15;
        } else if (mPlatform == Platform::BROADCOM) {
            columnCount = 15;
        }

        if (segments.size() != columnCount) {
            LOG_WARN("Failed to parse buddyinfo - invalid number of columns (got %zd, expected %zd)", segments.size(),
                     columnCount);
        } else {
            // Calculate fragmentation % for this node
            int totalFreePages = 0;

            //  Get all free page values, and work out total free pages
            for (int i = 4; i < (int) columnCount; i++) {
                int order = i - 4;

                int freeCount = std::stoi(segments[i]);
                totalFreePages += std::pow(2, order) * freeCount;
                freePages[order] = freeCount;
            }

            // Now find out the fragmentation percentages (see https://github.com/dsanders11/scripts/blob/master/Linux_Memory_Fragmentation.pdf and
            // http://thomas.enix.org/pub/rmll2005/rmll2005-gorman.pdf)
            double fragPercentage;
            for (int i = 0; i < (int) freePages.size(); i++) {
                fragPercentage = 0;

                // Seems inefficient...
                for (int j = i; j < (int) freePages.size(); j++) {
                    fragPercentage += (std::pow(2, j)) * freePages[j];
                }
                fragPercentage = (totalFreePages - fragPercentage) / totalFreePages;
                fragmentationPercent[i] = fragPercentage;
            }

            // Update measurements
            auto itr = mMemoryFragmentation.find(zoneName);
            if (itr != mMemoryFragmentation.end()) {
                auto &measurements = itr->second;

                for (int i = 0; i < (int) freePages.size(); i++) {
                    measurements[i].FreePages.AddDataPoint(freePages[i]);
                    measurements[i].Fragmentation.AddDataPoint(fragmentationPercent[i] * 100);
                }
            } else {
                std::vector<memoryFragmentation> measurements = {};
                for (int i = 0; i < (int) freePages.size(); i++) {
                    Measurement fp("Free_Pages");
                    fp.AddDataPoint(freePages[i]);

                    Measurement frag("Fragmentation_%");
                    frag.AddDataPoint(fragmentationPercent[i]);
                    memoryFragmentation fragMeasurement(fp, frag);
                    measurements.emplace_back(fragMeasurement);
                }

                mMemoryFragmentation.insert(std::make_pair(zoneName, measurements));
            }
        }
    }
}

/**
 * Broadcom GPU memory allocations.
 * Available from a series of directories under /sys/kernel/debug/dri/0/.
 * Each directory has a 'client' file which needs to be parsed.
 *
 * Example paths:
 *
 * root@xione-sercomm:~# find /sys/kernel/debug/dri/0/ -name client
 * /sys/kernel/debug/dri/0/13449-00000000f601794d/client
 * /sys/kernel/debug/dri/0/13030-00000000cf255c5d/client
 * /sys/kernel/debug/dri/0/12326-00000000426cbc26/client
 * /sys/kernel/debug/dri/0/12298-00000000954ee8cf/client
 * /sys/kernel/debug/dri/0/8804-000000004fe3dec5/client
 * /sys/kernel/debug/dri/0/8632-0000000055df6881/client
 * /sys/kernel/debug/dri/0/7566-000000003bfb5b6e/client
 * root@xione-sercomm:~#
 *
 * Each directory under /sys/kernel/debug/dri/0/ is of the form '<tid>-<64bit hex>'.
 *
 * tid is the thread id of the thread that allocated the gpu mem, the allocation being detailed in the 'client' file under that directory.
 * Not sure what the 64 bit hex is. An address?
 *
 * Example content of a 'client' file:
 *
 * root@xione-sercomm:~# cat /sys/kernel/debug/dri/0/13449-00000000f601794d/client
 *             command objects    Virtual  SHM pages Huge Pages
 *     SkyBrowserLaunc       2     4096KB        0KB        4MB
 * root@xione-sercomm:~#
 *
 * Need to correlate this TID to the main PID of the process to make analysis easier
 *
 * Note that the process name does not include full path so this is instead retrieved from Procrank using the pid extracted from the directory name.
*/
void MemoryMetric::GetGpuMemoryUsageBroadcom()
{
    std::string line;
    pid_t tid;

    for (const auto &entry: std::filesystem::directory_iterator("/sys/kernel/debug/dri/0/")) {
        const auto entryStr = entry.path().filename().string();
        if (entry.is_directory()) {
            // Scan as far as we need to.
            if (sscanf(entryStr.c_str(), "%d-", &tid) != 1) {
                // Not interested in this directory.
                continue;
            }

            std::string pathStr = std::string("/sys/kernel/debug/dri/0/") + entryStr + "/client";
            std::ifstream gpuMem(pathStr.c_str());
            if (!gpuMem) {
                LOG_WARN("Could not open gpu_memory file %s", pathStr.c_str());
                continue;
            }

            while (std::getline(gpuMem, line)) {
                char processName[32];
                unsigned int objectsNum;
                unsigned long virtualMemNum;
                char virtualMemNumUnit[3];
                unsigned long virtualMemNumBytes;

                // Scan as far as we need to.
                if (sscanf(line.c_str(), " %s %d %ld%2c", processName, &objectsNum, &virtualMemNum,
                           virtualMemNumUnit) == 4) {

                    virtualMemNumUnit[2] = 0;

                    std::string virtualMemNumUnitStr(virtualMemNumUnit);

                    if (virtualMemNumUnitStr == "KB") {
                        virtualMemNumBytes = virtualMemNum * 1024;
                    } else if (virtualMemNumUnitStr == "MB") {
                        virtualMemNumBytes = virtualMemNum * 1024 * 1024;
                    } else if (virtualMemNumUnitStr == "GB") {
                        virtualMemNumBytes = virtualMemNum * 1024 * 1024 * 1024;
                    } else {
                        LOG_WARN("Could not parse this line: \'%s\'", line.c_str());
                        continue;
                    }

                    // Convert TID to parent PID (TGID) to make things easier to correlate later on
                    pid_t pid = tidToParentPid(tid);

                    auto itr = mGpuMeasurements.find(pid);

                    if (itr != mGpuMeasurements.end()) {
                        // Already got a measurement for this PID
                        auto &measurement = itr->second;
                        measurement.Used.AddDataPoint(virtualMemNumBytes / (long double) 1024.0);
                    } else {
                        Process process(pid);
                        Measurement used("Memory_Usage_KB");
                        used.AddDataPoint(virtualMemNumBytes / (long double) 1024.0);

                        auto measurement = gpuMeasurement(process, used);
                        mGpuMeasurements.insert(std::make_pair(pid, measurement));
                    }
                }
            }
        }
    }
}

/* Amlogic GPU memory allocations
 *
 * Sizes are in pages, so convert to bytes
 *
 * root@sky-llama-panel:~# cat /sys/kernel/debug/mali0/gpu_memory
    mali0            total used_pages      25939
    ----------------------------------------------------
    kctx             pid              used_pages
    ----------------------------------------------------
    f1dbf000      14880       4558
    f1c19000      14438        135
    f1bb1000      14292      16359
    f18c0000      10899       4887
*/
void MemoryMetric::GetGpuMemoryUsageAmlogic()
{
    std::ifstream gpuMem("/sys/kernel/debug/mali0/gpu_memory");

    if (!gpuMem) {
        LOG_WARN("Could not open gpu_memory file");
        return;
    }

    std::string line;
    long gpuPages;
    pid_t pid;

    while (std::getline(gpuMem, line)) {
        if (sscanf(line.c_str(), "%*x %d %ld", &pid, &gpuPages) != 0) {
            unsigned long gpuBytes = gpuPages * mPageSize;

            auto itr = mGpuMeasurements.find(pid);

            if (itr != mGpuMeasurements.end()) {
                // Already got a measurement for this PID
                auto &measurement = itr->second;
                measurement.Used.AddDataPoint(gpuBytes / (long double) 1024.0);
            } else {
                Process process(pid);

                Measurement used("Memory_Usage_KB");
                used.AddDataPoint(gpuBytes / (long double) 1024.0);

                auto measurement = gpuMeasurement(process, used);
                mGpuMeasurements.insert(std::make_pair(pid, measurement));
            }
        }
    }
}


/* Realtek GPU memory allocations
 *
 * Uses a similar format to Amlogic but rendered slightly differently
 *
 * Sizes are in pages, so convert to bytes
 * root@skyxione:/sys/kernel/debug/mali0# cat gpu_memory
 *
 * mali0                  45605
 * kctx-0xfa847000      14102      15898
 * kctx-0xf7953000         42      15833
 * kctx-0xff0b0000       3316       9134
 * kctx-0xfec18000      20929       8344
 * kctx-0xfb9df000        135       6235
 * kctx-0xfb12e000       7081       4962
*/
void MemoryMetric::GetGpuMemoryUsageRealtek()
{
    std::ifstream gpuMem("/sys/kernel/debug/mali0/gpu_memory");

    if (!gpuMem) {
        LOG_WARN("Could not open gpu_memory file");
        return;
    }

    std::string line;
    long gpuPages;
    pid_t pid;

    while (std::getline(gpuMem, line)) {
        if (sscanf(line.c_str(), "  kctx-0x%*x %ld %d", &gpuPages, &pid) != 0) {
            unsigned long gpuBytes = gpuPages * mPageSize;

            auto itr = mGpuMeasurements.find(pid);

            if (itr != mGpuMeasurements.end()) {
                // Already got a measurement for this PID
                auto &measurement = itr->second;
                measurement.Used.AddDataPoint(gpuBytes / (long double) 1024.0);
            } else {
                Process process(pid);

                Measurement used("Memory Usage KB");
                used.AddDataPoint(gpuBytes / (long double) 1024.0);

                auto measurement = gpuMeasurement(process, used);
                mGpuMeasurements.insert(std::make_pair(pid, measurement));
            }
        }
    }
}

/**
 * Given a thread ID, return the main PID (TGID) the thread belongs to
 * @return PID
 */
pid_t MemoryMetric::tidToParentPid(pid_t tid)
{
    std::string statusFilePath = "/proc/" + std::to_string(tid) + "/status";

    std::ifstream statusFile(statusFilePath);

    if (!statusFile) {
        LOG_WARN("Failed to open file %s", statusFilePath.c_str());
        return -1;
    }

    std::string line;
    pid_t pid;

    while (std::getline(statusFile, line)) {
        if (sscanf(line.c_str(), "Tgid:\t%d", &pid) == 1) {
            return pid;
        }
    }

    // Failed to find Tgid in file, weird?
    return -1;
}
