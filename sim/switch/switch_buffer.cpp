#include "switch_buffer.h"

#include <iostream>

SwitchBuffer::SwitchBuffer(uint32_t maxBufferSize)
    : _max_buffer_size(maxBufferSize), _bytes_in_buffer(0) {}

void SwitchBuffer::useBuffer(Packet* pkt) {
    if (_bytes_in_buffer + pkt->size() > _max_buffer_size) {
        throw std::runtime_error("Buffer overflow.");
    }
    _bytes_in_buffer += pkt->size();
}

void SwitchBuffer::freeBuffer(Packet* pkt) {
    if (_bytes_in_buffer < pkt->size()) {
        throw std::runtime_error("Buffer underflow.");
    }
    _bytes_in_buffer -= pkt->size();
}
