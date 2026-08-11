#pragma once

#include <array>
#include <functional>
#include <list>
#include <memory>

#include "packet.h"
#include "packet_flow.h"

/// @brief Manages switch buffer state across all ports.
/// The switch buffer is used to store packets that are waiting to be transmitted.
/// This simple implementation assumes that the buffer is shared among all the ports of the switch
/// and has a single max buffer size that ensures that the buffer does not overflow.
class SwitchBuffer {
public:
    SwitchBuffer(uint32_t maxBufferSize);

    virtual ~SwitchBuffer() {}

    inline uint32_t getMaxBufferSize() { return _max_buffer_size; }

    inline uint32_t getRemainingBufferSize() { return _max_buffer_size - _bytes_in_buffer; }

    /// Use the buffer for the packet and update the state of the buffer.
    virtual void useBuffer(Packet* pkt);
    /// Free the buffer used by the packet. It is assumed that the packet is the one that was
    /// previously stored in the buffer.
    virtual void freeBuffer(Packet* pkt);

protected:
    const uint32_t _max_buffer_size;
    uint32_t       _bytes_in_buffer;
};
