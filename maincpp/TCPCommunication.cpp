#include "TCPCommunication.hpp"
#include <vector>

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
    Session(tcp::socket socket, TCPCommunication* owner, ConnectionID id)
        : socket_(std::move(socket)),
          timer_(socket_.get_executor()),
          owner_(owner),
          id_(id) {}

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
                socket_.shutdown(tcp::socket::shutdown_both, ec);
                socket_.close(ec);
            }
            timer_.cancel();
        });
    }

    tcp::socket& socket() { return socket_; }
    ConnectionID id() const { return id_; }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(boost::asio::buffer(read_buffer_),
            [this, self](const error_code& ec, size_t length) {
                if (!ec) {
                    reset_timer();
                    if (owner_->on_read_) {
                        std::vector<char> received_data(read_buffer_.begin(), read_buffer_.begin() + length);
                        owner_->on_read_(id_, received_data);
                    }
                    do_read(); // 继续下一个读取循环
                } else {
                    // 如果不是操作被取消的错误，则处理断开
                    if (ec != boost::asio::error::operation_aborted) {
                       owner_->remove_session(id_);
                       if(owner_->on_disconnect_){
                           owner_->on_disconnect_(id_);
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
            owner_->on_error_(id_, ec, context + " error");
        }
        close();
    }

    tcp::socket socket_;
    boost::asio::steady_timer timer_;
    std::array<char, 8192> read_buffer_;
    TCPCommunication* owner_;
    const ConnectionID id_;
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
             disconnect(INVALID_ID); // 断开所有连接
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
        
        // 客户端模式下，ID固定为1
        client_session_ = std::make_shared<Session>(tcp::socket(io_context_), this, INVALID_ID + 1);

        boost::asio::async_connect(client_session_->socket(), endpoints,
            [this, resolver](const error_code& ec, const tcp::endpoint& /*endpoint*/) {
                if (!ec) {
                    client_session_->start();
                    if (on_connect_) on_connect_(client_session_->id());
                } else {
                    if (on_connect_) on_connect_(INVALID_ID); // 连接失败
                    if (on_error_) on_error_(INVALID_ID, ec, "Client connect failed");
                }
            });
    } else { // Server Mode
        acceptor_ = std::make_unique<tcp::acceptor>(io_context_, tcp::endpoint(tcp::v4(), port));
        start_accept();
    }
}

void TCPCommunication::start_accept() {
    ConnectionID new_id = next_connection_id_++;
    auto new_session = std::make_shared<Session>(tcp::socket(io_context_), this, new_id);

    acceptor_->async_accept(new_session->socket(),
        [this, new_session](const error_code& ec) {
            handle_accept(new_session, ec);
        });
}

void TCPCommunication::handle_accept(std::shared_ptr<Session> new_session, const error_code& ec) {
    if (!ec) {
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_[new_session->id()] = new_session;
        }
        new_session->start();
        if (on_connect_) on_connect_(new_session->id());
        
        // 接受下一个连接
        start_accept();
    } else {
        if (on_error_ && ec != boost::asio::error::operation_aborted) {
            on_error_(INVALID_ID, ec, "Server accept failed");
        }
    }
}

void TCPCommunication::disconnect(ConnectionID id) {
    if (mode_ == Mode::Client) {
        if (client_session_) {
            client_session_->close();
            // 在Session的读取循环中处理断开回调和资源清理
        }
    } else { // Server Mode
        if (id != INVALID_ID) {
            std::shared_ptr<Session> session_to_close;
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                auto it = sessions_.find(id);
                if (it != sessions_.end()) {
                    session_to_close = it->second;
                }
            }
            if (session_to_close) {
                 session_to_close->close();
            }
        } else { // Disconnect all
             std::lock_guard<std::mutex> lock(sessions_mutex_);
             for(auto const& [key, val] : sessions_){
                 val->close();
             }
             sessions_.clear();
        }
    }
}

void TCPCommunication::write(const std::string& message, ConnectionID id) {
     write(std::vector<char>(message.begin(), message.end()), id);
}

void TCPCommunication::write(const std::vector<char>& data, ConnectionID id) {
     if(mode_ == Mode::Client){
         if(client_session_){
             client_session_->do_write(data);
         }
     } else {
        std::shared_ptr<Session> session_to_write;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(id);
            if(it != sessions_.end()){
                session_to_write = it->second;
            }
        }
        if(session_to_write){
            session_to_write->do_write(data);
        }
     }
}

void TCPCommunication::remove_session(ConnectionID id) {
    if(mode_ == Mode::Client){
        // 客户端会话结束
        client_session_.reset();
    } else {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.erase(id);
    }
}
