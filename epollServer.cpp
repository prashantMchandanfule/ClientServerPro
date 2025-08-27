#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <csignal>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "ctpl_stl.h"

#define MAX_EVENTS 1000
#define BUFFER_SIZE 1024

// ---------------- Client State ----------------
struct ClientState {
    int socket_fd;
    std::string recvBuffer;
    std::string sendBuffer;
    size_t bytesSent;
    ClientState(int fd = -1) : socket_fd(fd), bytesSent(0) {}
};

// ---------------- Server Class ----------------
class EpollThreadPoolServer {
public:
    using Callback = std::function<void(ClientState&)>;

    EpollThreadPoolServer(int port, int numThreads);
    ~EpollThreadPoolServer();
    void start();
    void stop();
    void onMessage(const Callback& cb);
    void onDisconnect(const Callback& cb);

private:
    int port;
    int server_fd;
    int epoll_fd;
    std::atomic<bool> running;
    ctpl::thread_pool threadPool;
    std::unordered_map<int, ClientState> clientStates;
    std::mutex clientsMutex;

    Callback messageCallback;
    Callback disconnectCallback;

    void setupServerSocket();      // create/bind/listen socket
    void acceptClient();           // accept new connection
    void handleRecv(int client_socket); // handle read
    void handleSend(int client_socket); // handle write
    void handleError(int client_socket); // handle disconnect/error
};

// ---------------- Implementation ----------------
EpollThreadPoolServer::EpollThreadPoolServer(int port, int numThreads)
    : port(port), server_fd(-1), epoll_fd(-1), running(false), threadPool(numThreads) {}

EpollThreadPoolServer::~EpollThreadPoolServer() {
    stop();
}

void EpollThreadPoolServer::setupServerSocket() {
    // Create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) throw std::runtime_error("Socket creation failed");

    // Reuse address
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind socket
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0)
        throw std::runtime_error("Bind failed");

    // Listen for connections
    if (listen(server_fd, SOMAXCONN) < 0)
        throw std::runtime_error("Listen failed");

    // Create epoll instance
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) throw std::runtime_error("epoll_create1 failed");

    // Add server socket to epoll
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);
}

void EpollThreadPoolServer::start() {
    setupServerSocket();
    running = true;

    std::cout << "Server started on port " << port << std::endl;

    epoll_event events[MAX_EVENTS];

    // Main epoll loop
    while (running) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        // Dispatch events
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            if (fd == server_fd) {
                acceptClient();
            } else if (events[i].events & EPOLLIN) {
                threadPool.push([this, fd](int) { handleRecv(fd); });
            } else if (events[i].events & EPOLLOUT) {
                threadPool.push([this, fd](int) { handleSend(fd); });
            } else {
                threadPool.push([this, fd](int) { handleError(fd); });
            }
        }
    }
}

void EpollThreadPoolServer::stop() {
    running = false;
    if (server_fd != -1) close(server_fd);
    if (epoll_fd != -1) close(epoll_fd);

    std::lock_guard<std::mutex> lock(clientsMutex);
    for (auto& [fd, state] : clientStates) {
        close(fd);
    }
    clientStates.clear();
    std::cout << "Server stopped." << std::endl;
}

void EpollThreadPoolServer::acceptClient() {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) return;

    // Add client socket to epoll
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = client_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);

    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        clientStates[client_fd] = ClientState(client_fd);
    }

    std::cout << "New client connected: FD=" << client_fd << std::endl;
}

void EpollThreadPoolServer::handleRecv(int client_socket) {
    char buffer[BUFFER_SIZE];
    int bytesRead = recv(client_socket, buffer, BUFFER_SIZE, 0);

    if (bytesRead <= 0) {
        handleError(client_socket);
        return;
    }

    std::lock_guard<std::mutex> lock(clientsMutex);
    auto& state = clientStates[client_socket];
    state.recvBuffer.append(buffer, bytesRead);

    if (messageCallback) {
        messageCallback(state);
    }
}

void EpollThreadPoolServer::handleSend(int client_socket) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    auto& state = clientStates[client_socket];

    if (state.bytesSent < state.sendBuffer.size()) {
        int bytesToSend = state.sendBuffer.size() - state.bytesSent;
        int bytesSent = send(client_socket,
                             state.sendBuffer.c_str() + state.bytesSent,
                             bytesToSend, 0);
        if (bytesSent <= 0) {
            handleError(client_socket);
            return;
        }
        state.bytesSent += bytesSent;
    }

    // All data sent, remove EPOLLOUT interest
    if (state.bytesSent >= state.sendBuffer.size()) {
        state.sendBuffer.clear();
        state.bytesSent = 0;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = client_socket;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_socket, &ev);
    }
}

void EpollThreadPoolServer::handleError(int client_socket) {
    std::cout << "Client disconnected: FD=" << client_socket << std::endl;

    if (disconnectCallback) {
        disconnectCallback(clientStates[client_socket]);
    }

    close(client_socket);
    std::lock_guard<std::mutex> lock(clientsMutex);
    clientStates.erase(client_socket);
}

void EpollThreadPoolServer::onMessage(const Callback& cb) {
    messageCallback = cb;
}

void EpollThreadPoolServer::onDisconnect(const Callback& cb) {
    disconnectCallback = cb;
}

// ---------------- Main ----------------
int main() {
    EpollThreadPoolServer server(9090, 4);

    server.onMessage([&](ClientState& state) {
        std::string msg = state.recvBuffer;
        std::cout << "Received from client FD " << state.socket_fd << ": " << msg << std::endl;

        // echo back plain message
        state.sendBuffer = "Echo: " + msg;

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.fd = state.socket_fd;
        epoll_ctl(server.epoll_fd, EPOLL_CTL_MOD, state.socket_fd, &ev);

        state.recvBuffer.clear();
    });

    server.onDisconnect([&](ClientState& state) {
        std::cout << "Cleaning up client FD " << state.socket_fd << std::endl;
    });

    server.start();
    return 0;
}
