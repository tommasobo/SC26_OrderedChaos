#include "data_collector.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "metric.h"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;

void DataCollector::_InitWithJsonObject(nlohmann::json config) {
    if (initialized_) {
        throw std::runtime_error("DataCollector has already been initialized");
    }
    initialized_ = true;
    config_      = config;

    // Initialize the output location.
    if (config_.find("output_location") != config_.end()) {
        auto output_location = config_["output_location"].get<std::string>();
        if (output_location == "file") {
            output_location_ = OutputLocation::kFile;
        } else if (output_location == "stdout") {
            output_location_ = OutputLocation::kStdout;
        } else {
            throw std::runtime_error("Invalid output location: " + output_location);
        }
    }

    // Initialize the data directory.
    if (config_.find("data_dir") != config_.end()) {
        data_dir_ = config_["data_dir"].get<std::string>();
    }

    // Initialize the filters.
    if (config_.find("filters") != config_.end()) {
        for (auto& filter_json : config_["filters"]) {
            // "regex" field in a filter is required.
            assert(filter_json.find("regex") != filter_json.end());
            // filter will be initialized with default values. Only the fields that are present
            // in the json will be updated.
            Filter filter;
            for (auto& [key, value] : filter_json.items()) {
                if (key == "regex") {
                    filter.regex = value.get<std::string>();
                } else if (key == "enabled") {
                    filter.enabled = value.get<bool>();
                } else if (key == "downsampling_ratio") {
                    filter.downsampling_ratio = value.get<uint64_t>();
                    assert(filter.downsampling_ratio > 0);
                } else if (key == "log_every_ns") {
                    filter.log_every_ns = value.get<uint64_t>();
                }
            }
            filters_.push_back(filter);
        }
    }
}

// Initialize the DataCollector with a config file. This function should be called only once.
void DataCollector::_InitWithConfig(std::string config_file) {
    std::ifstream config_stream(config_file);
    if (!config_stream.is_open()) {
        throw std::runtime_error("Failed to open config file: " + config_file);
    }
    config_ = nlohmann::json::parse(config_stream);
    InitWithJsonObject(config_);
}

// Destructor that exports all metrics to files. It is called when the program exits. If the
// data directory does not exist, it is created using the mkdir -p command.
DataCollector::~DataCollector() {
    return;  // Do not export metrics on destruction. Export them explicitly using ExportMetrics().
    // Create data directory if it does not exist.
    if (output_location_ == OutputLocation::kFile) {
        try {
            fs::create_directories(data_dir_);
        } catch (const fs::filesystem_error& e) {
            std::cerr << "Failed to create directory: " + data_dir_ + "\nError: " + e.what()
                      << std::endl;
            return;
        }
    }

    for (auto& [name, metric] : metrics_registry_) {
        ExportMetric(name, metric.get());
    }
    ExportMetric(global_key_value_metric_->name(), global_key_value_metric_.get());
}

void DataCollector::ExportMetric(std::string name, Metric* metric) {
    if (output_location_ == OutputLocation::kFile) {
        std::ofstream file(data_dir_ + "/" + name + ".csv");
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file: " + data_dir_ + "/" + name + ".csv");
        }
        metric->ExportData(file);
    } else if (output_location_ == OutputLocation::kStdout) {
        metric->ExportData(std::cout);
    }
}

// Match the metric name with the filters that are defined in the config file. Return the first
// filter that matches the metric name. If no filter matches, return the filter with default
// values.
Filter DataCollector::MatchRegex(std::string name) {
    for (auto& filter : filters_) {
        if (std::regex_match(name, std::regex(filter.regex))) {
            return filter;
        }
    }
    return Filter();  // default filter
}

// Check if the existing metric is a CsvMetric with the same columns. If not, throw an error.
// The existing metric is the downcasted version of the metric that is already in the registry.
void DataCollector::AssertExistingCsvMetricAfterDowncast(const CsvMetric*                existing,
                                                         const std::vector<std::string>& columns) {
    if (existing == nullptr) {
        throw std::runtime_error("Metric already exists with a different type");
    }
    if (existing->columns() != columns) {
        throw std::runtime_error("Metric " + existing->name() +
                                 " already exists with different columns");
    }
}

// Register a CsvMetric with the specified name and columns. If the metric already exists, it is
CsvMetric* DataCollector::_RegisterCsvMetric(std::string              name,
                                             std::vector<std::string> columns,
                                             bool                     return_existing /*= true*/) {
    if (metrics_registry_.find(name) != metrics_registry_.end()) {
        if (return_existing) {
            // Metric already exists. Check if it is a CsvMetric with the same columns. If not,
            // throw an error.
            auto existing_metric = dynamic_cast<CsvMetric*>(metrics_registry_[name].get());
            AssertExistingCsvMetricAfterDowncast(existing_metric, columns);
            return existing_metric;
        } else {
            throw std::runtime_error("Metric " + name + " already exists");
        }
    }
    Filter chosen_filter = MatchRegex(name);
    /* std::cout << "Chosen filter for metric " << name << ": " << chosen_filter.str() << std::endl; */
    metrics_registry_[name] = std::make_unique<CsvMetric>(name, chosen_filter.enabled, columns);
    return dynamic_cast<CsvMetric*>(metrics_registry_[name].get());
}

TimeSeriesMetric* DataCollector::_RegisterTimeseriesMetric(std::string              name,
                                                           std::vector<std::string> columns,
                                                           bool return_existing /*= true*/) {
    if (metrics_registry_.find(name) != metrics_registry_.end()) {
        if (return_existing) {
            // Metric already exists. Check if it is a TimeSeriesMetric with the same columns.
            // If not, throw an error.
            auto existing_metric = dynamic_cast<TimeSeriesMetric*>(metrics_registry_[name].get());
            AssertExistingCsvMetricAfterDowncast(existing_metric, columns);
            return existing_metric;
        } else {
            throw std::runtime_error("Metric " + name + " already exists");
        }
    }
    Filter chosen_filter = MatchRegex(name);
    /* std::cout << "Chosen filter for metric " << name << ": " << chosen_filter.str() << std::endl; */
    metrics_registry_[name] = std::make_unique<TimeSeriesMetric>(name,
                                                                 chosen_filter.enabled,
                                                                 columns,
                                                                 chosen_filter.downsampling_ratio,
                                                                 chosen_filter.log_every_ns);
    return dynamic_cast<TimeSeriesMetric*>(metrics_registry_[name].get());
}

KeyValueMetric* DataCollector::_RegisterKeyValueMetric(std::string name,
                                                       bool        return_existing /*= true*/) {
    if (metrics_registry_.find(name) != metrics_registry_.end()) {
        if (return_existing) {
            auto existing_metric = dynamic_cast<KeyValueMetric*>(metrics_registry_[name].get());
            if (existing_metric == nullptr) {
                throw std::runtime_error("Metric already exists with a different type");
            }
            return existing_metric;
        } else {
            throw std::runtime_error("Metric " + name + " already exists");
        }
    }
    Filter chosen_filter = MatchRegex(name);
    /* std::cout << "Chosen filter for metric " << name << ": " << chosen_filter.str() << std::endl; */
    metrics_registry_[name] = std::make_unique<KeyValueMetric>(name, chosen_filter.enabled);
    return dynamic_cast<KeyValueMetric*>(metrics_registry_[name].get());
}

void DataCollector::_LogGlobalKeyValue(std::string key, std::string value) {
    global_key_value_metric_->LogData({key, value});
}
