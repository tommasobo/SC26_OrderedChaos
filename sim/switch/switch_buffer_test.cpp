#include "switch_buffer.h"

#include <gtest/gtest.h>

class DummyPacket : public Packet {
public:
    DummyPacket(uint32_t size, PktPriority priority) : _priority(priority) { _size = size; }

    PktPriority priority() const override { return _priority; }

    PktPriority _priority;
};

TEST(SwitchBufferTest, UseBuffer) {
    std::unique_ptr<SwitchBuffer> buffer_ = std::make_unique<SwitchBuffer>(10000);
    DummyPacket                   pkt(5000, Packet::PRIO_LO);
    buffer_->useBuffer(&pkt);
    EXPECT_EQ(buffer_->getRemainingBufferSize(), 5000);
}

TEST(SwitchBufferTest, FreeBuffer) {
    std::unique_ptr<SwitchBuffer> buffer_ = std::make_unique<SwitchBuffer>(10000);
    DummyPacket                   pkt(5000, Packet::PRIO_LO);
    buffer_->useBuffer(&pkt);
    buffer_->freeBuffer(&pkt);
    EXPECT_EQ(buffer_->getRemainingBufferSize(), 10000);
}

TEST(SwitchBufferTest, BufferOverflow) {
    std::unique_ptr<SwitchBuffer> buffer_ = std::make_unique<SwitchBuffer>(10000);
    DummyPacket                   pkt(15000, Packet::PRIO_LO);
    EXPECT_THROW(buffer_->useBuffer(&pkt), std::runtime_error);
}

TEST(SwitchBufferTest, BufferUnderflow) {
    std::unique_ptr<SwitchBuffer> buffer_ = std::make_unique<SwitchBuffer>(10000);
    DummyPacket                   pkt(5000, Packet::PRIO_LO);
    buffer_->useBuffer(&pkt);
    DummyPacket pkt2(6000, Packet::PRIO_LO);
    EXPECT_THROW(buffer_->freeBuffer(&pkt2), std::runtime_error);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
