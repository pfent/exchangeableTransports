#ifndef RDMA_RDMAMESSAGEBUFFER_H
#define RDMA_RDMAMESSAGEBUFFER_H

#include <atomic>
#include <vector>
#include <memory>
#include "util/RDMANetworking.h"

namespace l5 {
namespace util {
class Socket;
}
namespace datastructure {
class RDMAMessageBuffer {
public:

    /// Send data to the remote site
    void send(const uint8_t *data, size_t length);

    void send(const uint8_t *data, size_t length, bool inln);

    /// Receive data to a freshly allocated data vector
    std::vector<uint8_t> receive();

    /// Receive to a specific memory region with at last maxSize
    size_t receive(void *whereTo, size_t maxSize);

    /// Construct a message buffer of the given size, exchanging RDMA networking information over the given socket
    /// size _must_ be a power of 2.
    RDMAMessageBuffer(size_t size, util::Socket &sock);

    /// whether there is data to be read non-blockingly
    bool hasData() const;

private:
    const size_t size;
    util::RDMANetworking net;
    std::unique_ptr<volatile uint8_t[]> receiveBuffer;
    std::atomic<size_t> readPos{0};
    std::unique_ptr<uint8_t[]> sendBuffer;
    size_t messageCounter = 0;
    size_t sendPos = 0;
    volatile size_t currentRemoteReceive = 0;
    rdma::MemoryRegion localSend;
    rdma::MemoryRegion localReceive;
    rdma::MemoryRegion localReadPos;
    rdma::MemoryRegion localCurrentRemoteReceive;
    ibv::memoryregion::RemoteAddress remoteReceive;
    ibv::memoryregion::RemoteAddress remoteReadPos;

    void writeToSendBuffer(const uint8_t *data, size_t sizeToWrite);

    void readFromReceiveBuffer(size_t readPos, uint8_t *whereTo, size_t sizeToRead) const;

    void zeroReceiveBuffer(size_t beginReceiveCount, size_t sizeToZero);
};
} // namespace datastructure
} // namespace l5

#endif //RDMA_RDMAMESSAGEBUFFER_H
