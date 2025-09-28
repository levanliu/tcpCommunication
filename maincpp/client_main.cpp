#include "TCPCommunication.hpp"
#include <iostream>
#include <chrono>
#include <thread>

int main() {
    try {
        TCPCommunication client(TCPCommunication::Mode::Client);
        bool connected = false;

        client.setOnConnect([&connected](bool success) {
            if (success) {
                std::cout << "Client: Connected to server.\n";
                connected = true;
            } else {
                std::cout << "Client: Connection failed.\n";
            }
        });

        client.setOnDisconnect([]() {
            std::cout << "Client: Disconnected from server.\n";
        });

        client.setOnRead([](std::vector<char>& data) {
            std::string message(data.begin(), data.end());
            std::cout << "Client: Received: " << message << std::endl;
        });

        client.setOnError([](const boost::system::error_code& ec, const std::string& msg) {
            std::cerr << "Client Error: " << msg << " (" << ec.message() << ")\n";
        });

        client.connect("127.0.0.1", 12345);

        // 等待连接成功
        for (int i = 0; i < 5 && !connected; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!connected) {
            std::cerr << "Could not connect to server in time." << std::endl;
            return 1;
        }

        // 发送几条消息
        for (int i = 1; i <= 3; ++i) {
            std::string msg = "Hello from client, message " + std::to_string(i);
            client.write(msg);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::cout << "Client finished sending messages. Disconnecting in 2s.\n";
        std::this_thread::sleep_for(std::chrono::seconds(2));
        client.disconnect();

        // 等待一小会儿确保断开连接消息被处理
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}