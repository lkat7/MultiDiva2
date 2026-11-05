#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace md::net {

// WebSocket client on WinHTTP. All network I/O runs on its own threads; the game thread only
// queues outgoing messages and drains incoming ones, so it never blocks on the network.
class WsClient {
public:
    enum class Status { Disconnected, Connecting, Connected };

    ~WsClient();

    // Starts connecting to ws(s)://host:port/path in the background. Closes any previous connection.
    void Connect(const std::wstring& host, uint16_t port, const std::wstring& path, bool secure);
    void Close();

    // Queues a text message. Dropped if not connected.
    void Send(std::string message);

    // Moves received text messages into `out`.
    void Poll(std::vector<std::string>& out);

    // Messages queued but not yet written to the socket: grows when the connection can't keep up.
    size_t Backlog() const;

    Status GetStatus() const { return m_status; }
    std::string LastError() const;

private:
    void Run(std::wstring host, uint16_t port, std::wstring path, bool secure);
    void SendLoop();
    void Fail(const char* what, unsigned long code);
    void Cleanup();

    std::atomic<Status> m_status{Status::Disconnected};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::thread m_sendThread;

    void* m_session = nullptr; // HINTERNET
    void* m_connect = nullptr;
    void* m_socket = nullptr;

    mutable std::mutex m_mutex;
    std::condition_variable m_sendCv;
    std::deque<std::string> m_outbox;
    std::vector<std::string> m_inbox;
    std::string m_lastError;
};

} // namespace md::net
