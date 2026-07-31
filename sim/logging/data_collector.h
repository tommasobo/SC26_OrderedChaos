#ifndef HTSIM_SIM_DATA_COLLECTOR_H_
#define HTSIM_SIM_DATA_COLLECTOR_H_

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <regex>
#include <string>
#include <vector>

#include "metric.h"

struct Filter {
    // Regular expression to match the metric name. If the regex matches the metric name, the filter
    // is applied.
    std::string regex = ".*";
    // Whether to enable the metrics that match the regex. If disabled, the metric is not logged or
    // exported.
    bool enabled = true;
    // Only applies to timeseries data. Specifies the downsampling ratio to log data. If 1, all data
    // is logged.
    uint64_t downsampling_ratio = 1;
    // Only applies to timeseries data. Specifies the interval in nanoseconds to log data. The first
    // sample in a log_every_ns interval is logged, and the rest are ignored.
    uint64_t log_every_ns = 0;

    inline std::string str() {
        return "Filter: regex=" + regex + ", enabled=" + std::to_string(enabled) +
               ", downsampling_ratio=" + std::to_string(downsampling_ratio);
    }
};

enum class OutputLocation {
    kFile,
    kStdout,
};

// DataCollector is a singleton class that manages all metrics in the simulation. It is responsible
// for creating and exporting metrics. It also applies filters to metrics based on their names. The
// filters are defined in a config file. The data is exported to a specified directory when the
// DataCollector is destroyed. The data directory is specified in the config file. If the directory
// is not specified, the default directory kDefaultDataDir is used.
// Config file format:
// {
//     "output_location": "file",       // "file" or "stdout"
//     "data_dir": "./data/",          // data directory in case of "file" output location
// "filters": [
//     {
//         "regex": ".*",               // regex to match the metric name
//         "enabled": true,             // whether to enable the metric
//         "downsampling_ratio": "3",   // downsampling ratio
//         "log_every_ns": "1000"       // log every 1000 ns
//     },
//     ...
// ]
// }
class DataCollector {
public:
    // Initialize the DataCollector with a config file. This function should be called only once.
    static void InitWithConfig(std::string config_file) {
        get_instance()._InitWithConfig(config_file);
    }

    static void InitWithJsonObject(nlohmann::json config) {
        get_instance()._InitWithJsonObject(config);
    }

    // Register a CsvMetric with the specified name and columns. If the metric already exists, it is
    // returned if return_existing is true, else it fails.
    static CsvMetric* RegisterCsvMetric(std::string              name,
                                        std::vector<std::string> columns,
                                        bool                     return_existing = true) {
        return get_instance()._RegisterCsvMetric(name, columns, return_existing);
    }

    // Register a TimeseriesMetric with the specified name and columns. If the metric already
    // exists, it is returned if return_existing is true, else it fails.
    static TimeSeriesMetric* RegisterTimeseriesMetric(std::string              name,
                                                      std::vector<std::string> columns,
                                                      bool return_existing = true) {
        return get_instance()._RegisterTimeseriesMetric(name, columns, return_existing);
    }

    // Register a KeyValueMetric with the specified name and columns. If the metric already exists,
    // it is returned if return_existing is true, else it fails.
    static KeyValueMetric* RegisterKeyValueMetric(std::string name, bool return_existing = true) {
        return get_instance()._RegisterKeyValueMetric(name, return_existing);
    }

    // Log a key-value pair to the global key-value metric.
    static void LogGlobalKeyValue(std::string key, std::string value) {
        get_instance()._LogGlobalKeyValue(key, value);
    };

    static KeyValueMetric* GetGlobalKeyValueMetric() {
        return (get_instance().global_key_value_metric_).get();
    }

    static void setDataDir(std::string data_dir) { get_instance()._setDataDir(data_dir); }

private:
    static constexpr const char* kDefaultDataDir = "./output_metrics/";

    static inline DataCollector& get_instance() {
        static DataCollector instance;
        return instance;
    }

    // Object implementations of singleton interface
    void              _InitWithConfig(std::string config_file);
    void              _InitWithJsonObject(nlohmann::json config);
    CsvMetric*        _RegisterCsvMetric(std::string              name,
                                         std::vector<std::string> columns,
                                         bool                     return_existing = true);
    TimeSeriesMetric* _RegisterTimeseriesMetric(std::string              name,
                                                std::vector<std::string> columns,
                                                bool                     return_existing = true);
    KeyValueMetric*   _RegisterKeyValueMetric(std::string name, bool return_existing = true);
    void              _LogGlobalKeyValue(std::string key, std::string value);

    // Private constructor to ensure that the DataCollector is a singleton.
    DataCollector() {
        data_dir_                = kDefaultDataDir;
        output_location_         = OutputLocation::kFile;
        initialized_             = false;
        global_key_value_metric_ = std::make_unique<KeyValueMetric>("global_key_value", true);
    };

    // Destructor that exports all metrics to files. It is called when the program exits. If the
    // data directory does not exist, it is created using the mkdir -p command.
    ~DataCollector();

    // Export a metric to the specified output location.
    void ExportMetric(std::string name, Metric* metric);

    void _setDataDir(std::string data_dir) { data_dir_ = data_dir; }

    // Match the metric name with the filters that are defined in the config file. Return the first
    // filter that matches the metric name. If no filter matches, return the filter with default
    // values.
    Filter MatchRegex(std::string name);
    // Check if the existing metric is a CsvMetric with the same columns. If not, throw an error.
    // The existing metric is the downcasted version of the metric that is already in the registry.
    void AssertExistingCsvMetricAfterDowncast(const CsvMetric*                existing,
                                              const std::vector<std::string>& columns);

    // The DataCollector global instance only be initialized once.
    bool           initialized_;
    nlohmann::json config_;

    std::vector<Filter> filters_;  // If empty, the default filter is applied.
    OutputLocation      output_location_;
    std::string         data_dir_;  // Directory to export the data. Default is kDefaultDataDir.
    std::map<std::string, std::unique_ptr<Metric>>
        metrics_registry_;  // map: metric_name -> Metric unique_ptr

    // The global key-value metric is a special metric that is used to log key-value pairs that are
    // not associated with a specific object. The key-value pairs are logged to a single CSV file
    // and are always enabled.
    std::unique_ptr<KeyValueMetric> global_key_value_metric_;
};

#endif  // HTSIM_SIM_DATA_COLLECTOR_H_
