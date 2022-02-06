
#include <iostream>
#include <exception>
#include <memory>
#include <fstream>
#include <vector>
#include <functional>

#include <boost/version.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <libssh2.h>
#include <libssh2_sftp.h>

using boost::asio::ip::tcp;

#define BUFFER_SIZE 0x1000

namespace impl {

    struct Context {
        std::unique_ptr<tcp::resolver> resolver;
        std::unique_ptr<tcp::socket> socket;
        LIBSSH2_SESSION* session;
        LIBSSH2_SFTP* sftp_session;
        LIBSSH2_SFTP_HANDLE* sftp_handle;
        std::string target_host;
        std::string target_path;
        std::ofstream output_file;
        std::string username;
        std::string password;

        std::function<void()> handler;
    };

    void doCleanup(std::shared_ptr<Context> context) {
        // These cleanup calls are blocking, so one needs to create handlers and listen for EAGAIN for these aswell if blocking shutdown is not desired!!!
        libssh2_session_set_blocking(context->session, 1);

        libssh2_sftp_close(context->sftp_handle);
        libssh2_sftp_shutdown(context->sftp_session);
        libssh2_session_disconnect(context->session, "Normal Shutdown");
        libssh2_session_free(context->session);

        context->handler();
    }

    void doReceiveFile(std::shared_ptr<Context> context) {
        std::vector<char> buffer(BUFFER_SIZE);
        int rc = libssh2_sftp_read(context->sftp_handle, buffer.data(), buffer.size());
        if (rc > 0) {
            context->output_file.write(buffer.data(), rc);
            if (rc >= buffer.size()) {
                // buffer full, try to read again
                doReceiveFile(context);
            }
            else {
                // done receiving file
                doCleanup(context);
            }
        }
        else if (rc == LIBSSH2_ERROR_EAGAIN) {
            context->socket->async_wait(boost::asio::socket_base::wait_type::wait_read, [context](const boost::system::error_code& ec) {
                if (ec) {
                    throw std::runtime_error(ec.message());
                }
                doReceiveFile(context); 
            });
        }
        else if (rc < 0) { // different 
            throw std::runtime_error("Failed to receive file");
        }
    }

    void doOpenFile(std::shared_ptr<Context> context) {
        context->sftp_handle = libssh2_sftp_open(context->sftp_session, context->target_path.c_str(), LIBSSH2_FXF_READ, 0);
        if (!context->sftp_handle) {
            if(libssh2_session_last_errno(context->session) == LIBSSH2_ERROR_EAGAIN) {
                context->socket->async_wait(boost::asio::socket_base::wait_type::wait_write, [context](const boost::system::error_code& ec) {
                    if (ec) {
                        throw std::runtime_error(ec.message());
                    }
                    doOpenFile(context); 
                });
            }
            else {
                throw std::runtime_error("Failed to open file");
            }
        }
        else {
            doReceiveFile(context);
        }
    }

    void doSFTPInit(std::shared_ptr<Context> context) {
        context->sftp_session = libssh2_sftp_init(context->session);
        if (!context->sftp_session) {
            if (libssh2_session_last_errno(context->session) == LIBSSH2_ERROR_EAGAIN) {
                context->socket->async_wait(boost::asio::socket_base::wait_type::wait_write, [context](const boost::system::error_code& ec) {
                    if (ec) {
                        throw std::runtime_error(ec.message());
                    }
                    doSFTPInit(context); 
                });
            }
            else {
                throw std::runtime_error("Failed to init sftp session");
            }
        }
        else {
            doOpenFile(context);
        }
    }

    void doAuthentication(std::shared_ptr<Context> context) {
        int rc = libssh2_userauth_password(context->session, context->username.c_str(), context->password.c_str());
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            context->socket->async_wait(boost::asio::socket_base::wait_type::wait_write, [context](const boost::system::error_code& ec) {
                if (ec) {
                    throw std::runtime_error(ec.message());
                }
                doAuthentication(context);
            });
        }
        else if (rc) {
            throw std::runtime_error("Failed to authenticate");
        }
        else {
            doSFTPInit(context);
        }
    }

    void doSessionHandshake(std::shared_ptr<Context> context) {
        int rc = libssh2_session_handshake(context->session, context->socket->lowest_layer().native_handle());
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            context->socket->async_wait(boost::asio::socket_base::wait_type::wait_write, [context](const boost::system::error_code& ec) {
                if (ec) {
                    throw std::runtime_error(ec.message());
                }
                doSessionHandshake(context);
            });
        }
        else if (rc) {
            throw std::runtime_error("Failed to create ssh session");
        }
        else {
            const char* fingerprint = libssh2_hostkey_hash(context->session, LIBSSH2_HOSTKEY_HASH_SHA1);
            fprintf(stderr, "Fingerprint: ");
            for(int i = 0; i < 20; i++) {
                fprintf(stderr, "%02X ", (unsigned char)fingerprint[i]);
            }
            fprintf(stderr, "\n");

            doAuthentication(context);
        }
    }

    void connectHandler(const boost::system::error_code& ec, const tcp::endpoint& endpoint, std::shared_ptr<Context> context) {
        if (ec) {
            throw std::runtime_error(ec.message());
        }

        // init libssh
        context->session = libssh2_session_init();
        if (!context->session) {
            throw std::runtime_error("Init session failed");
        }

        libssh2_session_set_blocking(context->session, 0);

        doSessionHandshake(context);

    }
    
    void resolveHandler(const boost::system::error_code& ec, const tcp::resolver::results_type& endpoints, std::shared_ptr<Context> context) {
        if (ec) {
            throw std::runtime_error(ec.message());
        }
        if (endpoints.empty()) {
            throw std::runtime_error("No endpoints");
        }

        context->socket = std::make_unique<tcp::socket>(context->resolver->get_executor());
        boost::asio::async_connect(*context->socket, endpoints, [context](const boost::system::error_code& ec, const tcp::endpoint& endpoint){
            connectHandler(ec, endpoint, context);
        });
    }

}

template <class Handler>
void downloadFile(boost::asio::io_context& ioc, const std::string& target_host, const std::string& target_path, const std::string& destination_path, const std::string& username, const std::string& password, Handler&& handler) {
    auto context = std::make_shared<impl::Context>();
    
    context->target_host = target_host;
    context->target_path = target_path;
    context->output_file = std::ofstream(destination_path, std::ios::binary);
    context->username = username;
    context->password = password;
    
    context->resolver = std::make_unique<tcp::resolver>(ioc);

    context->handler = handler;
    
    context->resolver->async_resolve(tcp::resolver::query(target_host, "22"), [context](const boost::system::error_code& ec, const tcp::resolver::results_type& endpoints) {
        impl::resolveHandler(ec, endpoints, context);
    });
}

int main(int argc, char** argv) {

    if (argc < 3) {
        std::cout << "Invalid arguments\n";
        std::cout << "usage: " << argv[0] << " <ssh username> <ssh password>\n";
        return EXIT_FAILURE;
    }
    

    int rc = libssh2_init(0);
    if(rc != 0) {
        throw std::runtime_error("libssh2 init failed");
    }

    boost::asio::io_context ioc;
    
    std::string target_host = "localhost";
    std::string target_path = "/tmp/test1.txt";
    std::string destination_path = "/tmp/test2.txt";
    std::string username = argv[1];
    std::string password = argv[2];

    // write test file
    {
        // The file IO calls are blocking, but nonblocking file IO is outside the scope if this test
        std::ofstream test_file(target_path);
        for (int i = 0; i < 1000; ++i) {
            test_file << "i = " << i << " yopyo tyhis is a test file with some content\n";
        }
    }
    

    downloadFile(ioc, target_host, target_path, destination_path, username, password, [destination_path](){std::cout << "done, file should be written to: " << destination_path << "\n";});


    ioc.run();

    return EXIT_SUCCESS;
}

