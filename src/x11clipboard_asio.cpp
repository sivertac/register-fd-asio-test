
#include <iostream>
#include <exception>

#include <boost/version.hpp>
#include <boost/asio.hpp>

#include "X11Wrapper.hpp"

int main(int argc, char** argv) {

    boost::asio::io_context ioc;

    X11Wrapper::ClipboardWriter writer(ioc);

    boost::asio::post(ioc.get_executor(), [&](){
        std::cout << "Reading clipboard message...\n";
        std::cout << "res: " << X11Wrapper::readImpl() << "\n";
    });

    std::cout << "Setting clipboard message...\n";
    writer.setMsg("This is my clipboard message", [](){
        std::cout << "Reading clipboard message...\n";
        std::cout << "res: " << X11Wrapper::readImpl() << "\n";
    });

    ioc.run();
    
    return 0;
}
