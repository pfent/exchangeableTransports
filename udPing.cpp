#include <iostream>
#include <exchangeableTransports/util/tcpWrapper.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libibverbscpp/libibverbscpp.h>
#include <exchangeableTransports/rdma/Network.hpp>
#include <exchangeableTransports/rdma/QueuePair.hpp>

using namespace std;

constexpr uint16_t port = 1234;
constexpr auto ip = "127.0.0.1";

int main(int argc, char **argv) {
    if (argc < 2) {
        cout << "Usage: " << argv[0] << " <client / server>" << endl;
        return -1;
    }
    const auto isClient = argv[1][0] == 'c';

    ibv::device::DeviceList devices{};
    if (devices.size() != 1) {
        cout << "unexpected number of infiniband devices: " << devices.size() << endl;
        return -1;
    }

    static constexpr std::string_view data = "123456789012345678901234567890123456789012345678901234567890123\0"sv;
    auto net = rdma::Network();
    auto cq = rdma::CompletionQueuePair(net);
    auto qp = rdma::QueuePair(net, ibv::queuepair::Type::UD);

    std::array<char, 64> buf{};
    auto mr = net.registerMr(buf.data(), 64, {});

    auto socket = tcp_socket();
    if (isClient) {
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip, &addr.sin_addr);

        tcp_connect(socket, addr);

        auto remoteAddr = rdma::Address{qp.getQPN(), net.getLID()};
        tcp_write(socket, &remoteAddr, sizeof(remoteAddr));
        tcp_read(socket, &remoteAddr, sizeof(remoteAddr));
        qp.connect(remoteAddr);

        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = data[i];
        }

        {   // send the data
            auto send = ibv::workrequest::Simple<ibv::workrequest::Send>{};
            send.setLocalAddress(mr->getSlice());
            send.setInline();
            qp.postWorkRequest(send);
        }
        // since it was sent inline, we can safely reuse the buffer
        for (auto &i : buf) {
            i = 0;
        }

        {   // receive the data back again
            auto recv = ibv::workrequest::Recv{};
            recv.setId(42);
            auto receiveInfo = mr->getSlice();
            recv.setSge(&receiveInfo, 1);
            qp.postRecvRequest(recv);
            while (cq.pollSendCompletionQueue() != 42); // Poll until recv has finished
        }

        cout << "received: " << std::string(buf.begin(), buf.size()) << endl;
        // check if the data is still the same
        for (size_t i = 0; i < buf.size(); ++i) {
            if (buf[i] != data[i]) {
                throw;
            }
        }

    } else {
        {   // setup tcp socket
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = INADDR_ANY;

            tcp_bind(socket, addr);
            tcp_listen(socket);
        }
        sockaddr_in ignored{};
        auto acced = tcp_accept(socket, ignored);

        {   // connect Qps
            auto remoteAddr = rdma::Address{qp.getQPN(), net.getLID()};
            tcp_write(socket, &remoteAddr, sizeof(remoteAddr));
            tcp_read(socket, &remoteAddr, sizeof(remoteAddr));
            qp.connect(remoteAddr);
        }

        {   // receive into buf
            auto recv = ibv::workrequest::Recv{};
            recv.setId(42);
            auto receiveInfo = mr->getSlice();
            recv.setSge(&receiveInfo, 1);
            qp.postRecvRequest(recv);
            while (cq.pollSendCompletionQueue() != 42); // Poll until recv has finished
        }

        cout << "received: " << std::string(buf.begin(), buf.size()) << endl;

        {   // echo back the received data
            auto send = ibv::workrequest::Simple<ibv::workrequest::Send>{};
            send.setLocalAddress(mr->getSlice());
            send.setInline();
            qp.postWorkRequest(send);
        }

        tcp_close(acced);
    }

    tcp_close(socket);
}