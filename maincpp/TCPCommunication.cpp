#include "TCPCommunication.hpp"
#include <vector>
#include <algorithm>

// 使用boost::asio的命名空间
using boost::asio::ip::tcp;
using boost::system::error_code;

// --- 内部Session类的实现 ---

/**
 * @class TCPCommunication::Session
 * @brief 内部类，用于管理单个TCP连接的所有操作（读、写、超时）。
 *
 * 这个类被设计为只能通过shared_ptr进行管理，以确保在异步操作期间对象的生命周期。
 */
class TCPCommunication::Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, TCPCommunication* owner)
        : socket_(std::move(socket)),
          timer_(socket_.get_executor()),
          owner_(owner) {}

    ~Session() {
        // 当Session对象被销毁时，确保定时器被取消
        timer_.cancel();
    }

    void start() {
        reset_timer();
        do_read();
    }

    void do_write(const std::vector<char>& data) {
        auto self = shared_from_this();
        boost::asio::async_write(socket_, boost::asio::buffer(data),
            [this, self](const error_code& ec, size_t /*length*/) {
                if (!ec) {
                    reset_timer();
                } else {
                    handle_error("Write", ec);
                }
            });
    }

    void close() {
        boost::asio::post(socket_.get_executor(), [this, self = shared_from_this()]() {
            if (socket_.is_open()) {
                error_code ec;
                // Gracefully shutdown and close the socket
                // These operations may fail, but we're already closing
                auto shutdown_result = socket_.shutdown(tcp::socket::shutdown_both, ec);
                auto close_result = socket_.close(ec);
                // Suppress unused variable warnings
                (void)shutdown_result;
                (void)close_result;
            }
            timer_.cancel();
        });
    }

    tcp::socket& socket() { return socket_; }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(boost::asio::buffer(read_buffer_),
            [this, self](const error_code& ec, size_t length) {
                if (!ec) {
                    reset_timer();
                    if (owner_->on_read_) {
                        std::vector<char> received_data(read_buffer_.begin(), read_buffer_.begin() + length);
                        owner_->on_read_(received_data);
                    }
                    do_read(); // 继续下一个读取循环
                } else {
                    // 如果不是操作被取消的错误，则处理断开
                    if (ec != boost::asio::error::operation_aborted) {
                       owner_->remove_session(self);
                       if(owner_->on_disconnect_){
                           owner_->on_disconnect_();
                       }
                    }
                }
            });
    }

    void reset_timer() {
        long timeout = owner_->timeout_ms_.load();
        if (timeout <= 0) {
            return;
        }

        timer_.expires_at(std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout));

        auto self = shared_from_this();
        timer_.async_wait([this, self](const error_code& ec) {
            // 如果不是因为定时器被取消而触发，说明超时了
            if (ec != boost::asio::error::operation_aborted) {
                // 超时发生，关闭套接字
                close();
            }
        });
    }
    
    void handle_error(const std::string& context, const error_code& ec) {
        if (owner_->on_error_ && ec != boost::asio::error::operation_aborted) {
            owner_->on_error_(ec, context + " error");
        }
        close();
    }

    tcp::socket socket_;
    boost::asio::steady_timer timer_;
    std::array<char, 8192> read_buffer_;
    TCPCommunication* owner_;
};

// --- TCPCommunication 类的实现 ---

TCPCommunication::TCPCommunication(Mode mode)
    : mode_(mode),
      work_guard_(std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(io_context_.get_executor())) {
    io_thread_ = std::thread(&TCPCommunication::run_io_context, this);
}

TCPCommunication::~TCPCommunication() {
    stop();
}

void TCPCommunication::stop() {
    if (!is_stopped_.exchange(true)) {
        boost::asio::post(io_context_, [this]() {
             disconnect(); // 断开所有连接
            if (acceptor_) {
                acceptor_->close();
            }
        });
        work_guard_->reset(); // 允许 io_context.run() 退出
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
    }
}


void TCPCommunication::run_io_context() {
    io_context_.run();
}

void TCPCommunication::setTimeout(long milliseconds) {
    timeout_ms_.store(milliseconds > 0 ? milliseconds : 0);
}

void TCPCommunication::connect(const std::string& host, unsigned short port) {
    if (mode_ == Mode::Client) {
        auto resolver = std::make_shared<tcp::resolver>(io_context_);
        auto endpoints = resolver->resolve(host, std::to_string(port));
        
        session_ = std::make_shared<Session>(tcp::socket(io_context_), this);

        boost::asio::async_connect(session_->socket(), endpoints,
            [this, resolver](const error_code& ec, const tcp::endpoint& /*endpoint*/) {
                if (!ec) {
                    session_->start();
                    if (on_connect_) on_connect_(true);
                } else {
                    if (on_connect_) on_connect_(false); // 连接失败
                    if (on_error_) on_error_(ec, "Client connect failed");
                }
            });
    } else { // Server Mode
        acceptor_ = std::make_unique<tcp::acceptor>(io_context_, tcp::endpoint(tcp::v4(), port));
        start_accept();
    }
}

void TCPCommunication::start_accept() {
    auto new_session = std::make_shared<Session>(tcp::socket(io_context_), this);

    acceptor_->async_accept(new_session->socket(),
        [this, new_session](const error_code& ec) {
            handle_accept(new_session, ec);
        });
}

void TCPCommunication::handle_accept(std::shared_ptr<Session> new_session, const error_code& ec) {
    if (!ec) {
        // 如果已经有一个连接，关闭旧的连接
        if (session_) {
            session_->close();
        }
        
        session_ = new_session;
        session_->start();
        if (on_connect_) on_connect_(true);
        
        // 继续接受下一个连接（但只保持一个活动连接）
        start_accept();
    } else {
        if (on_error_ && ec != boost::asio::error::operation_aborted) {
            on_error_(ec, "Server accept failed");
        }
    }
}

void TCPCommunication::disconnect() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (session_) {
        session_->close();
        session_.reset();
    }
}

void TCPCommunication::write(const std::string& message) {
     write(std::vector<char>(message.begin(), message.end()));
}

void TCPCommunication::write(const std::vector<char>& data) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (session_) {
        session_->do_write(data);
    }
}

void TCPCommunication::remove_session(std::shared_ptr<Session> session_to_remove) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (session_ == session_to_remove) {
        session_.reset();
    }
}
