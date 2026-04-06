#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <array>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "Trie.h"

using boost::asio::ip::tcp;

class Session;

Trie topic_trie_;
std::mutex trie_mutex_;



std::mutex clients_mutex_;
std::unordered_map<std::string, std::weak_ptr<Session>> clients_;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket)
        : socket_(std::move(socket)) {
    }

    void start() { do_read(); }

private:
    tcp::socket socket_;
    std::vector<uint8_t> read_buffer_;
    std::array<uint8_t, 1024> temp_buffer_;
    std::string client_id_;

    void do_read() {
        auto self = shared_from_this();

        socket_.async_read_some(
            boost::asio::buffer(temp_buffer_),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (ec) {
                    std::cout << "Client disconnected: " << client_id_ << std::endl;
                    unregister_client();
                    return;
                }

                // Append to persistent buffer
                read_buffer_.insert(
                    read_buffer_.end(),
                    temp_buffer_.begin(),
                    temp_buffer_.begin() + length
                );

                process_buffer();
                do_read();
            }
        );
    }

    std::vector<std::string> split_topic(const std::string& topics) {
        std::vector<std::string> result;
        std::stringstream ss(topics);
        std::string item;

        while (std::getline(ss, item, ',')) {
            result.push_back(item);
		}

        return result;
    }

    void disconnect_ack() {
        std::cout << "Client Disconnected" << std::endl;
    }

    void process_buffer() {
        while (true) {
            if (read_buffer_.size() < 2)
                return;

            // Step 1: decode remaining length
            int multiplier = 1;
            int remaining_length = 0;
            int index = 1;
            uint8_t encodedByte;

            do {
                if (index >= read_buffer_.size())
                    return; // incomplete

                encodedByte = read_buffer_[index++];
                remaining_length += (encodedByte & 127) * multiplier;
                multiplier *= 128;

            } while ((encodedByte & 128) != 0);

            int total_packet_size = index + remaining_length;

            // Step 2: check if full packet is available
            if (read_buffer_.size() < total_packet_size)
                return; // wait for more data

            // Step 3: extract full packet
            std::vector<uint8_t> packet(
                read_buffer_.begin(),
                read_buffer_.begin() + total_packet_size
            );

            // Step 4: remove it from buffer
            read_buffer_.erase(
                read_buffer_.begin(),
                read_buffer_.begin() + total_packet_size
            );

            // Step 5: process packet
            handle_full_packet(packet);
        }
    }

    void handle_full_packet(const std::vector<uint8_t>& packet) {
        uint8_t packet_type = packet[0] >> 4;

        std::cout << "Full packet received. Type: "
            << (int)packet_type
            << ", Size: " << packet.size() << std::endl;

        switch (packet_type) {
        case 1:
            handle_connect(packet);
            break;
        case 3:
            handle_publish(packet);
            break;
        case 8:
            handle_subscribe(packet);
			break;
        case 12: 
            send_pingresp();
            break;
        case 14: 
            disconnect_ack();
            break;
        default:
            std::cout << "Unhandled packet\n";
            break;
        }
    }

    void send_pingresp() {
        auto buf = std::make_shared<std::array<uint8_t, 2>>(
            std::array<uint8_t, 2>{ 0xD0, 0x00 }
        );
        //                          ^^^^  ^^^^
        //                    type 13     remaining length = 0

        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(*buf),
            [this, self, buf](boost::system::error_code ec, std::size_t) {
                if (!ec) std::cout << "PINGRESP sent to: " << client_id_ << std::endl;
            }
        );
    }

    
    void handle_subscribe(const std::vector<uint8_t>& packet) {
        int index = 1;

        // Decode remaining length again (skip it)
        int multiplier = 1;
        int remaining_length = 0;
        uint8_t encodedByte;

        do {
            encodedByte = packet[index++];
            remaining_length += (encodedByte & 127) * multiplier;
            multiplier *= 128;
        } while ((encodedByte & 128) != 0);

        // Packet Identifier (2 bytes)
        uint16_t packet_id = (packet[index] << 8) | packet[index + 1];
        index += 2;

        std::vector<uint8_t> return_codes;

        while (index < packet.size()) {
            // Topic length
            uint16_t topic_len = (packet[index] << 8) | packet[index + 1];
            index += 2;

            std::string topic(packet.begin() + index,
                packet.begin() + index + topic_len);
            index += topic_len;

            uint8_t qos = packet[index++];

            std::cout << "Subscribed to: " << topic << " QoS: " << (int)qos << std::endl;

            // Store subscription
            {
                std::lock_guard<std::mutex> lock(trie_mutex_);
                topic_trie_.subscribe(topic, client_id_);
            }

            return_codes.push_back(qos); // granted QoS
        }
        std::cout << "Sending Suback!" << std::endl;
        send_suback(packet_id, return_codes);
    }

    void send_suback(uint16_t packet_id, const std::vector<uint8_t>& return_codes) {
        auto response = std::make_shared<std::vector<uint8_t>>();

        response->push_back(0x90);
        int remaining_length = 2 + return_codes.size();
        response->push_back(remaining_length);
        response->push_back(packet_id >> 8);
        response->push_back(packet_id & 0xFF);
        response->insert(response->end(), return_codes.begin(), return_codes.end());

        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(*response),
            [this, self, response](boost::system::error_code ec, std::size_t) {}
            //                ^^^^ shared_ptr keeps buffer alive until callback fires
        );
    }

    void handle_publish(const std::vector<uint8_t>& packet) {
        int index = 1;

        // Decode remaining length
        int multiplier = 1;
        int remaining_length = 0;
        uint8_t encodedByte;

        do {
            encodedByte = packet[index++];
            remaining_length += (encodedByte & 127) * multiplier;
            multiplier *= 128;
        } while ((encodedByte & 128) != 0);

        // Topic length
        uint16_t topic_len = (packet[index] << 8) | packet[index + 1];
        index += 2;

        std::string topic(packet.begin() + index,
            packet.begin() + index + topic_len);
        index += topic_len;

        // Payload
        std::vector<uint8_t> payload(packet.begin() + index, packet.end());
        std::string message(payload.begin(), payload.end());

        std::cout << "PUBLISH topic: " << topic << std::endl;

        std::cout << "PUBLISH received from client " << client_id_
            << " on topic: " << topic
            << " message: " << message << std::endl;

        route_message(topic, payload);
    }

    void route_message(const std::string& topic, const std::vector<uint8_t>& payload) {
        std::set<std::string> matched_client_ids;

        {
            std::lock_guard<std::mutex> lock(trie_mutex_);
			topic_trie_.match_topic(topic, matched_client_ids);
        }

        for (const auto& id : matched_client_ids) {
            std::shared_ptr<Session> sub;
            { 
                std::lock_guard<std::mutex> lock(clients_mutex_);
                auto it = clients_.find(id);
                if (it == clients_.end()) continue;

                sub = it->second.lock();
            }
            if (sub) {
                std::cout << "Routing message to client: " << id << std::endl;
                sub->send_publish(topic, payload);
            }
        }
    }


    void send_publish(const std::string& topic, const std::vector<uint8_t>& payload) {
        auto packet = std::make_shared<std::vector<uint8_t>>();

        packet->push_back(0x30);
        int remaining_length = 2 + topic.size() + payload.size();
        packet->push_back(remaining_length);
        packet->push_back(topic.size() >> 8);
        packet->push_back(topic.size() & 0xFF);
        packet->insert(packet->end(), topic.begin(), topic.end());
        packet->insert(packet->end(), payload.begin(), payload.end());

        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(*packet),
            [this, self, packet](boost::system::error_code ec, std::size_t) {}
        );
    }
   

    void handle_connect(const std::vector<uint8_t>& packet) {
        int index = 1;

        // Skip remaining length
        int multiplier = 1;
        uint8_t encodedByte;
        do {
            encodedByte = packet[index++];
            multiplier *= 128;
        } while ((encodedByte & 128) != 0);

        // Skip protocol name
        uint16_t proto_len = (packet[index] << 8) | packet[index + 1];
        index += 2 + proto_len;

        // Skip version + flags
        index += 2;

        // Skip keepalive
        index += 2;

        // --- CLIENT ID ---
        uint16_t client_id_len = (packet[index] << 8) | packet[index + 1];
        index += 2;

        std::string client_id(packet.begin() + index,
            packet.begin() + index + client_id_len);

        client_id_ = client_id;

        std::cout << "Client connected: " << client_id << std::endl;

        register_client();

        send_connack();
    }

    void send_connack() {
        uint8_t connack[] = {
            0x20, // CONNACK packet type
            0x02, // remaining length
            0x00, // connect acknowledge flags
            0x00  // return code (0 = success)
        };

        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(connack, sizeof(connack)),
            [this, self](boost::system::error_code ec, std::size_t) {
                if(ec) {
                    std::cout << "Failed to send CONNACK\n";
                    return;
                }
                
                std::cout << "CONNACK sent to client: "
                    << client_id_ << std::endl;
            }
        );
    }

    void register_client() {
        std::lock_guard<std::mutex> lock(clients_mutex_);

        // If same client ID exists → replace it
        if (clients_.count(client_id_)) {
            std::cout << "Client ID already exists, replacing: "
                << client_id_ << std::endl;

        }

        clients_[client_id_] = shared_from_this();
    }

    void unregister_client() {
        std::lock_guard<std::mutex> lock(clients_mutex_);

        if (!client_id_.empty()) {
            clients_.erase(client_id_);
        }
    }
};

class Server {
public:
    Server(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }

private:
    tcp::acceptor acceptor_;

    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket))->start();
                }
                do_accept(); // keep accepting new connections
            }
        );
    }
};

int main() {
    try {
        boost::asio::io_context io_context;
        Server server(io_context, 1883); 
        std::cout << "Server running on port 1883..." << std::endl;
        io_context.run();
    }
    catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}