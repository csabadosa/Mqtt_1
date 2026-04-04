#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <array>
#include <vector>
#include <unordered_map>

using boost::asio::ip::tcp;

class Session;

std::unordered_map<std::string, std::vector<std::weak_ptr<Session>>> subscriptions_;

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

    void do_read() {
        auto self = shared_from_this();

        socket_.async_read_some(
            boost::asio::buffer(temp_buffer_),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    // Append to persistent buffer
                    read_buffer_.insert(
                        read_buffer_.end(),
                        temp_buffer_.begin(),
                        temp_buffer_.begin() + length
                    );

                    process_buffer(); // <-- NEW
                    do_read();
                }
            }
        );
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

        default:
            std::cout << "Unhandled packet\n";
            break;
        }
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
            subscriptions_[topic].push_back(shared_from_this());

            return_codes.push_back(qos); // granted QoS
        }

        send_suback(packet_id, return_codes);
    }

    void send_suback(uint16_t packet_id, const std::vector<uint8_t>& return_codes) {
        std::vector<uint8_t> response;

        response.push_back(0x90); // SUBACK

        int remaining_length = 2 + return_codes.size();
        response.push_back(remaining_length);

        response.push_back(packet_id >> 8);
        response.push_back(packet_id & 0xFF);

        response.insert(response.end(), return_codes.begin(), return_codes.end());

        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(response),
            [this, self](boost::system::error_code ec, std::size_t) {}
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

        std::cout << "PUBLISH topic: " << topic << std::endl;

        route_message(topic, payload);
    }

    void route_message(const std::string& topic,
        const std::vector<uint8_t>& payload) {

        if (subscriptions_.find(topic) == subscriptions_.end())
            return;

        for (auto& weak_sub : subscriptions_[topic]) {
            if (auto sub = weak_sub.lock()) {
                sub->send_publish(topic, payload);
            }
        }
    }


    void send_publish(const std::string& topic,
        const std::vector<uint8_t>& payload) {

        std::vector<uint8_t> packet;

        packet.push_back(0x30); // PUBLISH QoS 0

        int remaining_length = 2 + topic.size() + payload.size();
        packet.push_back(remaining_length);

        // Topic length
        packet.push_back(topic.size() >> 8);
        packet.push_back(topic.size() & 0xFF);

        // Topic
        packet.insert(packet.end(), topic.begin(), topic.end());

        // Payload
        packet.insert(packet.end(), payload.begin(), payload.end());

        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(packet),
            [this, self](boost::system::error_code ec, std::size_t) {}
        );
    }
   

    void handle_connect(const std::vector<uint8_t>& packet) {
        std::cout << "CONNECT received (full packet)\n";

        uint8_t connack[] = { 0x20, 0x02, 0x00, 0x00 };

        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(connack, sizeof(connack)),
            [this, self](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    std::cout << "CONNACK sent\n";
                }
            }
        );
    }

    void handle_publish(const std::vector<uint8_t>& packet) {
        std::cout << "PUBLISH packet received\n";

        // You’ll parse:
        // - topic length (2 bytes)
        // - topic string
        // - payload

        // For now just log raw data
    }

    void do_write(std::size_t length) {
        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(read_buffer_, length),
            [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                if (!ec) {
                    do_read(); // keep reading more data
                }
            }
        );
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