#define WIN32_LEAN_AND_MEAN
#include <iostream>
#include <string>
#include <map>
#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstring>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <functional>
#include <windows.h>

#ifdef _WIN32
    #define _WINSOCK_DEPRECATED_NO_WARNINGS
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define closesocket close
#endif

// ==================== 加密模块 ====================
std::string xorEncryptDecrypt(const std::string& input, char key = 0x5A) {
    std::string output = input;
    for (size_t i = 0; i < output.size(); ++i) {
        output[i] = input[i] ^ key;
    }
    return output;
}

// ==================== 通信协议 ====================
#pragma pack(push, 1)
struct MessageHeader {
    uint16_t magic;      // 0xAA55
    uint8_t type;        // 1:登录 2:心跳 3:单聊 4:群发 5:登出
    uint8_t status;      // 0:成功 1:失败
    uint16_t sender_len;
    uint16_t target_len;
    uint32_t payload_len;
};
#pragma pack(pop)

struct Message {
    MessageHeader header;
    std::string sender_id;
    std::string target_id;
    std::string payload;

    std::vector<char> serialize() {
    // 构造完整负载：发送者ID + 接收者ID + 内容
    std::string combined = sender_id + target_id + payload;
    header.magic = 0xAA55;
    header.sender_len = (uint16_t)sender_id.size();
    header.target_len = (uint16_t)target_id.size();
    header.payload_len = (uint32_t)combined.size();

    std::vector<char> buffer(sizeof(MessageHeader) + combined.size());
    // 复制头部
    memcpy(buffer.data(), &header, sizeof(MessageHeader));
    // 复制负载数据
    memcpy(buffer.data() + sizeof(MessageHeader), combined.data(), combined.size());
    return buffer;
}

bool deserialize(const char* data, int len) {
    if (len < (int)sizeof(MessageHeader)) return false;
    memcpy(&header, data, sizeof(MessageHeader));
    if (header.magic != 0xAA55) return false;

    int body_len = (int)header.payload_len;
    if (len < (int)sizeof(MessageHeader) + body_len) return false;

    const char* body_start = data + sizeof(MessageHeader);
    // 直接从内存构造 std::string，不依赖 '\0'
    std::string body(body_start, body_len);
    size_t pos = 0;
    sender_id = body.substr(pos, header.sender_len);
    pos += header.sender_len;
    target_id = body.substr(pos, header.target_len);
    pos += header.target_len;
    payload = body.substr(pos);
    return true;
}
};

// ==================== 线程池 ====================
class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop;
public:
    ThreadPool(size_t threads) : stop(false) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] {
                            return this->stop.load() || !this->tasks.empty();
                        });
                        if (this->stop.load() && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
        }
    }
    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }
    ~ThreadPool() {
        stop.store(true);
        condition.notify_all();
        for (std::thread &worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }
};

// ==================== 服务器主类 ====================
class ChatServer {
private:
    SOCKET listen_fd;
    std::atomic<bool> is_running;
    std::map<SOCKET, std::string> online_clients;
    std::map<std::string, SOCKET> online_users;
    std::mutex clients_mutex;
    ThreadPool pool;
    int port;

    bool sendToClient(SOCKET fd, const Message& msg) {
        std::string encrypted_payload = msg.payload;
        Message send_msg = msg;
        send_msg.payload = encrypted_payload;
        auto buffer = send_msg.serialize();
        int ret = send(fd, buffer.data(), (int)buffer.size(), 0);
        return ret != SOCKET_ERROR;
    }

    void broadcastStatus(const std::string& username, bool online) {
        std::lock_guard<std::mutex> lock(clients_mutex);
        Message notify;
        notify.header.type = 3;
        notify.sender_id = "System";
        notify.target_id = "All";
        std::string status = online ? "上线了" : "下线了";
        notify.payload = username + " " + status;

        for (auto& pair : online_clients) {
            if (pair.second != username) {
                sendToClient(pair.first, notify);
            }
        }
    }

public:
    ChatServer(int p) : port(p), pool(8), is_running(false) {}

    bool start() {
        #ifdef _WIN32
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
                std::cerr << "WSAStartup failed" << std::endl;
                return false;
            }
        #endif

        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd == INVALID_SOCKET) {
            std::cerr << "Socket creation failed" << std::endl;
            return false;
        }

        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            std::cerr << "Bind failed on port " << port << std::endl;
            return false;
        }

        if (listen(listen_fd, SOMAXCONN) == SOCKET_ERROR) {
            std::cerr << "Listen failed" << std::endl;
            return false;
        }

        is_running = true;
        std::cout << "[服务器] 启动成功，监听端口: " << port << std::endl;
        std::cout << "[服务器] 多线程线程池已初始化 (8个工作线程)" << std::endl;

        while (is_running) {
            sockaddr_in client_addr;
            socklen_t len = sizeof(client_addr);
            SOCKET client_fd = accept(listen_fd, (sockaddr*)&client_addr, &len);
            if (client_fd == INVALID_SOCKET) continue;

            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(client_addr.sin_addr), ip, INET_ADDRSTRLEN);
            std::cout << "[服务器] 新连接来自: " << ip << ":" << ntohs(client_addr.sin_port) << std::endl;

            pool.enqueue([this, client_fd] {
                this->handleClient(client_fd);
            });
        }

        closesocket(listen_fd);
        #ifdef _WIN32
            WSACleanup();
        #endif
        return true;
    }

    void stop() {
        is_running = false;
    }

private:
    void handleClient(SOCKET client_fd) {
        char buffer[4096];
        std::string username;
        bool logged_in = false;
        auto last_heartbeat = std::chrono::steady_clock::now();

        while (is_running) {
            auto now = std::chrono::steady_clock::now();
            if (logged_in && std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat).count() > 90) {
                std::cout << "[服务器] 用户 " << username << " 心跳超时，断开连接" << std::endl;
                break;
            }

            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(client_fd, &read_set);
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            int select_ret = select(0, &read_set, NULL, NULL, &tv);
            if (select_ret <= 0) continue;

            int recv_len = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
            if (recv_len <= 0) {
                std::cout << "[服务器] 客户端 " << client_fd << " 断开连接" << std::endl;
                break;
            }

            Message msg;
            if (!msg.deserialize(buffer, recv_len)) {
                std::cerr << "[服务器] 收到无效数据包" << std::endl;
                continue;
            }

            // 处理登录
            if (msg.header.type == 1 && !logged_in) {
                username = msg.sender_id;
                if (msg.payload == "123456") {
                    logged_in = true;
                    {
                        std::lock_guard<std::mutex> lock(clients_mutex);
                        online_clients[client_fd] = username;
                        online_users[username] = client_fd;
                    }
                    Message reply;
                    reply.header.type = 1;
                    reply.header.status = 0;
                    reply.sender_id = "System";
                    reply.target_id = username;
                    reply.payload = "登录成功！";
                    sendToClient(client_fd, reply);

                    std::cout << "[服务器] 用户 " << username << " 登录成功 (在线人数: " << online_clients.size() << ")" << std::endl;
                    broadcastStatus(username, true);
                    last_heartbeat = std::chrono::steady_clock::now();
                } else {
                    Message reply;
                    reply.header.type = 1;
                    reply.header.status = 1;
                    reply.sender_id = "System";
                    reply.target_id = username;
                    reply.payload = "密码错误！";
                    sendToClient(client_fd, reply);
                    closesocket(client_fd);
                    return;
                }
                continue;
            }

            if (!logged_in) {
                closesocket(client_fd);
                return;
            }

            // 心跳
            if (msg.header.type == 2) {
                last_heartbeat = std::chrono::steady_clock::now();
                continue;
            }

            // 登出
            if (msg.header.type == 5) {
                std::cout << "[服务器] 用户 " << username << " 主动登出" << std::endl;
                break;
            }

            // 消息转发
            if (msg.header.type == 3 || msg.header.type == 4) {
                std::string decrypted_payload = msg.payload;
                std::cout << "[服务器] 转发消息: " << msg.sender_id << " -> " << msg.target_id
                          << " 内容: " << decrypted_payload.substr(0, 30) << "..." << std::endl;

                if (msg.header.type == 4) {
                    std::lock_guard<std::mutex> lock(clients_mutex);
                    for (auto& pair : online_clients) {
                        if (pair.second != msg.sender_id) {
                            Message fwd = msg;
                            fwd.payload = decrypted_payload;
                            sendToClient(pair.first, fwd);
                        }
                    }
                } else {
                    SOCKET target_fd;
                    {
                        std::lock_guard<std::mutex> lock(clients_mutex);
                        auto it = online_users.find(msg.target_id);
                        if (it == online_users.end()) {
                            Message reply;
                            reply.header.type = 3;
                            reply.header.status = 1;
                            reply.sender_id = "System";
                            reply.target_id = msg.sender_id;
                            reply.payload = "目标用户不在线";
                            sendToClient(client_fd, reply);
                            continue;
                        }
                        target_fd = it->second;
                    }
                    Message fwd = msg;
                    fwd.payload = decrypted_payload;
                    sendToClient(target_fd, fwd);
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (logged_in) {
                online_clients.erase(client_fd);
                online_users.erase(username);
                broadcastStatus(username, false);
            }
        }
        closesocket(client_fd);
        std::cout << "[服务器] 当前在线人数: " << online_clients.size() << std::endl;
    }
};

// ==================== 主函数 ====================
int main(int argc, char* argv[]) {
    #ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
    #endif
    int port = 8888;
    if (argc >= 2) port = atoi(argv[1]);

    std::cout << "========================================" << std::endl;
    std::cout << "  基于多线程技术的网络聊天系统 - 服务端" << std::endl;
    std::cout << "  广东财经大学 双百工程项目" << std::endl;
    std::cout << "========================================" << std::endl;

    ChatServer server(port);
    server.start();

    return 0;
}