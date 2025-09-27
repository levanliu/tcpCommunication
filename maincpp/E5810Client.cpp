#include "E5810Client.hpp"
#include <vector>

// 使用boost::asio的命名空间
using boost::asio::ip::tcp;
using boost::system::error_code;

E5810Client::E5810Client()
    : work_guard_(std::make_unique<boost::asio::executor_work_guard<
                      boost::asio::io_context::executor_type>>(
          io_context_.get_executor())) {
    io_thread_ = std::thread(&E5810Client::run_io_context, this);
}

E5810Client::~E5810Client() {
    stop();
}

void E5810Client::stop() {
    if (!is_stopped_.exchange(true)) {
        boost::asio::post(io_context_, [this]() { disconnect(); });
        work_guard_->reset();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
    }
}

void E5810Client::run_io_context() {
    try {
        io_context_.run();
    } catch (const std::exception& e) {
        std::cerr << "E5810Client io_context exception: " << e.what()
                  << std::endl;
    }
}

boost::asio::io_context& E5810Client::get_io_context() {
    return io_context_;
}

void E5810Client::setTimeout(long milliseconds) {
    timeout_ms_.store(milliseconds > 0 ? milliseconds : 0);
}

void E5810Client::connect(const std::string& host, unsigned short port) {
    if (client_session_) {
        disconnect();
    }

    auto resolver = std::make_shared<tcp::resolver>(io_context_);
    client_session_ = std::make_shared<Session>(tcp::socket(io_context_), this);

    resolver->async_resolve(
        host, std::to_string(port),
        [this, resolver](const error_code& ec_resolve,
                         tcp::resolver::results_type endpoints) {
            if (ec_resolve) {
                if (on_connect_)
                    on_connect_(INVALID_ID);
                if (on_error_)
                    on_error_(INVALID_ID, ec_resolve, "Resolve failed");
                client_session_.reset();
                return;
            }

            boost::asio::async_connect(
                client_session_->socket(), endpoints,
                [this](const error_code& ec_connect,
                       const tcp::endpoint& /*endpoint*/) {
                    if (!ec_connect) {
                        client_session_->start();
                        if (on_connect_)
                            on_connect_(CLIENT_ID);
                    } else {
                        if (on_connect_)
                            on_connect_(INVALID_ID);
                        if (on_error_)
                            on_error_(INVALID_ID, ec_connect,
                                      "Client connect failed");
                        client_session_.reset();
                    }
                });
        });
}

void E5810Client::disconnect() {
    if (client_session_) {
        client_session_->close();
    }
}

void E5810Client::write(const std::string& command) {
    // 自动添加 SCPI 终结符 (e.g., "*IDN?\n")
    std::string full_command = command + read_delimiter_;

    // 异步发送数据
    if (client_session_) {
        client_session_->do_write(
            std::vector<char>(full_command.begin(), full_command.end()));
    } else {
        if (on_error_)
            on_error_(INVALID_ID, {}, "Write failed: Not connected");
    }
}

void E5810Client::remove_session() {
    client_session_.reset();
}