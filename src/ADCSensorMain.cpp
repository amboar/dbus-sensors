/*
// Copyright (c) 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <ADCSensor.hpp>
#include <Utils.hpp>
#include <VariantVisitors.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/container/flat_set.hpp>
#include <experimental/filesystem>
#include <fstream>
#include <regex>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

static constexpr bool DEBUG = false;

namespace fs = std::experimental::filesystem;
static constexpr std::array<const char*, 1> SENSOR_TYPES = {
    "xyz.openbmc_project.Configuration.ADC"};
static std::regex INPUT_REGEX(R"(in(\d+)_input)");

void createSensors(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::unique_ptr<ADCSensor>>&
        sensors,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const std::unique_ptr<boost::container::flat_set<std::string>>&
        sensorsChanged)
{
    bool firstScan = sensorsChanged == nullptr;
    // use new data the first time, then refresh
    ManagedObjectType sensorConfigurations;
    bool useCache = false;
    for (const char* type : SENSOR_TYPES)
    {
        if (!getSensorConfiguration(type, dbusConnection, sensorConfigurations,
                                    useCache))
        {
            std::cerr << "error communicating to entity manager\n";
            return;
        }
        useCache = true;
    }
    std::vector<fs::path> paths;
    if (!find_files(fs::path("/sys/class/hwmon"), R"(in\d+_input)", paths))
    {
        std::cerr << "No temperature sensors in system\n";
        return;
    }

    // iterate through all found adc sensors, and try to match them with
    // configuration
    for (auto& path : paths)
    {
        std::smatch match;
        std::string pathStr = path.string();

        std::regex_search(pathStr, match, INPUT_REGEX);
        std::string indexStr = *(match.begin() + 1);

        auto directory = path.parent_path();
        // convert to 0 based
        size_t index = std::stoul(indexStr) - 1;
        auto oem_name_path =
            directory.string() + R"(/of_node/oemname)" + std::to_string(index);

        if (DEBUG)
            std::cout << "Checking path " << oem_name_path << "\n";
        std::ifstream nameFile(oem_name_path);
        if (!nameFile.good())
        {
            std::cerr << "Failure reading " << oem_name_path << "\n";
            continue;
        }
        std::string oemName;
        std::getline(nameFile, oemName);
        nameFile.close();
        if (!oemName.size())
        {
            // shouldn't have an empty name file
            continue;
        }
        oemName.pop_back(); // remove trailing null

        const SensorData* sensorData = nullptr;
        const std::string* interfacePath = nullptr;
        for (const std::pair<sdbusplus::message::object_path, SensorData>&
                 sensor : sensorConfigurations)
        {
            if (!boost::ends_with(sensor.first.str, oemName))
            {
                continue;
            }
            sensorData = &(sensor.second);
            interfacePath = &(sensor.first.str);
            break;
        }
        if (sensorData == nullptr)
        {
            std::cerr << "failed to find match for " << oemName << "\n";
            continue;
        }
        const std::pair<std::string, boost::container::flat_map<
                                         std::string, BasicVariantType>>*
            baseConfiguration = nullptr;
        for (const char* type : SENSOR_TYPES)
        {
            auto sensorBase = sensorData->find(type);
            if (sensorBase != sensorData->end())
            {
                baseConfiguration = &(*sensorBase);
                break;
            }
        }

        if (baseConfiguration == nullptr)
        {
            std::cerr << "error finding base configuration for" << oemName
                      << "\n";
            continue;
        }

        auto findSensorName = baseConfiguration->second.find("Name");
        if (findSensorName == baseConfiguration->second.end())
        {
            std::cerr << "could not determine configuration name for "
                      << oemName << "\n";
            continue;
        }
        std::string sensorName =
            sdbusplus::message::variant_ns::get<std::string>(
                findSensorName->second);

        // on rescans, only update sensors we were signaled by
        auto findSensor = sensors.find(sensorName);
        if (!firstScan && findSensor != sensors.end())
        {
            bool found = false;
            for (auto it = sensorsChanged->begin(); it != sensorsChanged->end();
                 it++)
            {
                if (boost::ends_with(*it, findSensor->second->name))
                {
                    sensorsChanged->erase(it);
                    findSensor->second = nullptr;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                continue;
            }
        }
        std::vector<thresholds::Threshold> sensorThresholds;
        if (!ParseThresholdsFromConfig(*sensorData, sensorThresholds))
        {
            std::cerr << "error populating thresholds for " << sensorName
                      << "\n";
        }

        auto findScaleFactor = baseConfiguration->second.find("ScaleFactor");
        float scaleFactor = 1.0;
        if (findScaleFactor != baseConfiguration->second.end())
        {
            scaleFactor = mapbox::util::apply_visitor(VariantToFloatVisitor(),
                                                      findScaleFactor->second);
        }
        sensors[sensorName] = std::make_unique<ADCSensor>(
            path.string(), objectServer, dbusConnection, io, sensorName,
            std::move(sensorThresholds), scaleFactor, *interfacePath);
    }
}

int main(int argc, char** argv)
{
    boost::asio::io_service io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    systemBus->request_name("xyz.openbmc_project.ADCSensor");
    sdbusplus::asio::object_server objectServer(systemBus);
    boost::container::flat_map<std::string, std::unique_ptr<ADCSensor>> sensors;
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;
    std::unique_ptr<boost::container::flat_set<std::string>> sensorsChanged =
        std::make_unique<boost::container::flat_set<std::string>>();

    io.post([&]() {
        createSensors(io, objectServer, sensors, systemBus, nullptr);
    });

    boost::asio::deadline_timer filterTimer(io);
    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&](sdbusplus::message::message& message) {
            if (message.is_method_error())
            {
                std::cerr << "callback method error\n";
                return;
            }
            sensorsChanged->insert(message.get_path());
            // this implicitly cancels the timer
            filterTimer.expires_from_now(boost::posix_time::seconds(1));

            filterTimer.async_wait([&](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    /* we were canceled*/
                    return;
                }
                else if (ec)
                {
                    std::cerr << "timer error\n";
                    return;
                }
                createSensors(io, objectServer, sensors, systemBus,
                              sensorsChanged);
            });
        };

    for (const char* type : SENSOR_TYPES)
    {
        auto match = std::make_unique<sdbusplus::bus::match::match>(
            static_cast<sdbusplus::bus::bus&>(*systemBus),
            "type='signal',member='PropertiesChanged',path_namespace='" +
                std::string(INVENTORY_PATH) + "',arg0namespace='" + type + "'",
            eventHandler);
        matches.emplace_back(std::move(match));
    }

    io.run();
}
