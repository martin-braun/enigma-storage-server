//
// Copyright (c) 2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

//------------------------------------------------------------------------------
//
// Example: HTTP server, small
//
//------------------------------------------------------------------------------
#include "Storage.hpp"
#include "pow.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <openssl/sha.h>
#include <string>
#include <unordered_map>

using tcp = boost::asio::ip::tcp;    // from <boost/asio.hpp>
namespace http = boost::beast::http; // from <boost/beast/http.hpp>
using namespace service_node;

class http_connection : public std::enable_shared_from_this<http_connection> {
  public:
    http_connection(tcp::socket socket, Storage& storage)
        : socket_(std::move(socket)), storage_(storage) {}

    // Initiate the asynchronous operations associated with the connection.
    void start() {
        read_request();
        check_deadline();
    }

  private:
    // The socket for the currently connected client.
    tcp::socket socket_;

    // The buffer for performing reads.
    boost::beast::flat_buffer buffer_{8192};

    // The request message.
    http::request<http::dynamic_body> request_;

    // The response message.
    http::response<http::dynamic_body> response_;

    // The timer for putting a deadline on connection processing.
    boost::asio::basic_waitable_timer<std::chrono::steady_clock> deadline_{
        socket_.get_executor().context(), std::chrono::seconds(60)};

    std::unordered_map<std::string, std::string> header_;
    Storage& storage_;

    // Asynchronously receive a complete request message.
    void read_request() {
        auto self = shared_from_this();

        http::async_read(
            socket_, buffer_, request_,
            [self](boost::beast::error_code ec, std::size_t bytes_transferred) {
                boost::ignore_unused(bytes_transferred);
                if (!ec) {
                    self->process_request();
                    self->write_response();
                }
            });
    }

    template <typename T> bool parse_header(T key_list) {
        for (const auto key : key_list) {
            const auto it = request_.find(key);
            if (it == request_.end()) {
                response_.result(http::status::bad_request);
                response_.set(http::field::content_type, "text/plain");
                boost::beast::ostream(response_.body())
                    << "Missing field in header : " << key;
                return false;
            }
            header_[key] = it->value().to_string();
        }
        return true;
    }

    void process_retrieve() {
        const std::vector<std::string> keys = {"pubkey"};
        if (!parse_header(keys))
            return;

        // optional lastHash
        std::string last_hash = "";
        const auto it = request_.find("last_hash");
        if (it != request_.end()) {
            last_hash = it->value().to_string();
        }

        std::vector<storage::Item> items;
        std::string body = "{\"messages\": [";

        try {
            storage_.retrieve(header_["pubkey"], items, last_hash);
        } catch (std::exception e) {
            response_.result(http::status::internal_server_error);
            response_.set(http::field::content_type, "text/plain");
            boost::beast::ostream(response_.body()) << e.what();
            return;
        }

        for (const auto& item : items) {
            body += "{";
            body += "\"hash\":\"" + item.hash + "\",";
            body += "\"timestamp\":\"" + std::to_string(item.timestamp) + "\",";
            body += "\"data\":\"";
            body.append(std::begin(item.bytes), std::end(item.bytes));
            body += "\"";
            body += "},";
        }
        body.pop_back();
        body += "]}";
        response_.result(http::status::ok);
        response_.set(http::field::content_type, "application/json");
        boost::beast::ostream(response_.body()) << body;
    }

    void process_store() {
        const std::vector<std::string> keys = {"X-Loki-pow-nonce", "X-Loki-ttl",
                                               "X-Loki-timestamp",
                                               "X-Loki-recipient"};
        if (!parse_header(keys))
            return;

        std::vector<uint8_t> bytes;

        for (auto seq : request_.body().data()) {
            const auto* cbuf = boost::asio::buffer_cast<const char*>(seq);
            bytes.insert(std::end(bytes), cbuf,
                         cbuf + boost::asio::buffer_size(seq));
        }
        // Do not store message if the PoW provided is invalid
        const bool validPoW =
            checkPoW(header_["X-Loki-pow-nonce"], header_["X-Loki-timestamp"],
                     header_["X-Loki-ttl"], header_["X-Loki-recipient"], bytes);
        if (!validPoW) {
            response_.result(http::status::forbidden);
            response_.set(http::field::content_type, "text/plain");
            boost::beast::ostream(response_.body())
                << "Provided PoW nonce is not valid.";
            return;
        }

        const int ttl = std::stoi(header_["X-Loki-ttl"]);
        bool success;

        unsigned char hashResult[SHA512_DIGEST_LENGTH];
        std::vector<unsigned char> messageContents(
            header_["X-Loki-timestamp"].size() +
            header_["X-Loki-pow-nonce"].size() +
            header_["X-Loki-recipient"].size() + bytes.size());
        messageContents.insert(std::end(messageContents),
                               std::begin(header_["X-Loki-timestamp"]),
                               std::end(header_["X-Loki-timestamp"]));
        messageContents.insert(std::end(messageContents),
                               std::begin(header_["X-Loki-pow-nonce"]),
                               std::end(header_["X-Loki-pow-nonce"]));
        messageContents.insert(std::end(messageContents),
                               std::begin(header_["X-Loki-recipient"]),
                               std::end(header_["X-Loki-recipient"]));
        messageContents.insert(std::end(messageContents), std::begin(bytes),
                               std::end(bytes));
        SHA512(messageContents.data(), messageContents.size(), hashResult);

        char hash[SHA512_DIGEST_LENGTH * 2 + 1];
        for (int i = 0; i < SHA512_DIGEST_LENGTH; i++)
            sprintf(&hash[i * 2], "%02x", (unsigned int)hashResult[i]);

        try {
            // TODO: Calculate hash and store instead of timestamp
            success =
                storage_.store(hash, header_["X-Loki-recipient"], bytes, ttl);
        } catch (std::exception e) {
            response_.result(http::status::internal_server_error);
            response_.set(http::field::content_type, "text/plain");
            boost::beast::ostream(response_.body()) << e.what();
            return;
        }

        if (!success) {
            response_.result(http::status::conflict);
            response_.set(http::field::content_type, "text/plain");
            boost::beast::ostream(response_.body())
                << "hash conflict - resource already present.";
            return;
        }

        response_.result(http::status::ok);
        response_.set(http::field::content_type, "application/json");
        boost::beast::ostream(response_.body()) << "{ \"status\": \"ok\" }";
    }

    // Determine what needs to be done with the request message.
    void process_request() {
        response_.version(request_.version());
        response_.keep_alive(false);

        const auto target = request_.target();
        switch (request_.method()) {
        case http::verb::get:
            if (target != "/retrieve") {
                response_.result(http::status::not_found);
                break;
            }

            process_retrieve();
            break;
        case http::verb::post:
            if (target != "/store") {
                response_.result(http::status::not_found);
                break;
            }

            process_store();
            break;

        default:
            response_.result(http::status::bad_request);
            break;
        }
    }

    // Asynchronously transmit the response message.
    void write_response() {
        auto self = shared_from_this();

        response_.set(http::field::content_length, response_.body().size());

        http::async_write(socket_, response_,
                          [self](boost::beast::error_code ec, std::size_t) {
                              self->socket_.shutdown(tcp::socket::shutdown_send,
                                                     ec);
                              self->deadline_.cancel();
                          });
    }

    // Check whether we have spent enough time on this connection.
    void check_deadline() {
        auto self = shared_from_this();

        deadline_.async_wait([self](boost::beast::error_code ec) {
            if (!ec) {
                // Close socket to cancel any outstanding operation.
                self->socket_.close(ec);
            }
        });
    }
};

// "Loop" forever accepting new connections.
void http_server(tcp::acceptor& acceptor, tcp::socket& socket,
                 Storage& storage) {
    acceptor.async_accept(socket, [&](boost::beast::error_code ec) {
        if (!ec)
            std::make_shared<http_connection>(std::move(socket), storage)
                ->start();
        http_server(acceptor, socket, storage);
    });
}
