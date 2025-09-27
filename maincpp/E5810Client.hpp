#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>  // 用于兼容旧的asio版本，如果您的版本较新可能不需要
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// 简化命名空间
using boost::asio::ip::tcp;
using boost::system::error_code;

/**
 * @class E5810Client
 * @brief 纯异步TCP客户端，用于通过E5810A/B/C等Gateway控制GPIB仪器（VXI-11 Raw Socket模式）。
 */
class E5810Client {
   public:
    using ConnectionID = uint64_t;
    static constexpr ConnectionID INVALID_ID = 0;
    static constexpr ConnectionID CLIENT_ID = 1;

    using OnConnectCallback = std::function<void(ConnectionID id)>;
    using OnDisconnectCallback = std::function<void(ConnectionID id)>;
    using OnReadCallback =
        std::function<void(ConnectionID id, const std::string& message)>;
    using OnErrorCallback = std::function<void(
        ConnectionID id, const error_code& ec, const std::string& message)>;

    E5810Client();
    ~E5810Client();

    boost::asio::io_context& get_io_context();

    void connect(const std::string& host, unsigned short port);
    void disconnect();

    // 发送SCPI命令，会自动添加终结符
    void write(const std::string& command);

    // 设置不活动超时（0禁用）
    void setTimeout(long milliseconds);

    // 设置SCPI消息的终结符 (通常是 "\n")
    void setReadDelimiter(const std::string& delimiter) {
        read_delimiter_ = delimiter;
    }

    // 回调函数设置
    void setOnConnect(OnConnectCallback cb) { on_connect_ = std::move(cb); }
    void setOnDisconnect(OnDisconnectCallback cb) {
        on_disconnect_ = std::move(cb);
    }
    void setOnRead(OnReadCallback cb) { on_read_ = std::move(cb); }
    void setOnError(OnErrorCallback cb) { on_error_ = std::move(cb); }

   private:
    class Session;

    void run_io_context();

    void stop();
    void remove_session();

    // --- 成员变量 ---
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>
        work_guard_;
    std::thread io_thread_;
    std::atomic<bool> is_stopped_{false};
    std::string read_delimiter_{"\n"};

    OnConnectCallback on_connect_;
    OnDisconnectCallback on_disconnect_;
    OnReadCallback on_read_;
    OnErrorCallback on_error_;

    std::shared_ptr<Session> client_session_;
    std::atomic<long> timeout_ms_{0};
};

// ----------------- E5810Client::Session 的前向声明和简单实现 -----------------

class E5810Client::Session : public std::enable_shared_from_this<Session> {
   public:
    Session(tcp::socket socket, E5810Client* owner)
        : socket_(std::move(socket)),
          timer_(socket_.get_executor()),
          owner_(owner),
          id_(CLIENT_ID) {}

    ~Session() { timer_.cancel(); }

    void start() {
        reset_timer();
        do_read();
    }

    void do_write(const std::vector<char>& data) {
        auto self = shared_from_this();
        boost::asio::async_write(
            socket_, boost::asio::buffer(data),
            [this, self](const error_code& ec, size_t /*length*/) {
                if (!ec) {
                    reset_timer();
                } else {
                    handle_error("Write", ec);
                }
            });
    }

    void close() {
        boost::asio::post(
            socket_.get_executor(), [this, self = shared_from_this()]() {
                if (socket_.is_open()) {
                    error_code ec;
                    socket_.shutdown(tcp::socket::shutdown_both, ec);
                    socket_.close(ec);
                }
                timer_.cancel();
            });
    }

    tcp::socket& socket() { return socket_; }

   private:
    void do_read() {
        auto self = shared_from_this();
        // 使用 async_read_until 读取到由 delimiter 终止的完整消息
        boost::asio::async_read_until(
            socket_, read_buffer_, owner_->read_delimiter_,
            [this, self](const error_code& ec, size_t /*length*/) {
                if (!ec) {
                    reset_timer();

                    std::istream is(&read_buffer_);
                    std::string received_message;
                    std::getline(is, received_message);

                    // 移除可能的 '\r' 字符
                    if (!received_message.empty() &&
                        received_message.back() == '\r') {
                        received_message.pop_back();
                    }

                    if (owner_->on_read_) {
                        owner_->on_read_(id_, received_message);
                    }

                    do_read();  // 循环读取下一个消息
                } else {
                    if (ec != boost::asio::error::operation_aborted) {
                        owner_->remove_session();
                        if (owner_->on_disconnect_) {
                            owner_->on_disconnect_(id_);
                        }
                    }
                }
            });
    }

    void reset_timer() {
        long timeout = owner_->timeout_ms_.load();
        if (timeout <= 0)
            return;

        timer_.expires_at(std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout));

        auto self = shared_from_this();
        timer_.async_wait([this, self](const error_code& ec) {
            if (ec != boost::asio::error::operation_aborted) {
                close();
                owner_->remove_session();
                if (owner_->on_disconnect_) {
                    owner_->on_disconnect_(id_);
                }
            }
        });
    }

    void handle_error(const std::string& context, const error_code& ec) {
        if (ec != boost::asio::error::operation_aborted) {
            if (owner_->on_error_) {
                owner_->on_error_(id_, ec, context + " error");
            }
            close();
            owner_->remove_session();
            if (owner_->on_disconnect_) {
                owner_->on_disconnect_(id_);
            }
        }
    }

    tcp::socket socket_;
    boost::asio::steady_timer timer_;
    boost::asio::streambuf read_buffer_;
    E5810Client* owner_;
    const ConnectionID id_;
};
