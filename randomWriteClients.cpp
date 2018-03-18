#include <iostream>
#include <thread>
#include <exchangeableTransports/util/tcpWrapper.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libibverbscpp/libibverbscpp.h>
#include <exchangeableTransports/rdma/Network.hpp>
#include <exchangeableTransports/rdma/QueuePair.hpp>
#include <exchangeableTransports/util/bench.h>
#include <exchangeableTransports/rdma/RcQueuePair.h>
#include <exchangeableTransports/rdma/UcQueuePair.h>
#include <exchangeableTransports/rdma/UdQueuePair.h>
#include <random>

using namespace std;

constexpr uint16_t port = 1234;
constexpr auto ip = "127.0.0.1";
constexpr size_t SHAREDMEM_MESSAGES = 1024 * 1024;
constexpr size_t BIGBADBUFFER_SIZE = 1024 * 1024 * 8; // 8MB

void connectSocket(int socket) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    for (int i = 0;; ++i) {
        try {
            tcp_connect(socket, addr);
            break;
        } catch (...) {
            std::this_thread::sleep_for(20ms);
            if (i > 10) throw;
        }
    }
}

void setUpListenSocket(int socket) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    tcp_bind(socket, addr);
    tcp_listen(socket);
}

auto createWriteWrWithImm(const ibv::memoryregion::Slice &slice) {
    auto write = ibv::workrequest::Simple<ibv::workrequest::WriteWithImm>{};
    write.setLocalAddress(slice);
    write.setInline();
    write.setSignaled();
    return write;
}

template<class QueuePair>
void runImmData(bool isClient, uint32_t dataSize) {
    std::string data(dataSize, 'A');
    auto net = rdma::Network();
    auto &cq = net.getSharedCompletionQueue();
    auto qp = QueuePair(net);

    auto recvbuf = std::vector<char>(BIGBADBUFFER_SIZE * 2); // *2 just to be sure everything fits
    auto recvmr = net.registerMr(recvbuf.data(), recvbuf.size(),
                                 {ibv::AccessFlag::LOCAL_WRITE, ibv::AccessFlag::REMOTE_WRITE});
    auto sendbuf = std::vector<char>(data.size());
    auto sendmr = net.registerMr(sendbuf.data(), sendbuf.size(), {});

    auto generator = std::default_random_engine{};
    auto randomDistribution = std::uniform_int_distribution<uint32_t>{0, BIGBADBUFFER_SIZE};

    auto socket = tcp_socket();
    if (isClient) {
        connectSocket(socket);

        std::copy(data.begin(), data.end(), sendbuf.begin());

        auto recv = ibv::workrequest::Recv{};
        recv.setId(42);
        // *first* post recv to always have a recv pending, so incoming send don't get swallowed
        qp.postRecvRequest(recv);

        auto remoteAddr = rdma::Address{qp.getQPN(), net.getLID()};
        tcp_write(socket, &remoteAddr, sizeof(remoteAddr));
        tcp_read(socket, &remoteAddr, sizeof(remoteAddr));
        auto remoteMr = rdma::RemoteMemoryRegion{reinterpret_cast<uintptr_t>(recvbuf.data()), recvmr->getRkey()};
        tcp_write(socket, &remoteMr, sizeof(remoteMr));
        tcp_read(socket, &remoteMr, sizeof(remoteMr));

        qp.connect(remoteAddr);

        auto write = createWriteWrWithImm(sendmr->getSlice());

        bench(SHAREDMEM_MESSAGES, [&]() {
            for (size_t i = 0; i < SHAREDMEM_MESSAGES; ++i) {
                auto destPos = randomDistribution(generator);
                write.setRemoteAddress(remoteMr.address + destPos, remoteMr.key);
                write.setImmData(destPos);
                qp.postWorkRequest(write);
                cq.pollSendCompletionQueueBlocking(ibv::workcompletion::Opcode::RDMA_WRITE);

                auto wc = cq.pollRecvWorkCompletionBlocking();
                auto recvPos = wc.getImmData();

                qp.postRecvRequest(recv);

                auto begin = recvbuf.begin() + recvPos;
                auto end = begin + dataSize;
                // check if the data is still the same
                if (not std::equal(begin, end, data.begin(), data.end())) {
                    throw;
                }
            }
        }, 1);

    } else {
        setUpListenSocket(socket);

        const auto acced = [&] {
            sockaddr_in ignored{};
            return tcp_accept(socket, ignored);
        }();

        auto recv = ibv::workrequest::Recv{};
        recv.setId(42);
        recv.setSge(nullptr, 0);
        // *first* post recv to always have a recv pending, so incoming send don't get swallowed
        qp.postRecvRequest(recv);

        auto remoteAddr = rdma::Address{qp.getQPN(), net.getLID()};
        tcp_write(acced, &remoteAddr, sizeof(remoteAddr));
        tcp_read(acced, &remoteAddr, sizeof(remoteAddr));
        auto remoteMr = rdma::RemoteMemoryRegion{reinterpret_cast<uintptr_t>(recvbuf.data()), recvmr->getRkey()};
        tcp_write(acced, &remoteMr, sizeof(remoteMr));
        tcp_read(acced, &remoteMr, sizeof(remoteMr));

        qp.connect(remoteAddr);

        auto write = createWriteWrWithImm(sendmr->getSlice());

        bench(SHAREDMEM_MESSAGES, [&]() {
            for (size_t i = 0; i < SHAREDMEM_MESSAGES; ++i) {
                // wait for message being written
                auto wc = cq.pollRecvWorkCompletionBlocking();
                qp.postRecvRequest(recv);

                auto recvPos = wc.getImmData();

                auto begin = recvbuf.begin() + recvPos;
                auto end = begin + dataSize;
                std::copy(begin, end, sendbuf.begin());
                // echo back the received data
                auto destPos = randomDistribution(generator);
                write.setRemoteAddress(remoteMr.address + destPos, remoteMr.key);
                write.setImmData(destPos);
                qp.postWorkRequest(write);
                cq.pollSendCompletionQueueBlocking(ibv::workcompletion::Opcode::RDMA_WRITE);
            }
        }, 1);

        tcp_close(acced);
    }

    tcp_close(socket);
}

auto createWriteWr(const ibv::memoryregion::Slice &slice) {
    auto write = ibv::workrequest::Simple<ibv::workrequest::Write>{};
    write.setLocalAddress(slice);
    write.setInline();
    write.setSignaled();
    return write;
}

template<class QueuePair>
void runPostedWrs(bool isClient, size_t dataSize, uint8_t pollPositions) {
    std::string data(dataSize, 'A');
    auto net = rdma::Network();
    auto &cq = net.getSharedCompletionQueue();
    auto qp = QueuePair(net);

    auto recvbuf = std::vector<char>(BIGBADBUFFER_SIZE * 2); // *2 just to be sure everything fits
    auto recvmr = net.registerMr(recvbuf.data(), recvbuf.size(),
                                 {ibv::AccessFlag::LOCAL_WRITE, ibv::AccessFlag::REMOTE_WRITE});
    auto recvPosBuf = std::vector<int32_t>(pollPositions);
    auto recvPosMr = net.registerMr(recvPosBuf.data(), recvPosBuf.size() * sizeof(int32_t),
                                    {ibv::AccessFlag::LOCAL_WRITE, ibv::AccessFlag::REMOTE_WRITE});

    auto sendbuf = std::vector<char>(data.size());
    auto sendmr = net.registerMr(sendbuf.data(), sendbuf.size(), {});

    auto sendPosBuf = std::vector<int32_t>(1);
    auto sendPosMr = net.registerMr(sendPosBuf.data(), sendPosBuf.size() * sizeof(int32_t),
                                    {ibv::AccessFlag::LOCAL_WRITE, ibv::AccessFlag::REMOTE_WRITE});

    auto generator = std::default_random_engine{};
    auto randomDistribution = std::uniform_int_distribution<uint32_t>{0, BIGBADBUFFER_SIZE};

    auto socket = tcp_socket();
    if (isClient) {
        connectSocket(socket);

        std::copy(data.begin(), data.end(), sendbuf.begin());

        // invalidate recv address
        std::fill(recvPosBuf.begin(), recvPosBuf.end(), -1);

        auto remoteAddr = rdma::Address{qp.getQPN(), net.getLID()};
        tcp_write(socket, &remoteAddr, sizeof(remoteAddr));
        tcp_read(socket, &remoteAddr, sizeof(remoteAddr));
        auto remoteMr = rdma::RemoteMemoryRegion{reinterpret_cast<uintptr_t>(recvbuf.data()), recvmr->getRkey()};
        tcp_write(socket, &remoteMr, sizeof(remoteMr));
        tcp_read(socket, &remoteMr, sizeof(remoteMr));
        // TODO: vary the send address
        auto remotePosMr = rdma::RemoteMemoryRegion{
                reinterpret_cast<uintptr_t>(&recvPosBuf.back()), recvPosMr->getRkey()};
        tcp_write(socket, &remotePosMr, sizeof(remotePosMr));
        tcp_read(socket, &remotePosMr, sizeof(remotePosMr));

        qp.connect(remoteAddr);

        auto write = createWriteWr(sendmr->getSlice());
        auto posWrite = createWriteWr(sendPosMr->getSlice());
        posWrite.setRemoteAddress(remotePosMr.address, remotePosMr.key);

        bench(SHAREDMEM_MESSAGES, [&]() {
            for (size_t i = 0; i < SHAREDMEM_MESSAGES; ++i) {
                auto destPos = randomDistribution(generator);
                sendPosBuf[0] = destPos;
                write.setRemoteAddress(remoteMr.address + destPos, remoteMr.key);

                qp.postWorkRequest(write);
                qp.postWorkRequest(posWrite);
                cq.pollSendCompletionQueueBlocking(ibv::workcompletion::Opcode::RDMA_WRITE);
                cq.pollSendCompletionQueueBlocking(ibv::workcompletion::Opcode::RDMA_WRITE);

                // wait for incoming message
                int32_t sender;
                for (;;)
                    for (sender = 0; sender < pollPositions; ++sender) {
                        if (*static_cast<volatile int32_t *>(&recvPosBuf[sender]) != -1) {
                            goto found;
                        }
                    }
                found:
                auto recvPos = recvPosBuf[sender];
                recvPosBuf[sender] = -1;

                auto begin = recvbuf.begin() + recvPos;
                auto end = begin + dataSize;
                // check if the data is still the same
                if (not std::equal(begin, end, data.begin(), data.end())) {
                    throw;
                }
            }
        }, 1);

    } else {
        setUpListenSocket(socket);

        const auto acced = [&] {
            sockaddr_in ignored{};
            return tcp_accept(socket, ignored);
        }();

        // invalidate recv address
        std::fill(recvPosBuf.begin(), recvPosBuf.end(), -1);

        auto remoteAddr = rdma::Address{qp.getQPN(), net.getLID()};
        tcp_write(acced, &remoteAddr, sizeof(remoteAddr));
        tcp_read(acced, &remoteAddr, sizeof(remoteAddr));
        auto remoteMr = rdma::RemoteMemoryRegion{reinterpret_cast<uintptr_t>(recvbuf.data()), recvmr->getRkey()};
        tcp_write(acced, &remoteMr, sizeof(remoteMr));
        tcp_read(acced, &remoteMr, sizeof(remoteMr));
        // TODO: vary the send address
        auto remotePosMr = rdma::RemoteMemoryRegion{
                reinterpret_cast<uintptr_t>(&recvPosBuf.back()), recvPosMr->getRkey()};
        tcp_write(acced, &remotePosMr, sizeof(remotePosMr));
        tcp_read(acced, &remotePosMr, sizeof(remotePosMr));

        qp.connect(remoteAddr);

        auto write = createWriteWr(sendmr->getSlice());
        auto posWrite = createWriteWr(sendPosMr->getSlice());
        posWrite.setRemoteAddress(remotePosMr.address, remotePosMr.key);

        bench(SHAREDMEM_MESSAGES, [&]() {
            for (size_t i = 0; i < SHAREDMEM_MESSAGES; ++i) {
                // wait for incoming message
                int32_t sender;
                for (;;)
                    for (sender = 0; sender < pollPositions; ++sender) {
                        if (*static_cast<volatile int32_t *>(&recvPosBuf[sender]) != -1) {
                            goto found2;
                        }
                    }
                found2:
                auto recvPos = recvPosBuf[sender];
                recvPosBuf[sender] = -1;

                auto begin = recvbuf.begin() + recvPos;
                auto end = begin + dataSize;
                std::copy(begin, end, sendbuf.begin());
                // echo back the received data
                auto destPos = randomDistribution(generator);
                sendPosBuf[0] = destPos;
                write.setRemoteAddress(remoteMr.address + destPos, remoteMr.key);
                qp.postWorkRequest(write);
                qp.postWorkRequest(posWrite);
                cq.pollSendCompletionQueueBlocking(ibv::workcompletion::Opcode::RDMA_WRITE);
                cq.pollSendCompletionQueueBlocking(ibv::workcompletion::Opcode::RDMA_WRITE);
            }
        }, 1);

        tcp_close(acced);
    }

    tcp_close(socket);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        cout << "Usage: " << argv[0] << " <client / server>" << endl;
        return -1;
    }
    const auto isClient = argv[1][0] == 'c';

    cout << "size, connection, clients, messages, seconds, msgps, user, kernel, total" << '\n';
    const auto length = 64;
    for (const int clients : {1, 2, 4, 8, 16, 32, 64, 128, 192, 255}) {
        cout << length << ", MemPoll " << clients << ", ";
        runPostedWrs<rdma::RcQueuePair>(isClient, length, clients);
        cout << length << ", ImmData " << clients << ", ";
        runImmData<rdma::RcQueuePair>(isClient, length);
    }
}
