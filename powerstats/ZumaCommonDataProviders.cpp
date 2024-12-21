/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <PowerStatsAidl.h>
#include <ZumaCommonDataProviders.h>
#include <AocStateResidencyDataProvider.h>
#include <CpupmStateResidencyDataProvider.h>
#include <DevfreqStateResidencyDataProvider.h>
#include <DisplayMrrStateResidencyDataProvider.h>
#include <AdaptiveDvfsStateResidencyDataProvider.h>
#include <TpuDvfsStateResidencyDataProvider.h>
#include <UfsStateResidencyDataProvider.h>
#include <dataproviders/GenericStateResidencyDataProvider.h>
#include <dataproviders/IioEnergyMeterDataProvider.h>
#include <dataproviders/PowerStatsEnergyConsumer.h>
#include <dataproviders/PowerStatsEnergyAttribution.h>
#include <dataproviders/PixelStateResidencyDataProvider.h>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>
#include <sys/stat.h>

using aidl::android::hardware::power::stats::AdaptiveDvfsStateResidencyDataProvider;
using aidl::android::hardware::power::stats::AocStateResidencyDataProvider;
using aidl::android::hardware::power::stats::CpupmStateResidencyDataProvider;
using aidl::android::hardware::power::stats::DevfreqStateResidencyDataProvider;
using aidl::android::hardware::power::stats::DisplayMrrStateResidencyDataProvider;
using aidl::android::hardware::power::stats::DvfsStateResidencyDataProvider;
using aidl::android::hardware::power::stats::UfsStateResidencyDataProvider;
using aidl::android::hardware::power::stats::EnergyConsumerType;
using aidl::android::hardware::power::stats::GenericStateResidencyDataProvider;
using aidl::android::hardware::power::stats::IioEnergyMeterDataProvider;
using aidl::android::hardware::power::stats::PixelStateResidencyDataProvider;
using aidl::android::hardware::power::stats::PowerStatsEnergyConsumer;
using aidl::android::hardware::power::stats::TpuDvfsStateResidencyDataProvider;

// TODO (b/181070764) (b/182941084):
// Remove this when Wifi/BT energy consumption models are available or revert before ship
using aidl::android::hardware::power::stats::EnergyConsumerResult;
using aidl::android::hardware::power::stats::Channel;
using aidl::android::hardware::power::stats::EnergyMeasurement;
class PlaceholderEnergyConsumer : public PowerStats::IEnergyConsumer {
  public:
    PlaceholderEnergyConsumer(std::shared_ptr<PowerStats> p, EnergyConsumerType type,
            std::string name) : kType(type), kName(name), mPowerStats(p), mChannelId(-1) {
        std::vector<Channel> channels;
        mPowerStats->getEnergyMeterInfo(&channels);

        for (const auto &c : channels) {
            if (c.name == "VSYS_PWR_WLAN_BT") {
                mChannelId = c.id;
                break;
            }
        }
    }
    std::pair<EnergyConsumerType, std::string> getInfo() override { return {kType, kName}; }

    std::optional<EnergyConsumerResult> getEnergyConsumed() override {
        int64_t totalEnergyUWs = 0;
        int64_t timestampMs = 0;
        if (mChannelId != -1) {
            std::vector<EnergyMeasurement> measurements;
            if (mPowerStats->readEnergyMeter({mChannelId}, &measurements).isOk()) {
                for (const auto &m : measurements) {
                    totalEnergyUWs += m.energyUWs;
                    timestampMs = m.timestampMs;
                }
            } else {
                LOG(ERROR) << "Failed to read energy meter";
                return {};
            }
        }

        return EnergyConsumerResult{.timestampMs = timestampMs,
                                .energyUWs = totalEnergyUWs>>1};
    }

    std::string getConsumerName() override {
        return kName;
    };

  private:
    const EnergyConsumerType kType;
    const std::string kName;
    std::shared_ptr<PowerStats> mPowerStats;
    int32_t mChannelId;
};

void addPlaceholderEnergyConsumers(std::shared_ptr<PowerStats> p) {
    p->addEnergyConsumer(
            std::make_unique<PlaceholderEnergyConsumer>(p, EnergyConsumerType::WIFI, "Wifi"));
    p->addEnergyConsumer(
            std::make_unique<PlaceholderEnergyConsumer>(p, EnergyConsumerType::BLUETOOTH, "BT"));
}

void addAoC(std::shared_ptr<PowerStats> p) {
    // AoC clock is synced from "libaoc.c"
    static const uint64_t AOC_CLOCK = 24576;
    std::string base = "/sys/devices/platform/17000000.aoc/";
    std::string prefix = base + "control/";

    // Add AoC cores (a32, ff1, hf0, and hf1)
    std::vector<std::pair<std::string, std::string>> coreIds = {
            {"AoC-A32", prefix + "a32_"},
            {"AoC-FF1", prefix + "ff1_"},
            {"AoC-HF1", prefix + "hf1_"},
            {"AoC-HF0", prefix + "hf0_"},
    };
    std::vector<std::pair<std::string, std::string>> coreStates = {
            {"DWN", "off"}, {"RET", "retention"}, {"WFI", "wfi"}};
    p->addStateResidencyDataProvider(std::make_unique<AocStateResidencyDataProvider>(coreIds,
            coreStates, AOC_CLOCK));

    // Add AoC voltage stats
    std::vector<std::pair<std::string, std::string>> voltageIds = {
            {"AoC-Voltage", prefix + "voltage_"},
    };
    std::vector<std::pair<std::string, std::string>> voltageStates = {{"NOM", "nominal"},
                                                                      {"SUD", "super_underdrive"},
                                                                      {"UUD", "ultra_underdrive"},
                                                                      {"UD", "underdrive"}};
    p->addStateResidencyDataProvider(
            std::make_unique<AocStateResidencyDataProvider>(voltageIds, voltageStates, AOC_CLOCK));

    // Add AoC monitor mode
    std::vector<std::pair<std::string, std::string>> monitorIds = {
            {"AoC", prefix + "monitor_"},
    };
    std::vector<std::pair<std::string, std::string>> monitorStates = {
            {"MON", "mode"},
    };
    p->addStateResidencyDataProvider(
            std::make_unique<AocStateResidencyDataProvider>(monitorIds, monitorStates, AOC_CLOCK));

    // Add AoC restart count
    const GenericStateResidencyDataProvider::StateResidencyConfig restartCountConfig = {
            .entryCountSupported = true,
            .entryCountPrefix = "",
            .totalTimeSupported = false,
            .lastEntrySupported = false,
    };
    const std::vector<std::pair<std::string, std::string>> restartCountHeaders = {
            std::make_pair("RESTART", ""),
    };
    std::vector<GenericStateResidencyDataProvider::PowerEntityConfig> cfgs;
    cfgs.emplace_back(
            generateGenericStateResidencyConfigs(restartCountConfig, restartCountHeaders),
            "AoC-Count", "");
    p->addStateResidencyDataProvider(std::make_unique<GenericStateResidencyDataProvider>(
            base + "restart_count", cfgs));
}

void addDvfsStats(std::shared_ptr<PowerStats> p) {
    // A constant to represent the number of nanoseconds in one millisecond
    const int NS_TO_MS = 1000000;
    std::string path = "/sys/devices/platform/acpm_stats/fvp_stats";

    std::vector<std::pair<std::string, std::string>> adpCfgs = {
        std::make_pair("CL0", "/sys/devices/system/cpu/cpufreq/policy0/stats"),
        std::make_pair("CL1", "/sys/devices/system/cpu/cpufreq/policy4/stats"),
        std::make_pair("CL2", "/sys/devices/system/cpu/cpufreq/policy8/stats"),
        std::make_pair("MIF",
                "/sys/devices/platform/17000010.devfreq_mif/devfreq/17000010.devfreq_mif")};

    p->addStateResidencyDataProvider(std::make_unique<AdaptiveDvfsStateResidencyDataProvider>(
            path, NS_TO_MS, adpCfgs));

    std::vector<DvfsStateResidencyDataProvider::Config> cfgs;
    cfgs.push_back({"AUR", {
        std::make_pair("1065MHz", "1065000"),
        std::make_pair("861MHz", "861000"),
        std::make_pair("713MHz", "713000"),
        std::make_pair("525MHz", "525000"),
        std::make_pair("355MHz", "355000"),
        std::make_pair("256MHz", "256000"),
        std::make_pair("178MHz", "178000"),
    }});

    p->addStateResidencyDataProvider(std::make_unique<DvfsStateResidencyDataProvider>(
            path, NS_TO_MS, cfgs));

    // TPU DVFS
    const int TICK_TO_MS = 100;
    std::vector<std::string> freqs = {
            "1119000",
            "1066000",
            "845000",
            "712000",
            "627000",
            "455000",
            "226000"
    };
    p->addStateResidencyDataProvider(std::make_unique<TpuDvfsStateResidencyDataProvider>(
            "/sys/devices/platform/1a000000.rio/tpu_usage", freqs, TICK_TO_MS));
}

void addSoC(std::shared_ptr<PowerStats> p) {
    // A constant to represent the number of nanoseconds in one millisecond.
    const int NS_TO_MS = 1000000;

    // ACPM stats are reported in nanoseconds. The transform function
    // converts nanoseconds to milliseconds.
    std::function<uint64_t(uint64_t)> acpmNsToMs = [](uint64_t a) { return a / NS_TO_MS; };
    const GenericStateResidencyDataProvider::StateResidencyConfig lpmStateConfig = {
            .entryCountSupported = true,
            .entryCountPrefix = "success_count:",
            .totalTimeSupported = true,
            .totalTimePrefix = "total_time_ns:",
            .totalTimeTransform = acpmNsToMs,
            .lastEntrySupported = true,
            .lastEntryPrefix = "last_entry_time_ns:",
            .lastEntryTransform = acpmNsToMs,
    };
    const GenericStateResidencyDataProvider::StateResidencyConfig downStateConfig = {
            .entryCountSupported = true,
            .entryCountPrefix = "down_count:",
            .totalTimeSupported = true,
            .totalTimePrefix = "total_down_time_ns:",
            .totalTimeTransform = acpmNsToMs,
            .lastEntrySupported = true,
            .lastEntryPrefix = "last_down_time_ns:",
            .lastEntryTransform = acpmNsToMs,
    };
    const GenericStateResidencyDataProvider::StateResidencyConfig reqStateConfig = {
            .entryCountSupported = true,
            .entryCountPrefix = "req_up_count:",
            .totalTimeSupported = true,
            .totalTimePrefix = "total_req_up_time_ns:",
            .totalTimeTransform = acpmNsToMs,
            .lastEntrySupported = true,
            .lastEntryPrefix = "last_req_up_time_ns:",
            .lastEntryTransform = acpmNsToMs,

    };
    const std::vector<std::pair<std::string, std::string>> powerStateHeaders = {
            std::make_pair("SICD", "SICD"),
            std::make_pair("SLEEP", "SLEEP"),
            std::make_pair("SLEEP_SLCMON", "SLEEP_SLCMON"),
            std::make_pair("SLEEP_HSI1ON", "SLEEP_HSI1ON"),
            std::make_pair("STOP", "STOP"),
    };
    const std::vector<std::pair<std::string, std::string>> mifReqStateHeaders = {
            std::make_pair("AOC", "AOC"),
            std::make_pair("GSA", "GSA"),
            std::make_pair("TPU", "TPU"),
            std::make_pair("AUR", "AUR"),
    };
    const std::vector<std::pair<std::string, std::string>> slcReqStateHeaders = {
            std::make_pair("AOC", "AOC"),
    };

    std::vector<GenericStateResidencyDataProvider::PowerEntityConfig> cfgs;
    cfgs.emplace_back(generateGenericStateResidencyConfigs(lpmStateConfig, powerStateHeaders),
            "LPM", "LPM:");
    cfgs.emplace_back(generateGenericStateResidencyConfigs(downStateConfig, powerStateHeaders),
            "MIF", "MIF:");
    cfgs.emplace_back(generateGenericStateResidencyConfigs(reqStateConfig, mifReqStateHeaders),
            "MIF-REQ", "MIF_REQ:");
    cfgs.emplace_back(generateGenericStateResidencyConfigs(downStateConfig, powerStateHeaders),
            "SLC", "SLC:");
    cfgs.emplace_back(generateGenericStateResidencyConfigs(reqStateConfig, slcReqStateHeaders),
            "SLC-REQ", "SLC_REQ:");

    p->addStateResidencyDataProvider(std::make_unique<GenericStateResidencyDataProvider>(
            "/sys/devices/platform/acpm_stats/soc_stats", cfgs));
}

void setEnergyMeter(std::shared_ptr<PowerStats> p) {
    std::vector<std::string> deviceNames { "s2mpg14-odpm", "s2mpg15-odpm" };
    p->setEnergyMeterDataProvider(std::make_unique<IioEnergyMeterDataProvider>(deviceNames, true));
}

void addCPUclusters(std::shared_ptr<PowerStats> p) {
    // A constant to represent the number of nanoseconds in one millisecond.
    const int NS_TO_MS = 1000000;

    std::function<uint64_t(uint64_t)> acpmNsToMs = [](uint64_t a) { return a / NS_TO_MS; };
    const GenericStateResidencyDataProvider::StateResidencyConfig cpuStateConfig = {
            .entryCountSupported = true,
            .entryCountPrefix = "down_count:",
            .totalTimeSupported = true,
            .totalTimePrefix = "total_down_time_ns:",
            .totalTimeTransform = acpmNsToMs,
            .lastEntrySupported = true,
            .lastEntryPrefix = "last_down_time_ns:",
            .lastEntryTransform = acpmNsToMs,
    };

    const std::vector<std::pair<std::string, std::string>> cpuStateHeaders = {
            std::make_pair("DOWN", ""),
    };

    std::vector<GenericStateResidencyDataProvider::PowerEntityConfig> cfgs;
    for (std::string name : {
            "CLUSTER0",
            "CLUSTER1",
            "CLUSTER2"}) {
        cfgs.emplace_back(generateGenericStateResidencyConfigs(cpuStateConfig, cpuStateHeaders),
            name, name);
    }

    p->addStateResidencyDataProvider(std::make_unique<GenericStateResidencyDataProvider>(
            "/sys/devices/platform/acpm_stats/core_stats", cfgs));

    CpupmStateResidencyDataProvider::Config config = {
        .entities = {
            std::make_pair("CPU0", "cpu0"),
            std::make_pair("CPU1", "cpu1"),
            std::make_pair("CPU2", "cpu2"),
            std::make_pair("CPU3", "cpu3"),
            std::make_pair("CPU4", "cpu4"),
            std::make_pair("CPU5", "cpu5"),
            std::make_pair("CPU6", "cpu6"),
            std::make_pair("CPU7", "cpu7"),
            std::make_pair("CPU8", "cpu8")},
        .states = {
            std::make_pair("DOWN", "[state1]")}};

    CpupmStateResidencyDataProvider::SleepConfig sleepConfig = {"LPM:", "SLEEP", "total_time_ns:"};

    p->addStateResidencyDataProvider(std::make_unique<CpupmStateResidencyDataProvider>(
            "/sys/devices/system/cpu/cpupm/cpupm/time_in_state", config,
            "/sys/devices/platform/acpm_stats/soc_stats", sleepConfig));

    p->addEnergyConsumer(PowerStatsEnergyConsumer::createMeterConsumer(p,
            EnergyConsumerType::CPU_CLUSTER, "CPUCL0", {"S4M_VDD_CPUCL0"}));
    p->addEnergyConsumer(PowerStatsEnergyConsumer::createMeterConsumer(p,
            EnergyConsumerType::CPU_CLUSTER, "CPUCL1", {"S3M_VDD_CPUCL1"}));
    p->addEnergyConsumer(PowerStatsEnergyConsumer::createMeterConsumer(p,
            EnergyConsumerType::CPU_CLUSTER, "CPUCL2", {"S2M_VDD_CPUCL2"}));
}

void addGPU(std::shared_ptr<PowerStats> p) {
    // Add gpu energy consumer
    std::map<std::string, int32_t> stateCoeffs;
    std::string path = "/sys/devices/platform/1f000000.mali";

    stateCoeffs = {
        {"150000",  637},
        {"302000", 1308},
        {"337000", 1461},
        {"376000", 1650},
        {"419000", 1861},
        {"467000", 2086},
        {"521000", 2334},
        {"580000", 2558},
        {"649000", 2886},
        {"723000", 3244},
        {"807000", 3762},
        {"890000", 4333}};

    p->addEnergyConsumer(PowerStatsEnergyConsumer::createMeterAndAttrConsumer(p,
            EnergyConsumerType::OTHER, "GPU", {"S2S_VDD_G3D", "S8S_VDD_G3D_L2"},
            {{UID_TIME_IN_STATE, path + "/uid_time_in_state"}},
            stateCoeffs));

    p->addStateResidencyDataProvider(std::make_unique<DevfreqStateResidencyDataProvider>("GPU",
            path));
}

void addMobileRadio(std::shared_ptr<PowerStats> p)
{
    // A constant to represent the number of microseconds in one millisecond.
    const int US_TO_MS = 1000;

    // modem power_stats are reported in microseconds. The transform function
    // converts microseconds to milliseconds.
    std::function<uint64_t(uint64_t)> modemUsToMs = [](uint64_t a) { return a / US_TO_MS; };
    const GenericStateResidencyDataProvider::StateResidencyConfig powerStateConfig = {
            .entryCountSupported = true,
            .entryCountPrefix = "count:",
            .totalTimeSupported = true,
            .totalTimePrefix = "duration_usec:",
            .totalTimeTransform = modemUsToMs,
            .lastEntrySupported = true,
            .lastEntryPrefix = "last_entry_timestamp_usec:",
            .lastEntryTransform = modemUsToMs,
    };
    const std::vector<std::pair<std::string, std::string>> powerStateHeaders = {
            std::make_pair("SLEEP", "SLEEP:"),
    };

    std::vector<GenericStateResidencyDataProvider::PowerEntityConfig> cfgs;
    cfgs.emplace_back(generateGenericStateResidencyConfigs(powerStateConfig, powerStateHeaders),
            "MODEM", "");

    p->addStateResidencyDataProvider(std::make_unique<GenericStateResidencyDataProvider>(
            "/sys/devices/platform/cpif/modem/power_stats", cfgs));

    p->addEnergyConsumer(PowerStatsEnergyConsumer::createMeterConsumer(p,
            EnergyConsumerType::MOBILE_RADIO, "MODEM",
            {"VSYS_PWR_MODEM", "VSYS_PWR_RFFE", "VSYS_PWR_MMWAVE"}));
}

void addGNSS(std::shared_ptr<PowerStats> p)
{
    // A constant to represent the number of microseconds in one millisecond.
    const int US_TO_MS = 1000;

    // gnss power_stats are reported in microseconds. The transform function
    // converts microseconds to milliseconds.
    std::function<uint64_t(uint64_t)> gnssUsToMs = [](uint64_t a) { return a / US_TO_MS; };

    const GenericStateResidencyDataProvider::StateResidencyConfig gnssStateConfig = {
        .entryCountSupported = true,
        .entryCountPrefix = "count:",
        .totalTimeSupported = true,
        .totalTimePrefix = "duration_usec:",
        .totalTimeTransform = gnssUsToMs,
        .lastEntrySupported = true,
        .lastEntryPrefix = "last_entry_timestamp_usec:",
        .lastEntryTransform = gnssUsToMs,
    };

    const std::vector<std::pair<std::string, std::string>> gnssStateHeaders = {
        std::make_pair("ON", "GPS_ON:"),
        std::make_pair("OFF", "GPS_OFF:"),
    };

    std::vector<GenericStateResidencyDataProvider::PowerEntityConfig> cfgs;
    cfgs.emplace_back(generateGenericStateResidencyConfigs(gnssStateConfig, gnssStateHeaders),
            "GPS", "");

    p->addStateResidencyDataProvider(std::make_unique<GenericStateResidencyDataProvider>(
            "/dev/bbd_pwrstat", cfgs));

    p->addEnergyConsumer(PowerStatsEnergyConsumer::createMeterConsumer(p,
            EnergyConsumerType::GNSS, "GPS", {"L9S_GNSS_CORE"}));
}

void addPCIe(std::shared_ptr<PowerStats> p) {
    // Add PCIe power entities for Modem and WiFi
    const GenericStateResidencyDataProvider::StateResidencyConfig pcieStateConfig = {
        .entryCountSupported = true,
        .entryCountPrefix = "Cumulative count:",
        .totalTimeSupported = true,
        .totalTimePrefix = "Cumulative duration msec:",
        .lastEntrySupported = true,
        .lastEntryPrefix = "Last entry timestamp msec:",
    };
    const std::vector<std::pair<std::string, std::string>> pcieStateHeaders = {
        std::make_pair("UP", "Link up:"),
        std::make_pair("DOWN", "Link down:"),
    };

    // Add PCIe - Modem
    const std::vector<GenericStateResidencyDataProvider::PowerEntityConfig> pcieModemCfgs = {
        {generateGenericStateResidencyConfigs(pcieStateConfig, pcieStateHeaders), "PCIe-Modem",
                "Version: 1"}
    };

    p->addStateResidencyDataProvider(std::make_unique<GenericStateResidencyDataProvider>(
            "/sys/devices/platform/12100000.pcie/power_stats", pcieModemCfgs));

    // Add PCIe - WiFi
    const std::vector<GenericStateResidencyDataProvider::PowerEntityConfig> pcieWifiCfgs = {
        {generateGenericStateResidencyConfigs(pcieStateConfig, pcieStateHeaders),
            "PCIe-WiFi", "Version: 1"}
    };

    p->addStateResidencyDataProvider(std::make_unique<GenericStateResidencyDataProvider>(
            "/sys/devices/platform/13120000.pcie/power_stats", pcieWifiCfgs));
}

void addWifi(std::shared_ptr<PowerStats> p) {
    // The transform function converts microseconds to milliseconds.
    std::function<uint64_t(uint64_t)> usecToMs = [](uint64_t a) { return a / 1000; };
    const GenericStateResidencyDataProvider::StateResidencyConfig stateConfig = {
        .entryCountSupported = true,
        .entryCountPrefix = "count:",
        .totalTimeSupported = true,
        .totalTimePrefix = "duration_usec:",
        .totalTimeTransform = usecToMs,
        .lastEntrySupported = true,
        .lastEntryPrefix = "last_entry_timestamp_usec:",
        .lastEntryTransform = usecToMs,
    };
    const GenericStateResidencyDataProvider::StateResidencyConfig pcieStateConfig = {
        .entryCountSupported = true,
        .entryCountPrefix = "count:",
        .totalTimeSupported = true,
        .totalTimePrefix = "duration_usec:",
        .totalTimeTransform = usecToMs,
        .lastEntrySupported = false,
    };

    const std::vector<std::pair<std::string, std::string>> stateHeaders = {
        std::make_pair("AWAKE", "AWAKE:"),
        std::make_pair("ASLEEP", "ASLEEP:"),

    };
    const std::vector<std::pair<std::string, std::string>> pcieStateHeaders = {
        std::make_pair("L0", "L0:"),
        std::make_pair("L1", "L1:"),
        std::make_pair("L1_1", "L1_1:"),
        std::make_pair("L1_2", "L1_2:"),
        std::make_pair("L2", "L2:"),
    };

    const std::vector<GenericStateResidencyDataProvider::PowerEntityConfig> cfgs = {
        {generateGenericStateResidencyConfigs(stateConfig, stateHeaders), "WIFI", "WIFI"},
        {generateGenericStateResidencyConfigs(pcieStateConfig, pcieStateHeaders), "WIFI-PCIE",
                "WIFI-PCIE"}
    };

    p->addStateResidencyDataProvider(std::make_unique<GenericStateResidencyDataProvider>(
            "/sys/wifi/power_stats", cfgs));
}

void addUfs(std::shared_ptr<PowerStats> p) {
    p->addStateResidencyDataProvider(std::make_unique<UfsStateResidencyDataProvider>(
            "/sys/bus/platform/devices/13200000.ufs/ufs_stats/"));
}

void addPowerDomains(std::shared_ptr<PowerStats> p) {
    // A constant to represent the number of nanoseconds in one millisecond.
    const int NS_TO_MS = 1000000;

    std::function<uint64_t(uint64_t)> acpmNsToMs = [](uint64_t a) { return a / NS_TO_MS; };
    const GenericStateResidencyDataProvider::StateResidencyConfig cpuStateConfig = {
            .entryCountSupported = true,
            .entryCountPrefix = "on_count:",
            .totalTimeSupported = true,
            .totalTimePrefix = "total_on_time_ns:",
            .totalTimeTransform = acpmNsToMs,
            .lastEntrySupported = true,
            .lastEntryPrefix = "last_on_time_ns:",
            .lastEntryTransform = acpmNsToMs,
    };

    const std::vector<std::pair<std::string, std::string>> cpuStateHeaders = {
            std::make_pair("ON", ""),
    };

    std::vector<GenericStateResidencyDataProvider::PowerEntityConfig> cfgs;
    for (std::string name : {
            "pd-tpu",
            "pd-ispfe",
            "pd-eh",
            "pd-bw",
            "pd-aur",
            "pd-yuvp",
            "pd-tnr",
            "pd-rgbp",
            "pd-mfc",
            "pd-mcsc",
            "pd-gse",
            "pd-gdc",
            "pd-g2d",
            "pd-dpuf1",
            "pd-dpuf0",
            "pd-dpub",
            "pd-embedded_g3d",
            "pd-g3d"}) {
        cfgs.emplace_back(generateGenericStateResidencyConfigs(cpuStateConfig, cpuStateHeaders),
            name, name + ":");
    }

    p->addStateResidencyDataProvider(std::make_unique<GenericStateResidencyDataProvider>(
            "/sys/devices/platform/acpm_stats/pd_stats", cfgs));
}

void addDevfreq(std::shared_ptr<PowerStats> p) {
    p->addStateResidencyDataProvider(std::make_unique<DevfreqStateResidencyDataProvider>(
            "INT",
            "/sys/devices/platform/17000020.devfreq_int/devfreq/17000020.devfreq_int"));

    p->addStateResidencyDataProvider(std::make_unique<DevfreqStateResidencyDataProvider>(
            "INTCAM",
            "/sys/devices/platform/17000030.devfreq_intcam/devfreq/17000030.devfreq_intcam"));

    p->addStateResidencyDataProvider(std::make_unique<DevfreqStateResidencyDataProvider>(
            "DISP",
            "/sys/devices/platform/17000040.devfreq_disp/devfreq/17000040.devfreq_disp"));

    p->addStateResidencyDataProvider(std::make_unique<DevfreqStateResidencyDataProvider>(
            "CAM",
            "/sys/devices/platform/17000050.devfreq_cam/devfreq/17000050.devfreq_cam"));

    p->addStateResidencyDataProvider(std::make_unique<DevfreqStateResidencyDataProvider>(
            "TNR",
            "/sys/devices/platform/17000060.devfreq_tnr/devfreq/17000060.devfreq_tnr"));

    p->addStateResidencyDataProvider(std::make_unique<DevfreqStateResidencyDataProvider>(
            "MFC",
            "/sys/devices/platform/17000070.devfreq_mfc/devfreq/17000070.devfreq_mfc"));

    p->addStateResidencyDataProvider(std::make_unique<DevfreqStateResidencyDataProvider>(
            "BW",
            "/sys/devices/platform/17000080.devfreq_bw/devfreq/17000080.devfreq_bw"));

    p->addStateResidencyDataProvider(std::make_unique<DevfreqStateResidencyDataProvider>(
            "DSU",
            "/sys/devices/platform/17000090.devfreq_dsu/devfreq/17000090.devfreq_dsu"));

    p->addStateResidencyDataProvider(std::make_unique<DevfreqStateResidencyDataProvider>(
            "BCI",
            "/sys/devices/platform/170000a0.devfreq_bci/devfreq/170000a0.devfreq_bci"));
}

void addTPU(std::shared_ptr<PowerStats> p) {
    std::map<std::string, int32_t> stateCoeffs;

    stateCoeffs = {
        // TODO (b/197721618): Measuring the TPU power numbers
        {"226000",  10},
        {"455000",  20},
        {"627000",  30},
        {"712000",  40},
        {"845000",  50},
        {"967000",  60},
        {"1119000", 70}};

    p->addEnergyConsumer(PowerStatsEnergyConsumer::createMeterAndAttrConsumer(p,
            EnergyConsumerType::OTHER, "TPU", {"S7M_VDD_TPU"},
            {{UID_TIME_IN_STATE, "/sys/devices/platform/1a000000.rio/tpu_usage"}},
            stateCoeffs));
}

/**
 * Unlike other data providers, which source power entity state residency data from the kernel,
 * this data provider acts as a general-purpose channel for state residency data providers
 * that live in user space. Entities are defined here and user space clients of this provider's
 * vendor service register callbacks to provide state residency data for their given pwoer entity.
 */
void addPixelStateResidencyDataProvider(std::shared_ptr<PowerStats> p) {

    auto pixelSdp = std::make_unique<PixelStateResidencyDataProvider>();

    pixelSdp->addEntity("Bluetooth", {{0, "Idle"}, {1, "Active"}, {2, "Tx"}, {3, "Rx"}});

    pixelSdp->start();

    p->addStateResidencyDataProvider(std::move(pixelSdp));
}

void addDisplayMRR(std::shared_ptr<PowerStats> p) {
    p->addStateResidencyDataProvider(std::make_unique<DisplayMrrStateResidencyDataProvider>(
            "Display", "/sys/class/drm/card0/device/primary-panel/"));
}

void addZumaCommonDataProviders(std::shared_ptr<PowerStats> p) {
    setEnergyMeter(p);

    addAoC(p);
    addPixelStateResidencyDataProvider(p);
    addCPUclusters(p);
    addSoC(p);
    addGNSS(p);
    addMobileRadio(p);
    addNFC(p);
    addPCIe(p);
    addWifi(p);
    addTPU(p);
    addUfs(p);
    addPowerDomains(p);
    addDvfsStats(p);
    addDevfreq(p);
    addGPU(p);
}

void addNFC(std::shared_ptr<PowerStats> p) {
    const GenericStateResidencyDataProvider::StateResidencyConfig nfcStateConfig = {
        .entryCountSupported = true,
        .entryCountPrefix = "Cumulative count:",
        .totalTimeSupported = true,
        .totalTimePrefix = "Cumulative duration msec:",
        .lastEntrySupported = true,
        .lastEntryPrefix = "Last entry timestamp msec:",
    };
    const std::vector<std::pair<std::string, std::string>> nfcStateHeaders = {
        std::make_pair("IDLE", "Idle mode:"),
        std::make_pair("ACTIVE", "Active mode:"),
        std::make_pair("ACTIVE-RW", "Active Reader/Writer mode:"),
    };

    std::vector<GenericStateResidencyDataProvider::PowerEntityConfig> cfgs;
    cfgs.emplace_back(generateGenericStateResidencyConfigs(nfcStateConfig, nfcStateHeaders),
            "NFC", "NFC subsystem");

    std::string path;
    struct stat buffer;
    for (int i = 0; i < 10; i++) {
        std::string idx = std::to_string(i);
        path = "/sys/devices/platform/10c80000.hsi2c/i2c-" + idx + "/" + idx + "-0008/power_stats";
        if (!stat(path.c_str(), &buffer))
            break;
    }
    p->addStateResidencyDataProvider(std::make_unique<GenericStateResidencyDataProvider>(
            path, cfgs));
}
