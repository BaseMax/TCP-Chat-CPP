#include <algorithm>
#include <string>
#include <string_view>
#include <list>
#include <print>
#include <format>
#include <optional>
#include <concepts>
#include <ranges>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <vector>

constexpr std::string_view HOSTNAME = "127.0.0.1";
constexpr int PORT = 3000;
constexpr size_t BUFFER_SIZE = 1024;

template<typename T>
concept SocketType = std::is_integral_v<T> && requires(T s) {
    { close(s) } -> std::same_as<int>;
    { send(s, "", 0, 0) } -> std::same_as<ssize_t>;
    { recv(s, nullptr, 0, 0) } -> std::same_as<ssize_t>;
};

template<typename T>
concept ClientContainer = requires(T t, int socket, std::string nick) {
    { t.emplace_back(socket, std::move(nick)) };
    { t.remove_if([](const auto&) { return true; }) };
    requires std::ranges::range<T>;
};

template<SocketType SocketT = int>
struct Client {
    SocketT socket;
    std::string nickname;
};

template<SocketType SocketT = int, ClientContainer Container = std::list<Client<SocketT>>>
class ChatServer {
private:
    Container clients;
    SocketT server_socket;
    fd_set master_set;
    SocketT max_fd;

public:
    ChatServer() {
        setup_server();
    }

    ~ChatServer() {
        for (const auto& client : clients) {
            close(client.socket);
        }
        close(server_socket);
    }

    void run() {
        while (true) {
            fd_set read_fds = master_set;
            if (select(max_fd + 1, &read_fds, nullptr, nullptr, nullptr) < 0) {
                if (errno == EINTR) continue;
                std::print(stderr, "select error: {}\n", strerror(errno));
                continue;
            }

            for (SocketT fd : std::views::iota(0, max_fd + 1)) {
                if (FD_ISSET(fd, &read_fds)) {
                    if (fd == server_socket) {
                        handle_new_connection();
                    } else {
                        handle_client_data(fd);
                    }
                }
            }
        }
    }

private:
    void setup_server() {
        server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket < 0) {
            throw std::runtime_error(std::format("socket failed: {}", strerror(errno)));
        }

        int opt = 1;
        if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            throw std::runtime_error(std::format("setsockopt failed: {}", strerror(errno)));
        }

        sockaddr_in server_addr{
            .sin_family = AF_INET,
            .sin_port = htons(PORT),
            .sin_addr = {inet_addr(HOSTNAME.data())}
        };

        if (bind(server_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
            throw std::runtime_error(std::format("bind failed: {}", strerror(errno)));
        }

        if (listen(server_socket, 10) < 0) {
            throw std::runtime_error(std::format("listen failed: {}", strerror(errno)));
        }

        FD_ZERO(&master_set);
        FD_SET(server_socket, &master_set);
        max_fd = server_socket;

        std::print("🚀 Server running on {}:{}\n", HOSTNAME, PORT);
    }

    void broadcast(SocketT sender_fd, std::string_view message) {
        for (const auto& client : clients | std::views::filter([sender_fd](const auto& c) {
                                      return c.socket != sender_fd;
                                  })) {
            ssize_t sent = send(client.socket, message.data(), message.size(), MSG_NOSIGNAL);
            if (sent < 0 && errno != EPIPE) {
                std::print(stderr, "broadcast send failed: {}\n", strerror(errno));
            }
        }
    }

    std::optional<Client<SocketT>*> find_client(SocketT socket) {
        auto it = std::ranges::find_if(clients, [socket](const auto& c) { return c.socket == socket; });
        return it != clients.end() ? std::optional(&*it) : std::nullopt;
    }

    bool nickname_exists(std::string_view nickname) {
        return std::ranges::any_of(clients, [nickname](const auto& c) { return c.nickname == nickname; });
    }

    void send_welcome(SocketT socket) {
        std::string online_users = std::ranges::fold_left(
            clients | std::views::filter([socket](const auto& c) {
                return c.socket != socket && !c.nickname.empty();
            }) | std::views::transform(&Client<SocketT>::nickname),
            std::string{},
            [](std::string acc, std::string_view nick) {
                if (acc.empty()) return std::string{nick};
                acc += ", ";
                acc += nick;
                return acc;
            }
            );

        size_t count = std::ranges::count_if(clients, [socket](const auto& c) {
            return c.socket != socket && !c.nickname.empty();
        });

        std::string message = count == 0
                                  ? "🎉 Welcome! You are the only user here.\r\n"
                                  : std::format("🎉 Welcome! {} users online.\r\n👥 Users: {}\r\n", count, online_users);

        if (send(socket, message.data(), message.size(), MSG_NOSIGNAL) < 0 && errno != EPIPE) {
            std::print(stderr, "send welcome failed: {}\n", strerror(errno));
        }
    }

    void register_client(SocketT socket, std::string_view nickname) {
        if (nickname_exists(nickname)) {
            constexpr std::string_view error = "❌ Nickname taken, choose another:\r\n> ";
            send(socket, error.data(), error.size(), MSG_NOSIGNAL);
            return;
        }

        clients.emplace_back(socket, std::string{nickname});
        std::print("👤 Registered: {}\n", nickname);
        send_welcome(socket);

        std::string join_msg = std::format("👋 {} joined the chat\r\n", nickname);
        broadcast(socket, join_msg);
    }

    void remove_client(SocketT socket) {
        if (auto client = find_client(socket)) {
            std::string msg = std::format("👋 {} left the chat\r\n",
                                          (*client)->nickname.empty() ? "unknown" : (*client)->nickname);
            std::print("❌ {} disconnected\n",
                       (*client)->nickname.empty() ? "unknown" : (*client)->nickname);

            broadcast(socket, msg);
            shutdown(socket, SHUT_RDWR);
            close(socket);
            FD_CLR(socket, &master_set);
            clients.remove_if([socket](const auto& c) { return c.socket == socket; });

            if (socket == max_fd) {
                max_fd = server_socket;
                for (const auto& c : clients) {
                    if (c.socket > max_fd) max_fd = c.socket;
                }
            }
        }
    }

    void handle_new_connection() {
        sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        SocketT new_socket = accept(server_socket, reinterpret_cast<sockaddr*>(&client_addr), &addrlen);

        if (new_socket < 0) {
            std::print(stderr, "accept failed: {}\n", strerror(errno));
            return;
        }

        FD_SET(new_socket, &master_set);
        if (new_socket > max_fd) {
            max_fd = new_socket;
        }

        std::print("✅ Connected: {}\n", inet_ntoa(client_addr.sin_addr));
        constexpr std::string_view prompt = "👋 Enter nickname:\r\n> ";
        if (send(new_socket, prompt.data(), prompt.size(), MSG_NOSIGNAL) < 0) {
            std::print(stderr, "send prompt failed: {}\n", strerror(errno));
            close(new_socket);
            FD_CLR(new_socket, &master_set);
        }
    }

    void handle_client_data(SocketT socket) {
        std::vector<char> buffer(BUFFER_SIZE);
        ssize_t bytes = recv(socket, buffer.data(), buffer.size() - 1, 0);

        if (bytes < 0) {
            if (errno == ECONNRESET || errno == EPIPE) {
                std::print("Connection reset by peer (socket {})\n", socket);
            } else {
                std::print(stderr, "recv error: {}\n", strerror(errno));
            }
            remove_client(socket);
            return;
        }

        if (bytes == 0) {
            std::print("Client {} closed connection\n", socket);
            remove_client(socket);
            return;
        }

        buffer[bytes] = '\0';  // Null terminate
        std::string_view message(buffer.data(), static_cast<size_t>(bytes));
        auto end_pos = message.find_first_of("\r\n");
        if (end_pos != std::string_view::npos) {
            message = message.substr(0, end_pos);
        }

        if (message.empty()) {
            return;
        }

        if (auto client = find_client(socket); !client) {
            register_client(socket, message);
        } else {
            std::string broadcast_msg = std::format("💬 {}: {}\r\n", (*client)->nickname, message);
            std::print("📢 {}: {}\n", (*client)->nickname, message);
            broadcast(socket, broadcast_msg);
        }
    }
};

int main() {
    try {
        ChatServer<> server;
        server.run();
    } catch (const std::exception& e) {
        std::print(stderr, "Fatal error: {}\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
