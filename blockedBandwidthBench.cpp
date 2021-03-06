#include <include/DomainSocketsTransport.h>
#include <include/SharedMemoryTransport.h>
#include "include/RdmaTransport.h"
#include <thread>
#include <include/TcpTransport.h>
#include <util/doNotOptimize.h>
#include <util/ycsb.h>
#include "util/bench.h"

using namespace l5::transport;

constexpr size_t operator "" _k(unsigned long long i) { return i * 1024; }

constexpr size_t operator "" _m(unsigned long long i) { return i * 1024 * 1024; }

constexpr size_t operator "" _g(unsigned long long i) { return i * 1024 * 1024 * 1024; }

static constexpr auto printResults = []
      (double workSize, auto avgTime, auto userPercent, auto systemPercent, auto totalPercent) {
   std::cout << workSize / 1e6 << ", "
             << avgTime << ", "
             << (workSize / 1e6 / avgTime) << ", "
             << userPercent << ", "
             << systemPercent << ", "
             << totalPercent << '\n';
};

static constexpr uint16_t port = 1234;
static const char* ip = "127.0.0.1";
static constexpr size_t DATA_SIZE = 10_g;
static constexpr size_t BUFFER_SIZE = 256_m;

struct TestData {
   std::vector<uint8_t> testdata;

   TestData(bool isClient) : testdata(DATA_SIZE) {
      if (not isClient) {
         RandomString rand;
         rand.fill(DATA_SIZE, reinterpret_cast<char*>(testdata.data()));
      }
   }

   auto begin() {
      return testdata.begin();
   }

   auto end() {
      return testdata.end();
   }

   auto size() {
      return testdata.size();
   }

   auto data() {
      return testdata.data();
   }
};

template<class Server, class Client>
void
doRun(const std::string &name, bool isClient, const std::string &connection, TestData &testdata, size_t chunkSize) {
   if (isClient) {
      std::cout << "connecting to " << connection << "..." << std::flush;

      auto client = Client();
      for (int i = 0;; ++i) {
         try {
            client.connect(connection);
            break;
         } catch (...) {
            std::cout << "." << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (i > 10) throw;
         }
      }
      std::cout << " connected!\n";

      std::cout << "receiving " << DATA_SIZE / 1014. / 1024. / 1024. << "GB data from the server\n";

      DoNotOptimize(testdata);
      for (auto it = testdata.begin(); it < testdata.end();) {
         auto toRead = std::min(chunkSize, static_cast<size_t>(std::distance(it, testdata.end())));
         client.read(it.base(), toRead);
         it += toRead;
      }
      ClobberMemory();

   } else { // server
      // immediately accept to not block the client
      auto server = Server(connection);
      server.accept();


      if (chunkSize < 1_m) {
         std::cout << name << ", " << chunkSize / 1024. << "KB, " << std::flush;
      } else if (chunkSize < 1_g) {
         std::cout << name << ", " << chunkSize / 1024. / 1024. << "MB, " << std::flush;
      } else {
         std::cout << name << ", " << chunkSize / 1024. / 1024. / 1024. << "GB, " << std::flush;
      }

      // cut the data into chunks
      bench(testdata.size(), [&] {
         for (auto it = testdata.begin(); it < testdata.end();) {
            auto toSend = std::min(chunkSize, static_cast<size_t>(std::distance(it, testdata.end())));
            server.write(it.base(), toSend);
            it += toSend;
         }
      }, printResults);
   }
}

// usually the server sends the data "en bloc" -> client can only read after the server finishes (buffersize) memcopy
// in this test, we do a separate measurement with fixed buffersize (e.g. 256M) and varying blocksizes
int main(int argc, char** argv) {
   if (argc < 2) {
      std::cout << "Usage: " << argv[0] << " <client / server> <(optional) 127.0.0.1>" << std::endl;
      return -1;
   }
   const auto isClient = argv[1][0] == 'c';
   const auto isLocal = [&] {
      if (argc > 2) {
         ip = argv[2];
      }
      return strcmp("127.0.0.1", ip) == 0;
   }();
   const auto connection = [&] {
      if (isClient) {
         return ip + std::string(":") + std::to_string(port);
      } else {
         return std::to_string(port);
      }
   }();

   auto testdata = TestData(isClient);

   if (not isClient) std::cout << "connection, chunk size [MB], MB transmitted, time, MB/s, user, system, total\n" << std::flush;

   for (auto i : {1_k, 2_k, 4_k, 8_k, 16_k, 32_k, 64_k, 128_k, 256_k, 512_k,
                  1_m, 2_m, 4_m, 8_m, 16_m, 32_m, 64_m, 128_m, 256_m, 512_m,
                  1_g, 2_g}) {
      if (isLocal) {
         doRun<SharedMemoryTransportServer<BUFFER_SIZE>,
               SharedMemoryTransportClient<BUFFER_SIZE>
         >("shared memory", isClient, "/tmp/testSocket", testdata, i);
         doRun<DomainSocketsTransportServer,
               DomainSocketsTransportClient
         >("domain socket", isClient, "/tmp/testSocket", testdata, i);
         doRun<TcpTransportServer,
               TcpTransportClient
         >("          tcp", isClient, connection, testdata, i);
      } else {
         doRun<RdmaTransportServer<BUFFER_SIZE>,
               RdmaTransportClient<BUFFER_SIZE>
         >("rdma", isClient, connection, testdata, i);
      }
   }
}
