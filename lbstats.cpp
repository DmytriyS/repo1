/*
 * File:   main.cpp
 * Author: dmytros
 *
 * Created on June 4, 2015, 9:31 AM
 */

#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <netdb.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <thread>
#include <future>
#include <unistd.h>

template <typename... ARGS>
void log(const char* fmt, ARGS... args) {
    std::printf(fmt, args...);
    std::cout << std::endl;
}

template<>
void log<>(const char* m) {
    std::cout << m;
    std::cout << std::endl;
}

void man() {
    log("\nusage: lbstats [HOST:]PORT PACKETSIZE STATSPERIOD");
}

inline long time_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

int openConnection(const std::string& host, const std::string& port) {
    constexpr int r_error = -1;

    log("opening connection to host = %s, port = %s", host.c_str(), port.c_str());

    addrinfo* addrInfo = NULL;
    addrinfo hints{};
    bzero(&hints, sizeof (hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;

    sockaddr* addr = NULL;
    socklen_t slen = 0;

    int e = 0;
    if ((e = getaddrinfo(host.c_str(), port.c_str(), &hints, &addrInfo)) == 0) {
        if (addrInfo && addrInfo->ai_addr) {
            addr = addrInfo->ai_addr;
            addr->sa_family = AF_INET;
            slen = addrInfo->ai_addrlen;
        } else {
            log("error: getaddrinfo()");
            return r_error;
        }
    } else {
        log("error: getaddrinfo(): %s", gai_strerror(e));
        return r_error;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        log("error: socket(): %1%", strerror(errno));
        return r_error;
    }

    if (connect(sockfd, addr, slen) < 0) {
        log("error: connect(): %s", strerror(errno));
        return r_error;
    }

    return sockfd;
}

struct Task {
private:
    std::vector<char> storage;

    virtual void runImpl() = 0;

public:

    Task(int sockfd, int packetSize, int statsPeriod)
    : sockfd(sockfd), bufSize(packetSize), statsPeriod(statsPeriod) {

        storage = std::vector<char>(bufSize);
        buffer = storage.data();
    }

    static void run(Task& t) {
        t.runImpl();
    }

protected:
    const int sockfd;
    const int bufSize;
    const int statsPeriod;

    char* buffer = nullptr;
};

struct Writer : Task {

    Writer(int sockfd, int packetSize, int statsPeriod) : Task(sockfd, packetSize, statsPeriod) {
    }

private:

    void runImpl() {
        long* lb = reinterpret_cast<long*> (buffer);

        while (true) {
            ::usleep(100);
            *lb = time_ns();
            auto writeN = ::send(sockfd, buffer, bufSize, 0);
            if (writeN < 0) {
                log("error: send() %s", strerror(errno));
                break;
            } else if (writeN != bufSize) {
                log("error: send() written %d bytes", writeN);
                break;
            }
        }
        log("writer has stopped");
    };
};

struct Reader : Task {

    Reader(int sockfd, int packetSize, int statsPeriod) : Task(sockfd, packetSize, statsPeriod) {
    }

private:

    void runImpl() {
        constexpr long nsecinms = 1000000;
        constexpr long nsecinus = 1000;

        long* lb = reinterpret_cast<long*> (buffer);

        while (true) {
            auto readN = ::recv(sockfd, buffer, bufSize, MSG_WAITALL);
            if (readN > 0) {
                if (bufSize != readN) {
                    log("error: revc() read %d bytes", readN);
                    break;
                }
                if (packetsN == statsPeriod) {
                    log("packet average lifespan: %10ld us", ((totalNs / packetsN) / nsecinus));
                    packetsN = 0;
                    totalNs = 0;
                }
                ++packetsN;
                long send_ns = *lb;
                totalNs += time_ns() - send_ns;

            } else if (readN < 0) {
                log("error: recv() %s", strerror((errno)));
                break;
            } else {
                log("warn: no data. Connection closed?");
                break;
            }
        }
        log("reader has stopped");
    }

    int packetsN = 0;
    long totalNs = 0;
};

/*
 * args: [host:]port, packet size, stats period (in packets)
 *
 */
int main(int argc, char** argv) {

    static_assert(sizeof (long) == 8, "");

    enum ARGS {
        AddrPos = 1,
        PacketSizePos = 2,
        StatsPeriodPos = 3,
        MaxArgs
    };

    if (argc < MaxArgs) {
        log("error: too few arguments");
        man();
        return 1;
    }

    const std::string addr = argv[AddrPos];
    const int packetSize = std::atoi(argv[PacketSizePos]);
    const int statsPeriod = std::atoi(argv[StatsPeriodPos]);

    std::string host;
    std::string port;

    size_t colonPos = addr.find(':');
    if (colonPos == std::string::npos) {
        host = "localhost";
        port = addr;
    } else {
        host = addr.substr(0, colonPos);
        port = addr.substr(colonPos + 1);
    }

    if (packetSize < sizeof (long)) {
        log("error: packetSize must be greater than 8");
        return 2;
    }

    log("arguments: host = %s, port = %s, packetSize = %d, statsPeriod = %d", host.c_str(), port.c_str(), packetSize, statsPeriod);

    int sockfd = openConnection(host, port);

    if (sockfd < 0) {
        log("failed to open connection");
        return 3;
    }

    log("connected to %s", addr.c_str());

    Writer w{sockfd, packetSize, statsPeriod};
    Reader r{sockfd, packetSize, statsPeriod};

    log("starting writer");
    std::thread tw{Task::run, std::ref(w)};

    log("starting reader");
    std::thread tr{Task::run, std::ref(r)};

    tw.join();
    tr.join();

    log("finished");

    return 0;
}

