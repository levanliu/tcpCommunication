#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>

// 引入 Boost.Asio 库，这是实现异步网络通信的核心
#include <boost/asio.hpp>

/**
 * @class TCPCommunication
 * @brief 一个支持客户端和服务器模式的C++17异步TCP通信模块。
 *
 * 该类封装了 Boost.Asio 的复杂性，提供了一个简单的API来处理TCP连接。
 * 它可以在一个后台线程中运行所有网络操作，通过回调函数与主线程通信。
 */
class TCPCommunication {
public:
    /**
     * @enum Mode
     * @brief 定义模块的工作模式：客户端或服务器。
     */
    enum class Mode {
        Client,
        Server
    };



    // --- 回调函数类型定义 ---

    /**
     * @brief 连接回调函数
     * @param success 连接是否成功。
     */
    using OnConnectCallback = std::function<void(bool success)>;

    /**
     * @brief 断开连接回调函数
     */
    using OnDisconnectCallback = std::function<void()>;

    /**
     * @brief 数据接收回调函数
     * @param data 接收到的数据。
     */
    using OnReadCallback = std::function<void(std::vector<char>& data)>;

    /**
     * @brief 错误回调函数
     * @param error_code 错误码。
     * @param message 错误信息。
     */
    using OnErrorCallback = std::function<void(const boost::system::error_code& error_code, const std::string& message)>;


    /**
     * @brief 构造函数
     * @param mode TCPCommunication模块的工作模式（客户端或服务器）。
     */
    explicit TCPCommunication(Mode mode);

    /**
     * @brief 析构函数
     * 自动停止所有网络活动并清理资源。
     */
    ~TCPCommunication();

    // --- 核心API ---

    /**
     * @brief 连接或启动服务器。
     * - 客户端模式: 异步连接到指定的服务器。
     * - 服务器模式: 在指定端口上开始监听客户端连接。'host'参数被忽略。
     * @param host 服务器地址（仅客户端模式需要）。
     * @param port 服务器端口（客户端模式）或监听端口（服务器模式）。
     */
    void connect(const std::string& host, unsigned short port);

    /**
     * @brief 断开连接。
     * - 客户端模式: 断开与服务器的连接。
     * - 服务器模式: 断开所有客户端连接并停止服务器。
     */
    void disconnect();

    /**
     * @brief 发送数据。
     * - 客户端模式: 异步发送数据到服务器。
     * - 服务器模式: 异步广播数据到所有连接的客户端。
     * @param message 要发送的字符串数据。
     */
    void write(const std::string& message);
    
    /**
     * @brief 发送数据。
     * - 客户端模式: 异步发送数据到服务器。
     * - 服务器模式: 异步广播数据到所有连接的客户端。
     * @param data 要发送的二进制数据。
     */
    void write(const std::vector<char>& data);


    /**
     * @brief 读取数据。
     * 注意：这是一个名义上的方法，以满足API要求。实际的数据读取是通过
     * OnReadCallback 异步回调函数自动处理的，无需手动调用此方法。
     */
    void read() { /* 无操作 - 读取由OnReadCallback自动处理 */ }

    /**
     * @brief 设置不活动超时。
     * 如果一个连接在指定时间内没有任何读写活动，它将被自动断开。
     * @param milliseconds 超时时间（毫秒）。设置为0表示禁用超时。
     */
    void setTimeout(long milliseconds);

    // --- 回调函数设置 ---
    void setOnConnect(OnConnectCallback cb) { on_connect_ = std::move(cb); }
    void setOnDisconnect(OnDisconnectCallback cb) { on_disconnect_ = std::move(cb); }
    void setOnRead(OnReadCallback cb) { on_read_ = std::move(cb); }
    void setOnError(OnErrorCallback cb) { on_error_ = std::move(cb); }

private:
    // 内部Session类的前向声明，用于处理单个TCP连接
    class Session;

    // 运行io_context的后台线程函数
    void run_io_context();
    // 停止服务和后台线程
    void stop();

    // 服务器模式：开始接受新连接
    void start_accept();
    // 服务器模式：处理接受新连接的结果
    void handle_accept(std::shared_ptr<Session> new_session, const boost::system::error_code& ec);
    
    // 从会话列表中移除一个会话
    void remove_session(std::shared_ptr<Session> session);

    // --- 成员变量 ---
    const Mode mode_;
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;
    std::thread io_thread_;
    std::atomic<bool> is_stopped_{false};

    // 回调函数
    OnConnectCallback on_connect_;
    OnDisconnectCallback on_disconnect_;
    OnReadCallback on_read_;
    OnErrorCallback on_error_;

    // 服务器模式专用
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;

    // 单一会话（客户端或服务器模式）
    std::shared_ptr<Session> session_;
    std::mutex session_mutex_;

    // 配置
    std::atomic<long> timeout_ms_{0};
};
