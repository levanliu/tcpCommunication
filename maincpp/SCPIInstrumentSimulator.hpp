#include <boost/asio.hpp>
#include <iostream>
#include <string>
#include <thread>

using boost::asio::ip::tcp;
using boost::system::error_code;

class SCPIInstrumentSimulator {
   public:
    SCPIInstrumentSimulator(boost::asio::io_context& io_context,
                            unsigned short port);

   private:
    void do_accept();

    void handle_session(tcp::socket socket);

    boost::asio::io_context& io_context_;
    tcp::acceptor acceptor_;
};