#pragma once
#include <string>

#include "packet.h"
#include "packet_flow.h"

class NIC {
public:
    NIC(id_t src_id) : _src_id(src_id) {}

    virtual const string& nodename() const = 0;

    id_t src_id() const { return _src_id; }

protected:
    id_t _src_id;
};
