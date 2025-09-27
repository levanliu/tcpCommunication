#include "SCPIInstrumentSimulator.hpp"
#include <boost/asio.hpp>
#include <iostream>
#include <string>
#include <thread>

using boost::asio::ip::tcp;
using boost::system::error_code;

SCPIInstrumentSimulator::SCPIInstrumentSimulator(
    boost::asio::io_context& io_context, unsigned short port)
    : io_context_(io_context),
      acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
    std::cout << "Simulator listening on port " << port << "..." << std::endl;
    do_accept();
}

void SCPIInstrumentSimulator::do_accept() {
    // 创建一个新的 socket 等待连接
    acceptor_.async_accept([this](error_code ec, tcp::socket socket) {
        if (!ec) {
            // 连接成功，启动一个新线程来处理这个连接
            std::cout << "Simulator: Client connected from "
                      << socket.remote_endpoint().address().to_string()
                      << std::endl;
            std::thread(&SCPIInstrumentSimulator::handle_session, this,
                        std::move(socket))
                .detach();
        }
        // 继续接受下一个连接
        do_accept();
    });
}

void SCPIInstrumentSimulator::handle_session(tcp::socket socket) {
    try {
        // 使用 streambuf 来处理以 '\n' 结尾的完整消息
        boost::asio::streambuf buffer;
        std::string delimiter = "\n";

        for (;;) {
            // 同步读取，直到遇到终结符
            size_t bytes_transferred =
                boost::asio::read_until(socket, buffer, delimiter);

            // 从 streambuf 中提取消息
            std::istream is(&buffer);
            std::string command;
            std::getline(is, command);

            // 移除可能的 '\r' 字符
            if (!command.empty() && command.back() == '\r') {
                command.pop_back();
            }

            if (command.empty())
                continue;

            std::cout << "Simulator received: [" << command << "]" << std::endl;

            // --- SCPI 响应逻辑 ---
            std::string response = "";
            if (command == "*IDN?") {
                response = "AGILENT,E5810A,SN12345678,V1.0";
            } else if (command.find("MEAS:VOLT:DC?") == 0) {
                response = "+1.23456E+00";  // 模拟万用表读数
            } else if (command.find("WRITE:DELAY") == 0) {
                response = "";  // 写命令通常没有响应
            } else {
                response = "Command not supported: " + command;
            }

            // 如果有响应，发送回去 (自动添加终结符)
            if (!response.empty()) {
                response += delimiter;
                boost::asio::write(socket, boost::asio::buffer(response));
                std::cout << "Simulator sent: ["
                          << response.substr(0, response.size() - 1) << "]"
                          << std::endl;
            }
        }
    } catch (const boost::system::system_error& e) {
        // 客户端断开连接时，read_until 会抛出 "End of file" 异常
        if (e.code() == boost::asio::error::eof) {
            std::cout << "Simulator: Client disconnected gracefully."
                      << std::endl;
        } else {
            // 其他类型的错误，仍然打印
            std::cerr << "Simulator session ended with error: " << e.what()
                      << std::endl;
        }
    }
}