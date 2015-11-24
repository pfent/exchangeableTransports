//---------------------------------------------------------------------------
// (c) 2015 Wolf Roediger <roediger@in.tum.de>
// Technische Universitaet Muenchen
// Institut fuer Informatik, Lehrstuhl III
// Boltzmannstr. 3
// 85748 Garching
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//---------------------------------------------------------------------------
#include "rdma/Network.hpp"
#include "rdma/MemoryRegion.hpp"
#include "rdma/WorkRequest.hpp"
#include "util/ConnectionSetup.hpp"
#include "util/Coordinator.hpp"
#include "util/Utility.hpp"
//---------------------------------------------------------------------------
#include <iomanip>
#include <iostream>
#include <memory>
#include <algorithm>
#include <cassert>
#include <unistd.h>
#include <zmq.hpp>
#include <thread>
#include <sstream>
#include <util/Coordinator.hpp>
//---------------------------------------------------------------------------
using namespace std;
using namespace rdma;
//---------------------------------------------------------------------------
namespace {
uint32_t getNodeCount(int argc, char **argv)
{
   if (argc != 2) {
      cerr << "usage: " << argv[0] << " [nodeCount]" << endl;
      exit(EXIT_FAILURE);
   }
   uint32_t nodeCount;
   istringstream in(argv[1]);
   in >> nodeCount;
   return nodeCount;
}
}
//---------------------------------------------------------------------------
int main(int argc, char **argv)
{
   uint32_t nodeCount = getNodeCount(argc, argv);
   zmq::context_t context(1);
   util::Coordinator coordinator(context, nodeCount, util::getHostname());
   cout << "> start supportBarrier" << endl;
   coordinator.supportBarrier(); // async

   while (1) {
      cout << "> supportHostnameExchange" << endl;
      coordinator.supportHostnameExchange();
   }
}
//---------------------------------------------------------------------------
