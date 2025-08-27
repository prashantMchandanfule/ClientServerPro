#include "lin.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <cstring>
#include <thread>
#include <mutex>
#include <fcntl.h>
#include <ctime>
#include <cstring>
#include <sstream>
#include <errno.h> // For errno

using namespace std;
// Buffer size for receiving messages
#define BUFFER_SIZE 1024

#define MAX_EVENTS 100

mutex coutMutex; // mutex to sync the output on console . we can comment the lock code.

LinServer::LinServer() : server_fd(-1), epoll_fd(-1) {}

LinServer::~LinServer()
{
    if (server_fd != -1)
    {
        close(server_fd);
    }
    if (epollThread.joinable())
    {
        epollThread.join();
    }

    SSL_CTX_free(ctx);
    cleanup_openssl();
}

// Initialize the static thread pool using the hardware concurrency
ctpl::thread_pool LinServer::threadPool(LinServer::determineThreadPoolSize());

// Helper function to determine the number of threads based on hardware concurrency
int LinServer::determineThreadPoolSize()
{
    int concurrency = std::thread::hardware_concurrency();
    return (concurrency > 0) ? concurrency : 4; // Default to 4 if hardware concurrency is unavailable
}





bool LinServer::setSocketNonBlocking(int socketId)
{
    int flags = fcntl(socketId, F_GETFL, 0);
    if (flags == -1)
    {
        std::cerr << "Error getting socket flags!" << std::endl;
        return false;
    }

    flags |= O_NONBLOCK;
    if (fcntl(socketId, F_SETFL, flags) == -1)
    {
        std::cerr << "Error setting socket to non-blocking!" << std::endl;
        return false;
    }

    std::cout << "Socket set to non-blocking mode." << std::endl;
    return true;
}

void LinServer::handleClient(int client_socket)
{
    char buffer[BUFFER_SIZE] = {0};

    // Continue to receive data until the client closes the connection
    while (true)
    {
        // Clear the buffer before receiving new data
        memset(buffer, 0, sizeof(buffer));

        // Receive data from the client
        int valread = read(client_socket, buffer, BUFFER_SIZE);

        // If the read is less than or equal to 0, the client has closed the connection
        if (valread <= 0)
        {
            std::lock_guard<std::mutex> lock(coutMutex);
            std::cout << "Client disconnected or error occurred. Closing connection." << std::endl;
            break; // Exit the loop to close the connection
        }

        // Log the received data
        {
            std::lock_guard<std::mutex> lock(coutMutex);
            std::cout << "Received: " << buffer << std::endl;
        }

        // Prepare the HTTP response
        std::string http_response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: " +
            std::to_string(valread) + "\r\n"
                                      "\r\n" +
            std::string(buffer, valread); // Echo the received data

        // Send the HTTP response back to the client
        send(client_socket, http_response.c_str(), http_response.length(), 0);

        {
            std::lock_guard<std::mutex> lock(coutMutex);
            std::cout << "Echo response sent!" << std::endl;
        }
    }

    // Close the connection
    close(client_socket);
    {
        std::lock_guard<std::mutex> lock(coutMutex);
        std::cout << "Client connection closed." << std::endl;
    }
}






bool LinServer::initialize(int port, const std::string &ip_address)
{
    cout << "initialising the server..." << endl;
    struct sockaddr_in address;
    int opt = 1;


    // Initialize OpenSSL.
    init_openssl();
    ctx = create_ssl_context();
    configure_context();

    // Create socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
        std::cerr << "Socket creation failed!" << std::endl;
        return false;
    }

    // Set socket options to reuse address and port
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
    {
        std::cerr << "setsockopt failed!" << std::endl;
        return false;
    }

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1)
    {
        std::cerr << "Epoll creation failed!" << std::endl;
        return false;
    }

    // Set up the server address structure
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // all available ip addresses
    address.sin_port = htons(port);       // Set the port number

    // Bind the socket to the IP address and port
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        std::cerr << "Bind failed!" << std::endl;
        return false;
    }

    // Start listening for connections
    if (listen(server_fd, SOMAXCONN) < 0)
    {
        std::cerr << "Listen failed!" << std::endl;
        return false;
    }

    if(listenCallback)
        listenCallback(server_fd);

    //call the listen callback once everything is good to accept the connection : 
    std::cout << "Server initialized on " << ip_address << ":" << port << std::endl;
    return true;
}



void LinServer::start()
{
    epollThread = std::thread(&LinServer::epollLoop, this);
    struct sockaddr_in client_address;
    socklen_t addrlen = sizeof(client_address);
    char buffer[BUFFER_SIZE] = {0};
    int client_socket = -1;

    std::cout << "Waiting for connections..." << std::endl;

    while (true)
    { // Infinite loop to accept connections continuously
        // Accept an incoming connection
        if ((client_socket = accept(server_fd, (struct sockaddr *)&client_address, &addrlen)) < 0)
        {
            std::cerr << "Accept failed! Error: " << strerror(errno) << std::endl;
            continue; // Continue to the next iteration to accept new connections
        }
        //std::cout << "Connection accepted!" << std::endl;

       


        



        // Set client socket to non-blocking
        setSocketNonBlocking(client_socket);
        // Add the new client socket to the epoll set for monitoring
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLET;
        ev.data.fd = client_socket;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &ev);
        //std::cout << "New client connected and added to epoll." << std::endl;

        //also initialise the client socket state here so that we can store the ssl for this socket to use later
        clientStates[client_socket] = ClientState(client_socket); // Initialize client state
        clientStates[client_socket].ssl = ssl;

    }
}

void LinServer::epollLoop()
{
    struct epoll_event events[MAX_EVENTS];

    while (true)
    {
        int event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < event_count; i++)
        {
            int fd = events[i].data.fd;

            if (events[i].events & EPOLLIN)
            {
                // Push to thread pool for receiving data
                threadPool.push([this, fd](int thread_id)
                                { this->handleRecv(fd); });
            }
            else if (events[i].events & EPOLLOUT)
            {
                // Push to thread pool for sending data
                threadPool.push([this, fd](int thread_id)
                                { this->handleSend(fd); });
            }
            else if (events[i].events & EPOLLERR)
            {
                // Handle errors
                threadPool.push([this, fd](int thread_id)
                                { this->handleError(fd); });
            }
        }
    }
}








void LinServer::handleRecv(int client_socket)
{
    // Ensure client state is initialized the first time
    if (clientStates.find(client_socket) == clientStates.end())
    {
        clientStates[client_socket] = ClientState(client_socket); // Initialize client state
    }

    auto &state = clientStates[client_socket]; // Access the client's state
    char buffer[BUFFER_SIZE];
    //int bytesRead = recv(client_socket, buffer, BUFFER_SIZE, 0); //this call was for simple http request
    int bytesRead = SSL_read(state.ssl, buffer, BUFFER_SIZE);

    if (bytesRead > 0)
    {
        // std::cout << "Received data from client: " << std::string(buffer, bytesRead) << std::endl;

        state.recvBuffer.append(buffer, bytesRead);

        // Check if the request is complete (e.g., HTTP would check for \r\n\r\n or content length)
        if (isRequestComplete(state.client_socket))
        {
            // Once the entire request is received, send it to the upper layer through callback
            //we should think whether we want to call the callback in same thread or 
            //should we push it to the thread pool
            receiveCallback(state);
        }
        else
        {
            // If the request is incomplete, mark that we're still waiting for more data
           // state.waitingForRecv = true;
        }
    }
    else if (bytesRead == 0)
    {
        std::cout << "Client disconnected." << std::endl;
        close(client_socket);
        clientStates.erase(client_socket); // Clean up state
    }
    else
    {
        if (errno != EWOULDBLOCK && errno != EAGAIN)
        {
            std::cerr << "Error in recv: " << strerror(errno) << std::endl;
            handleError(client_socket);
        }
        else
        {
            // more data to recv
            return;
        }
    }
}

void LinServer::handleSend(int client_socket)
{
    auto &state = clientStates[client_socket]; // access client state

    if (state.bytesSent < state.sendBuffer.size())
    {
        // Calculate how much more data needs to be sent
        int bytesToSend = state.sendBuffer.size() - state.bytesSent;
        //int bytesSent = send(client_socket, state.sendBuffer.c_str() + state.bytesSent, bytesToSend, 0); this api was used for simple http request
        int bytesSent = SSL_write(state.ssl, state.sendBuffer.c_str() + state.bytesSent, bytesToSend);

        if (bytesSent > 0)
        {
            state.bytesSent += bytesSent; // Update the number of bytes sent so far
            if (state.bytesSent == state.sendBuffer.size())
            {
                // If all data has been sent, reset the state
                std::cout << "All data sent to client." << std::endl;

                //lets clear the state and close the socket . making http as stateless protocol. remove from the states map
                state.clear();

                // close(client_socket);
                // clientStates.erase(client_socket); // Clean up state
            }
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // wait for the next opportunity to send
                return;
            }
            else
            {
                // An error occurred, handle it
                std::cerr << "Error in send." << std::endl;
                handleError(client_socket);
            }
        }
    }
}

// Prepare the response data and initiate sending process
void LinServer::SendResponse(ClientState& client_state)
{
    handleSend(client_state.client_socket); // Start sending the data
}

void LinServer::handleError(int client_socket)
{
    auto &state = clientStates[client_socket]; 
    std::cerr << "Socket error, closing connection." << std::endl;
    SSL_shutdown(state.ssl);
    SSL_free(state.ssl);
    close(client_socket);
    clientStates.erase(client_socket); // Clean up state
}