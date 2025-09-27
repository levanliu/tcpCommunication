#include <chrono>
#include <iomanip>  // 用于 std::setprecision
#include <thread>
#include "E5810Client.hpp"              // 包含客户端头文件
#include "SCPIInstrumentSimulator.hpp"  // 包含仿真服务器头文件

// 定义本地环回地址和 VXI-11 Raw Socket 端口
const std::string HOST = "127.0.0.1";
const unsigned short PORT = 5025;

// -------------------------------------------------------------------
// 辅助函数: 仪器通信应用程序的 回调处理逻辑
// -------------------------------------------------------------------

void handle_connect(E5810Client& client, E5810Client::ConnectionID id) {
    if (id != E5810Client::INVALID_ID) {
        std::cout << "✅ Client Connected! ID: " << id << std::endl;

        // 步骤 1: 请求识别信息 (Identification Query)
        client.write("*IDN?");

        // 步骤 2: 发送一个写命令
        client.write("WRITE:DELAY 50");

        // 步骤 3: 请求测量数据
        client.write("MEAS:VOLT:DC?");
    } else {
        std::cout << "❌ Client Connection Failed." << std::endl;
    }
}

void handle_disconnect(E5810Client::ConnectionID id) {
    std::cout << "🔌 Client Disconnected! ID: " << id << std::endl;
}

void handle_read(E5810Client::ConnectionID id, const std::string& message) {
    // 异步接收到完整的 SCPI 响应
    std::cout << "📦 Received (ID: " << id << "): [" << message << "]"
              << std::endl;

    // 您可以在这里解析响应，例如检查 *IDN? 响应或处理测量值
    if (message.find("AGILENT") == 0) {
        std::cout << "   -> Device identified successfully." << std::endl;
    } else if (message.find("+") == 0) {
        double voltage = std::stod(message);
        std::cout << "   -> Measurement Value: " << std::fixed
                  << std::setprecision(6) << voltage << " V" << std::endl;
    }
}

void handle_error(E5810Client::ConnectionID id,
                  const boost::system::error_code& ec,
                  const std::string& message) {
    std::cerr << "⚠️ ERROR (ID: " << id << "): " << message << " - "
              << ec.message() << std::endl;
}

// -------------------------------------------------------------------
// 主程序
// -------------------------------------------------------------------

// 外部声明，让 main 可以调用 SCPIInstrumentSimulator 的代码
extern void start_simulator(unsigned short port);

int main() {
    // 1. 启动 SCPI 仿真服务器（在自己的线程中运行）
    boost::asio::io_context sim_io_context;
    SCPIInstrumentSimulator simulator(sim_io_context, PORT);
    std::thread sim_thread([&sim_io_context]() { sim_io_context.run(); });

    // 给予服务器启动时间
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 2. 启动异步 TCP 客户端
    E5810Client client;

    // 3. 设置回调函数
    client.setOnConnect(
        std::bind(handle_connect, std::ref(client), std::placeholders::_1));
    client.setOnDisconnect(handle_disconnect);
    client.setOnRead(handle_read);
    client.setOnError(handle_error);

    // 4. 设置配置
    client.setTimeout(5000);        // 5秒不活动超时
    client.setReadDelimiter("\n");  // VXI-11 Raw Socket 模式的默认终结符

    // 5. 连接到仿真服务器
    std::cout << "\nAttempting to connect to " << HOST << ":" << PORT << "..."
              << std::endl;
    client.connect(HOST, PORT);

    // 6. 保持主线程运行，等待异步操作完成
    std::cout << "Client is running in background. Press Enter to exit."
              << std::endl;
    std::cin.get();

    // 7. 清理资源
    client.disconnect();
    sim_io_context.stop();
    if (sim_thread.joinable()) {
        sim_thread.join();
    }

    return 0;
}