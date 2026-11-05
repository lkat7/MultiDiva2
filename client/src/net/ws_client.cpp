#include "ws_client.h"
#include "../core/log.h"

#include <windows.h>
#include <winhttp.h>

namespace md::net {

WsClient::~WsClient()
{
    Close();
}

void WsClient::Connect(const std::wstring& host, uint16_t port, const std::wstring& path, bool secure)
{
    Close();
    m_stop = false;
    m_status = Status::Connecting;
    {
        std::lock_guard lock(m_mutex);
        m_lastError.clear();
        m_outbox.clear();
    }
    m_thread = std::thread(&WsClient::Run, this, host, port, path, secure);
}

void WsClient::Close()
{
    m_stop = true;
    m_sendCv.notify_all();
    HINTERNET socket;
    {
        std::lock_guard lock(m_mutex);
        socket = m_socket;
    }
    // Sends our close frame without waiting; the server's reply ends the pending receive.
    if (socket)
        WinHttpWebSocketShutdown(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    if (m_thread.joinable())
        m_thread.join();
    m_status = Status::Disconnected;
}

void WsClient::Send(std::string message)
{
    if (m_status != Status::Connected)
        return;
    {
        std::lock_guard lock(m_mutex);
        m_outbox.push_back(std::move(message));
    }
    m_sendCv.notify_one();
}

size_t WsClient::Backlog() const
{
    std::lock_guard lock(m_mutex);
    return m_outbox.size();
}

void WsClient::Poll(std::vector<std::string>& out)
{
    std::lock_guard lock(m_mutex);
    for (auto& msg : m_inbox)
        out.push_back(std::move(msg));
    m_inbox.clear();
}

std::string WsClient::LastError() const
{
    std::lock_guard lock(m_mutex);
    return m_lastError;
}

void WsClient::Fail(const char* what, unsigned long code)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s failed (error %lu)", what, code);
    {
        std::lock_guard lock(m_mutex);
        if (m_lastError.empty())
            m_lastError = buf;
    }
    if (!m_stop)
        Log("net: %s", buf);
}

void WsClient::Cleanup()
{
    std::lock_guard lock(m_mutex);
    if (m_socket) WinHttpCloseHandle(m_socket);
    if (m_connect) WinHttpCloseHandle(m_connect);
    if (m_session) WinHttpCloseHandle(m_session);
    m_socket = m_connect = m_session = nullptr;
}

void WsClient::Run(std::wstring host, uint16_t port, std::wstring path, bool secure)
{
    HINTERNET session = WinHttpOpen(L"MultiDiva", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host.c_str(), port, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    {
        std::lock_guard lock(m_mutex);
        m_session = session;
        m_connect = connect;
    }
    if (!request) {
        Fail("connect", GetLastError());
        Cleanup();
        m_status = Status::Disconnected;
        return;
    }

    // The receive timeout also bounds WebSocket receives; the server sends a `ping` every 10 s,
    // so 30 s of silence means the connection is dead.
    WinHttpSetTimeouts(request, 5000, 5000, 5000, 30000);
    HINTERNET socket = nullptr;
    if (WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
        && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr)) {
        socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    }
    DWORD upgradeError = GetLastError();
    WinHttpCloseHandle(request);

    if (!socket || m_stop) {
        if (!m_stop)
            Fail("connect", upgradeError);
        if (socket)
            WinHttpCloseHandle(socket);
        Cleanup();
        m_status = Status::Disconnected;
        return;
    }

    {
        std::lock_guard lock(m_mutex);
        m_socket = socket;
    }
    m_status = Status::Connected;
    Log("net: connected");
    m_sendThread = std::thread(&WsClient::SendLoop, this);

    // Receive loop: blocks in WinHttpWebSocketReceive until data arrives or the socket closes.
    std::string message;
    std::vector<char> buffer(16 * 1024);
    while (!m_stop) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD err = WinHttpWebSocketReceive(socket, buffer.data(), (DWORD)buffer.size(), &read, &type);
        if (err != ERROR_SUCCESS) {
            Fail("receive", err);
            break;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            Fail("server closed the connection", 0);
            break;
        }
        message.append(buffer.data(), read);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            std::lock_guard lock(m_mutex);
            m_inbox.push_back(std::move(message));
            message.clear();
        } else if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
            message.clear(); // binary frames are not part of the protocol
        }
    }

    m_status = Status::Disconnected;
    m_stop = true;
    m_sendCv.notify_all();
    if (m_sendThread.joinable())
        m_sendThread.join();
    Cleanup();
    Log("net: disconnected");
}

void WsClient::SendLoop()
{
    for (;;) {
        std::string msg;
        {
            std::unique_lock lock(m_mutex);
            m_sendCv.wait(lock, [&] { return m_stop || !m_outbox.empty(); });
            if (m_stop)
                return;
            msg = std::move(m_outbox.front());
            m_outbox.pop_front();
        }
        // A send and a receive may be outstanding at the same time on one WinHTTP WebSocket.
        DWORD err = WinHttpWebSocketSend(m_socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, msg.data(), (DWORD)msg.size());
        if (err != ERROR_SUCCESS) {
            Fail("send", err);
            return;
        }
    }
}

} // namespace md::net
