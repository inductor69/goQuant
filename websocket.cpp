#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>
#include <cstdlib>
#include <iostream>
#include <string>
#include <json/json.h>

// Namespace aliases for convenience
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

class WebSocketClient : public std::enable_shared_from_this<WebSocketClient>
{
public:
    // Constructor: initialize with io_context and SSL context
    explicit WebSocketClient(net::io_context& ioc, ssl::context& ctx)
        : resolver_(ioc), ws_(ioc, ctx) {}

    // Start the connection process
    void run(const std::string& host, const std::string& port, const std::string& instrument)
    {
        host_ = host;
        instrument_ = instrument;

        // Start the asynchronous operation
        resolver_.async_resolve(host, port,
            beast::bind_front_handler(&WebSocketClient::on_resolve, shared_from_this()));
    }

private:
    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    beast::flat_buffer buffer_;
    std::string host_;
    std::string instrument_;

    // Callback for async_resolve
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results)
    {
        if (ec)
            return fail(ec, "resolve");

        // Set a timeout on the operation
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));

        // Make the connection on the IP address we get from a lookup
        beast::get_lowest_layer(ws_).async_connect(results,
            beast::bind_front_handler(&WebSocketClient::on_connect, shared_from_this()));
    }

    // Callback for async_connect
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep)
    {
        if (ec)
            return fail(ec, "connect");

        // Update the host_ string. This will provide the value of the
        // Host HTTP header during the WebSocket handshake.
        host_ += ':' + std::to_string(ep.port());

        // Set a timeout on the operation
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));

        // Perform the SSL handshake
        ws_.next_layer().async_handshake(ssl::stream_base::client,
            beast::bind_front_handler(&WebSocketClient::on_ssl_handshake, shared_from_this()));
    }

    // Callback for SSL async_handshake
    void on_ssl_handshake(beast::error_code ec)
    {
        if (ec)
            return fail(ec, "ssl_handshake");

        // Turn off the timeout on the tcp_stream, because
        // the websocket stream has its own timeout system.
        beast::get_lowest_layer(ws_).expires_never();

        // Set suggested timeout settings for the websocket
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));

        // Perform the websocket handshake
        ws_.async_handshake(host_, "/ws/api/v2",
            beast::bind_front_handler(&WebSocketClient::on_handshake, shared_from_this()));
    }

    // Callback for WebSocket async_handshake
    void on_handshake(beast::error_code ec)
    {
        if (ec)
            return fail(ec, "handshake");

        // Send the subscription message
        subscribe_to_orderbook();
    }

    // Subscribe to the orderbook
    void subscribe_to_orderbook()
    {
        // Prepare the subscription message using JsonCpp
        Json::Value subscription;
        subscription["jsonrpc"] = "2.0";
        subscription["id"] = 1;
        subscription["method"] = "public/subscribe";
        subscription["params"]["channels"].append("book." + instrument_ + ".100ms");

        // Convert JSON to string
        Json::FastWriter writer;
        std::string message = writer.write(subscription);

        // Send the message
        ws_.async_write(net::buffer(message),
            beast::bind_front_handler(&WebSocketClient::on_write, shared_from_this()));
    }

    // Callback for async_write
    void on_write(beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if (ec)
            return fail(ec, "write");

        // Read a message
        read();
    }

    // Asynchronously read a message
    void read()
    {
        ws_.async_read(buffer_,
            beast::bind_front_handler(&WebSocketClient::on_read, shared_from_this()));
    }

    // Callback for async_read
    void on_read(beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if (ec)
            return fail(ec, "read");

        // Convert the message to a string
        std::string data = beast::buffers_to_string(buffer_.data());
        
        // Parse the JSON message
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(data, root))
        {
            std::cout << "Received message:" << std::endl;
            std::cout << root.toStyledString() << std::endl;
        }
        else
        {
            std::cerr << "Failed to parse JSON: " << data << std::endl;
        }

        // Clear the buffer
        buffer_.consume(buffer_.size());

        // Queue up another read
        read();
    }

    // Report a failure
    void fail(beast::error_code ec, char const* what)
    {
        std::cerr << what << ": " << ec.message() << "\n";
    }
};

int main(int argc, char** argv)
{
    // Check command line arguments
    if (argc != 2)
    {
        std::cerr << "Usage: websocket_client <instrument>\n";
        std::cerr << "Example: websocket_client BTC-PERPETUAL\n";
        return EXIT_FAILURE;
    }
    const std::string instrument = argv[1];

    try
    {
        // The io_context is required for all I/O
        net::io_context ioc;

        // The SSL context is required for SSL
        ssl::context ctx{ssl::context::tlsv12_client};
        ctx.set_verify_mode(ssl::verify_peer);
        ctx.set_default_verify_paths();

        // Launch the asynchronous operation
        std::make_shared<WebSocketClient>(ioc, ctx)->run("www.deribit.com", "443", instrument);

        // Run the I/O service. The call will return when
        // the socket is closed.
        ioc.run();
    }
    catch (std::exception const& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
