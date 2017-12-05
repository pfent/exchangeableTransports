#ifndef EXCHANGABLETRANSPORTS_BENCH_H
#define EXCHANGABLETRANSPORTS_BENCH_H

#include <tuple>
#include <ios>
#include <fstream>
#include <vector>
#include <iterator>
#include <chrono>

double getGlobalStat() {
    auto globalStat = std::ifstream("/proc/stat", std::ios::in);
    if (not globalStat.is_open()) {
        throw std::runtime_error{"couldn't open /proc/stat"};
    }

    std::vector<std::string> ret;
    std::copy(std::istream_iterator<std::string>(globalStat),
              std::istream_iterator<std::string>(),
              std::back_inserter(ret));

    double user = stod(ret[1]), nice = stod(ret[2]), system = stod(ret[3]), idle = stod(ret[4]);
    return user + nice + system + idle;
}

std::tuple<double, double> getOwnStat() {
    auto ownStat = std::ifstream("/proc/self/stat");
    if (not ownStat.is_open()) {
        throw std::runtime_error{"couldn't open /proc/stat"};
    }

    std::vector<std::string> ret;
    std::copy(std::istream_iterator<std::string>(ownStat),
              std::istream_iterator<std::string>(),
              std::back_inserter(ret));

    double utime = stod(ret[13]), stime = stod(ret[14]);
    return {utime, stime};
}

template<typename T>
auto bench(size_t workSize, T &&fun, size_t repetitions = 1) {
    std::vector<std::tuple<double, double, double>> cpuLoads;
    std::vector<double> times;

    for (size_t i = 0; i < repetitions; ++i) {
        const auto before = getGlobalStat();
        const auto
        [ownUserBefore, ownSystemBefore] = getOwnStat();

        const auto start = std::chrono::steady_clock::now();
        for (size_t j = 0; j < workSize; ++j) {
            fun();
        }
        const auto end = std::chrono::steady_clock::now();

        const auto after = getGlobalStat();
        const auto
        [ownUserAfter, ownSystemAfter] = getOwnStat();

        const auto diff = (after - before);
        const auto userPercent = (ownUserAfter - ownUserBefore) / diff * 100.0;
        const auto systemPercent = (ownSystemAfter - ownSystemBefore) / diff * 100.0;
        const auto totalPercent = userPercent + systemPercent;

        const auto sTaken = std::chrono::duration<double>(end - start).count();

        cpuLoads.emplace_back(userPercent, systemPercent, totalPercent);
        times.emplace_back(sTaken);
    }

    const auto
    [userPercent, systemPercent, totalPercent] = [&]() {
        double userTotal = 0, systemTotal = 0, totalTotal = 0;
        for (auto[u, s, t] : cpuLoads) {
            userTotal += u;
            systemTotal += s;
            totalTotal += t;
        }
        return std::make_tuple(userTotal / repetitions, systemTotal / repetitions, totalTotal / repetitions);
    }();

    const auto avgTime = [&]() {
        double total = 0;
        for (auto time : times) {
            total += time;
        }
        return total / repetitions;
    }();

    std::cout << workSize << ", "
              << avgTime << ", "
              << (workSize / avgTime) << ", "
              << userPercent << ", "
              << systemPercent << ", "
              << totalPercent << '\n';
}

#endif //EXCHANGABLETRANSPORTS_BENCH_H
