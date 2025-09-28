#include "TCPCommunication.hpp"
#include <iostream>
#include <chrono>
#include <thread>

int main() {
    try {
        TCPCommunication server(TCPCommunication::Mode::Server);

        server.setOnConnect([](bool success) {
            if (success) {
                std::cout << "Server: Client connected.\n";
            }
        });

        server.setOnDisconnect([]() {
            std::cout << "Server: Client disconnected.\n";
        });

        server.setOnRead([&server](std::vector<char>& data) {
            std::string message(data.begin(), data.end());
            std::cout << "Server: Received: " << message << std::endl;

            // 回显消息到所有连接的客户端
            std::string response = "Server received: " + message;
            server.write(response);
        });

        server.setOnError([](const boost::system::error_code& ec, const std::string& msg) {
            std::cerr << "Server Error: " << msg << " (" << ec.message() << ")\n";
        });

        server.setTimeout(5000); // 5秒不活动则超时
        server.connect("0.0.0.0", 12345);
        std::cout << "Server listening on port 12345...\n";

        // 让服务器运行，直到用户输入
        std::cout << "Press Enter to exit.\n";
        std::cin.get();

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }
    return 0;
}