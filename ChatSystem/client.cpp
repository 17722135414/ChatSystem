#define WIN32_LEAN_AND_MEAN
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <vector>
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

// 加密函数（同服务端）
std::string xorEncryptDecrypt(const std::string& input, char key = 0x5A) {
    std::string output = input;
    for (size_t i = 0; i < output.size(); ++i) {
        output[i] = input[i] ^ key;
    }
    return output;
}

#pragma pack(push, 1)
struct MessageHeader {
    uint16_t magic;
    uint8_t type;
    uint8_t status;
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
        header.magic = 0xAA55;
        header.sender_len = (uint16_t)sender_id.size();
        header.target_len = (uint16_t)target_id.size();
        std::string combined = sender_id + target_id + payload;
        header.payload_len = (uint32_t)combined.size();

        std::vector<char> buffer(sizeof(MessageHeader) + combined.size());
        memcpy(buffer.data(), &header, sizeof(MessageHeader));
        memcpy(buffer.data() + sizeof(MessageHeader), combined.c_str(), combined.size());
        return buffer;
    }

    bool deserialize(const char* data, int len) {
        if (len < sizeof(MessageHeader)) return false;
        memcpy(&header, data, sizeof(MessageHeader));
        if (header.magic != 0xAA55) return false;
        int body_len = header.payload_len;
        if (len < sizeof(MessageHeader) + body_len) return false;
        std::string body(data + sizeof(MessageHeader), body_len);
        size_t pos = 0;
        sender_id = body.substr(pos, header.sender_len);
        pos += header.sender_len;
        target_id = body.substr(pos, header.target_len);
        pos += header.target_len;
        payload = body.substr(pos);
        return true;
    }
};

class ChatClient {
private:
    SOCKET sock_fd;
    std::string username;
    std::atomic<bool> is_connected;
    std::thread recv_thread;
    std::thread heartbeat_thread;

    void sendMessage(const Message& msg) {
        Message send_msg = msg;
        send_msg.payload = msg.payload;
        auto buffer = send_msg.serialize();
        send(sock_fd, buffer.data(), (int)buffer.size(), 0);
    }

    void receiveLoop() {
        char buffer[4096];
        while (is_connected) {
            int recv_len = recv(sock_fd, buffer, sizeof(buffer) - 1, 0);
            if (recv_len <= 0) {
                std::cout << "\n[系统] 与服务器断开连接" << std::endl;
                is_connected = false;
                break;
            }
            Message msg;
            if (!msg.deserialize(buffer, recv_len)) continue;
            

            // 根据 header.type 处理
            if (msg.header.type == 1) {
                if (msg.header.status == 0) {
                    std::cout << "\n[系统] " << msg.payload << std::endl;
                } else {
                    std::cout << "\n[系统] 登录失败: " << msg.payload << std::endl;
                    is_connected = false;
                }
            } else if (msg.header.type == 3) {
    		    // 清理接收到的 ID 中可能存在的不可见字符
    		    std::string sender = msg.sender_id;
  		    std::string target = msg.target_id;
  		    std::cout << "\n[" << sender << " -> " << target << "] "
           			    << msg.payload << std::endl;
		} else {
                std::cout << "\n[系统消息] " << msg.payload << std::endl;
                std::cout << ">> ";
                std::cout.flush();
            }
        }
    }

    void heartbeatLoop() {
        while (is_connected) {
            std::this_thread::sleep_for(std::chrono::seconds(25));
            if (!is_connected) break;
            Message heartbeat;
            heartbeat.header.type = 2;
            heartbeat.sender_id = username;
            heartbeat.target_id = "Server";
            heartbeat.payload = "ping";
            sendMessage(heartbeat);
        }
    }

public:
    ChatClient() : sock_fd(INVALID_SOCKET), is_connected(false) {}

    ~ChatClient() {
        disconnect();
    }

    bool connectTo(const std::string& ip, int port) {
        #ifdef _WIN32
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) return false;
        #endif

        sock_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd == INVALID_SOCKET) return false;

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        if (::connect(sock_fd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            std::cerr << "连接失败，请检查服务器是否启动" << std::endl;
            return false;
        }

        is_connected = true;
        std::cout << "[系统] 连接服务器成功" << std::endl;
        return true;
    }

bool login(const std::string& user, const std::string& pass) {
    username = user;
    Message login_msg;
    login_msg.header.type = 1;
    login_msg.sender_id = user;
    login_msg.target_id = "Server";
    login_msg.payload = pass;

    auto buffer = login_msg.serialize();
    send(sock_fd, buffer.data(), (int)buffer.size(), 0);

    recv_thread = std::thread(&ChatClient::receiveLoop, this);
    heartbeat_thread = std::thread(&ChatClient::heartbeatLoop, this);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return is_connected;
}

void sendText(const std::string& target, const std::string& content) {
    if (!is_connected) {
        std::cout << "[系统] 未连接到服务器" << std::endl;
        return;
    }

    // 手动构造干净的目标字符串（仅保留可见字符，去除所有空白和不可见字符）
    std::string clean_target;
    for (char c : target) {
        if (c >= 32 && c <= 126) { // 只保留 ASCII 可见字符（字母数字和标点）
            clean_target += c;
        }
    }
    // 如果 clean_target 为空，则无法发送
    if (clean_target.empty()) {
        std::cout << "[系统] 目标用户名无效" << std::endl;
        return;
    }

    Message msg;
    msg.header.type = 3;
    msg.sender_id = username;
    msg.target_id = clean_target;  // 使用清理后的
    msg.payload = content;

    auto buffer = msg.serialize();
    send(sock_fd, buffer.data(), (int)buffer.size(), 0);
}

    void broadcastText(const std::string& content) {
        if (!is_connected) return;
        Message msg;
        msg.header.type = 4;
        msg.sender_id = username;
        msg.target_id = "All";
        msg.payload = content;
        sendMessage(msg);
    }

    void disconnect() {
        if (is_connected) {
            Message logout;
            logout.header.type = 5;
            logout.sender_id = username;
            logout.target_id = "Server";
            logout.payload = "bye";
            sendMessage(logout);
            is_connected = false;
        }
        if (sock_fd != INVALID_SOCKET) {
            closesocket(sock_fd);
            sock_fd = INVALID_SOCKET;
        }
        if (recv_thread.joinable()) recv_thread.join();
        if (heartbeat_thread.joinable()) heartbeat_thread.join();
        #ifdef _WIN32
            WSACleanup();
        #endif
    }
};

int main(int argc, char* argv[]) {
    #ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
    #endif
    #ifdef _WIN32
        system("chcp 65001 > nul");
    #endif
    std::string server_ip = "127.0.0.1";
    int port = 8888;
    if (argc >= 3) {
        server_ip = argv[1];
        port = atoi(argv[2]);
    }

    std::cout << "========================================" << std::endl;
    std::cout << "  基于多线程技术的网络聊天系统 - 客户端" << std::endl;
    std::cout << "  广东财经大学 双百工程项目" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "服务器地址: " << server_ip << ":" << port << std::endl;

    ChatClient client;
    if (!client.connectTo(server_ip, port)) {
        std::cout << "按回车键退出..." << std::endl;
        std::cin.get();
        return -1;
    }

    std::string username, password;
    while (true) {
           std::cout << "请输入用户名: ";
           std::getline(std::cin, username);
	   if ( !username.empty()) break;
    }
    while (true) {
           std::cout << "请输入密码 (默认123456): ";
           std::getline(std::cin, password);
           if ( !password.empty()) break;
    }

    if (!client.login(username, password)) {
        std::cout << "登录失败，按回车退出..." << std::endl;
        std::cin.get();
        return -1;
    }

    std::cout << "\n========== 聊天帮助 ==========" << std::endl;
    std::cout << "私聊: @用户名 消息内容" << std::endl;
    std::cout << "群聊: # 消息内容" << std::endl;
    std::cout << "退出: exit" << std::endl;
    std::cout << "===============================" << std::endl;
    std::cout << ">> ";

    std::string input;
    while (true) {
        std::getline(std::cin, input);
        if (input == "exit") break;
        if (input.empty()) {
            std::cout << ">> ";
            continue;
        }

        if (input[0] == '@') {
            size_t space_pos = input.find(' ');
            if (space_pos == std::string::npos) {
                std::cout << "格式错误: @用户名 消息" << std::endl;
            } else {
                std::string target = input.substr(1, space_pos - 1);
                std::string content = input.substr(space_pos + 1);
                client.sendText(target, content);
            }
        } else if (input[0] == '#') {
            std::string content = input.substr(1);
            if (!content.empty()) client.broadcastText(content);
        } else {
            std::cout << "未知命令，请使用 @用户名 消息 或 # 消息" << std::endl;
        }
        std::cout << ">> ";
    }

    client.disconnect();
    std::cout << "[系统] 已退出" << std::endl;
    return 0;
}