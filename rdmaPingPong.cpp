#include <iostream>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <rdma_tests/rdma/CompletionQueuePair.hpp>
#include <rdma_tests/rdma/QueuePair.hpp>
#include <rdma_tests/rdma/MemoryRegion.hpp>
#include <rdma_tests/rdma/WorkRequest.hpp>
#include <infiniband/verbs.h>
#include "rdma/Network.hpp"
#include "tcpWrapper.h"

using namespace std;
using namespace rdma;

void exchangeQPNAndConnect(int sock, Network &network, QueuePair &queuePair);

void receiveAndSetupRmr(int sock, RemoteMemoryRegion &buffer);

void sendRmrInfo(int sock, MemoryRegion &buffer);

int main(int argc, char **argv) {
    if (argc < 3 || (argv[1][0] == 'c' && argc < 4)) {
        cout << "Usage: " << argv[0] << " <client / server> <Port> [IP (if client)]" << endl;
        return -1;
    }
    const auto isClient = argv[1][0] == 'c';
    const auto port = ::atoi(argv[2]);

    auto sock = tcp_socket();

    static const size_t MESSAGES = 1024 * 128;
    static const size_t BUFFER_SIZE = 64;

    // RDMA networking. The queues are needed on both sides
    Network network;
    CompletionQueuePair completionQueue(network);
    QueuePair queuePair(network, completionQueue);

    if (isClient) {
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, argv[3], &addr.sin_addr);

        tcp_connect(sock, addr);
        exchangeQPNAndConnect(sock, network, queuePair);

        uint8_t receiveBuffer[BUFFER_SIZE]{0};
        uint8_t sendBuffer[] = "123456789012345678901234567890123456789012345678901234567890123";
        static_assert(BUFFER_SIZE == sizeof(sendBuffer), "sendBuffer needs the right size ");
        MemoryRegion localSend(sendBuffer, BUFFER_SIZE, network.getProtectionDomain(), MemoryRegion::Permission::All);
        MemoryRegion localReceive(receiveBuffer, BUFFER_SIZE, network.getProtectionDomain(),
                                  MemoryRegion::Permission::All);
        RemoteMemoryRegion remoteReceive;
        sendRmrInfo(sock, localReceive);
        receiveAndSetupRmr(sock, remoteReceive);

        const auto start = chrono::steady_clock::now();
        for (size_t i = 0; i < MESSAGES; ++i) {
            WriteWorkRequestBuilder(localSend, remoteReceive, true)
                    .send(queuePair);
            completionQueue.pollSendCompletionQueue(IBV_WC_RDMA_WRITE);
            while (receiveBuffer[BUFFER_SIZE - 2] == 0) sched_yield(); // sync with response
            for (size_t j = 0; j < BUFFER_SIZE; ++j) {
                if (receiveBuffer[j] != sendBuffer[j]) {
                    throw runtime_error{"expected '1~9', received " + string(begin(receiveBuffer), end(receiveBuffer))};
                }
            }
            fill(begin(receiveBuffer), end(receiveBuffer), 0);
        }
        const auto end = chrono::steady_clock::now();
        const auto msTaken = chrono::duration<double, milli>(end - start).count();
        const auto sTaken = msTaken / 1000;
        cout << MESSAGES << " " << sizeof(sendBuffer) << "B messages exchanged in " << msTaken << "ms" << endl;
        cout << MESSAGES / sTaken << " msg/s" << endl;
    } else {
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        tcp_bind(sock, addr);
        listen(sock, SOMAXCONN);
        sockaddr_in inAddr;

        auto acced = tcp_accept(sock, inAddr);
        exchangeQPNAndConnect(acced, network, queuePair);

        uint8_t receiveBuffer[BUFFER_SIZE];
        uint8_t sendBuffer[BUFFER_SIZE];
        MemoryRegion localReceive(receiveBuffer, BUFFER_SIZE, network.getProtectionDomain(),
                                  MemoryRegion::Permission::All);
        MemoryRegion localSend(sendBuffer, BUFFER_SIZE, network.getProtectionDomain(), MemoryRegion::Permission::All);
        RemoteMemoryRegion remoteReceive;
        sendRmrInfo(acced, localReceive);
        receiveAndSetupRmr(acced, remoteReceive);

        for (size_t i = 0; i < MESSAGES; ++i) {
            while (receiveBuffer[BUFFER_SIZE - 2] == 0) sched_yield();
            copy(begin(receiveBuffer), end(receiveBuffer), begin(sendBuffer));
            fill(begin(receiveBuffer), end(receiveBuffer), 0);
            WriteWorkRequestBuilder(localSend, remoteReceive, true)
                    .send(queuePair);
            completionQueue.pollSendCompletionQueue(IBV_WC_RDMA_WRITE);
        }

        close(acced);
    }

    close(sock);
    return 0;
}

struct RmrInfo {
    uint32_t bufferKey;
    uintptr_t bufferAddress;
};

void receiveAndSetupRmr(int sock, RemoteMemoryRegion &buffer) {
    RmrInfo rmrInfo;
    tcp_read(sock, &rmrInfo, sizeof(rmrInfo));
    rmrInfo.bufferKey = ntohl(rmrInfo.bufferKey);
    rmrInfo.bufferAddress = be64toh(rmrInfo.bufferAddress);
    buffer.key = rmrInfo.bufferKey;
    buffer.address = rmrInfo.bufferAddress;
}

void sendRmrInfo(int sock, MemoryRegion &buffer) {
    RmrInfo rmrInfo;
    rmrInfo.bufferKey = htonl(buffer.key->rkey);
    rmrInfo.bufferAddress = htobe64((uint64_t) buffer.address);
    tcp_write(sock, &rmrInfo, sizeof(rmrInfo));
}

void exchangeQPNAndConnect(int sock, Network &network, QueuePair &queuePair) {
    rdma::Address addr;
    addr.lid = network.getLID();
    addr.qpn = queuePair.getQPN();
    tcp_write(sock, &addr, sizeof(addr)); // Send own qpn to server
    tcp_read(sock, &addr, sizeof(addr)); // receive qpn
    queuePair.connect(addr);
    cout << "connected to qpn " << addr.qpn << " lid: " << addr.lid << endl;
}

