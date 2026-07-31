#pragma once

#include <cstdint>
#include <string>

#include "logger_types.h"

class DataReceiver : public Logged {
public:
    DataReceiver(const std::string& name) : Logged(name){};
    virtual ~DataReceiver(){};
    virtual uint64_t cumulative_ack() = 0;
    virtual uint32_t drops()          = 0;
};