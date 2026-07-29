#include "UEApiClient/UEApiClient.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <process.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ue::api
{
namespace
{
using json = nlohmann::json;

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(Socket socket)
{
    if (socket != kInvalidSocket)
    {
        closesocket(socket);
    }
}
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void CloseSocket(Socket socket)
{
    if (socket != kInvalidSocket)
    {
        close(socket);
    }
}
#endif

bool SetNonBlocking(const Socket socket, const bool enabled)
{
#if defined(_WIN32)
    u_long mode = enabled ? 1UL : 0UL;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0
        && fcntl(
               socket,
               F_SETFL,
               enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

bool ConnectWouldBlock()
{
#if defined(_WIN32)
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK
        || error == WSAEINPROGRESS
        || error == WSAEALREADY;
#else
    return errno == EINPROGRESS
        || errno == EWOULDBLOCK
        || errno == EAGAIN;
#endif
}

struct ParsedEndpoint
{
    std::string host;
    std::string port;
};

std::optional<ParsedEndpoint> ParseEndpoint(std::string_view endpoint)
{
    constexpr std::string_view prefix = "http://";
    if (!endpoint.starts_with(prefix))
    {
        return std::nullopt;
    }
    endpoint.remove_prefix(prefix.size());
    const auto colon = endpoint.rfind(':');
    if (colon == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::string host(endpoint.substr(0, colon));
    const std::string port(endpoint.substr(colon + 1));
    if ((host != "127.0.0.1" && host != "localhost" && host != "::1")
        || port.empty())
    {
        return std::nullopt;
    }
    int parsed_port = 0;
    const auto parsed = std::from_chars(
        port.data(),
        port.data() + port.size(),
        parsed_port);
    if (parsed.ec != std::errc{}
        || parsed.ptr != port.data() + port.size()
        || parsed_port <= 0
        || parsed_port > 65535)
    {
        return std::nullopt;
    }
    return ParsedEndpoint{ host, port };
}

std::string Lower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string Trim(std::string value)
{
    const auto not_space = [](const unsigned char character)
    {
        return !std::isspace(character);
    };
    const auto begin = std::find_if(value.begin(), value.end(), not_space);
    const auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
    if (begin >= end)
    {
        return {};
    }
    return std::string(begin, end);
}

bool IsHeaderName(const std::string_view name)
{
    if (name.empty())
    {
        return false;
    }
    for (const unsigned char character : name)
    {
        if (!std::isalnum(character)
            && character != '-'
            && character != '_')
        {
            return false;
        }
    }
    return true;
}

bool IsHeaderValue(const std::string_view value)
{
    return std::none_of(
        value.begin(),
        value.end(),
        [](const unsigned char character)
        {
            return character == '\r'
                || character == '\n'
                || character == 0x7f
                || character < 0x20;
        });
}

bool IsExpiredClientSessionResponse(const HttpResult& response)
{
    if (response.status != 401 && response.status != 404)
    {
        return false;
    }
    const json envelope =
        json::parse(response.body, nullptr, false, true);
    if (!envelope.is_object()
        || !envelope.contains("error")
        || !envelope["error"].is_object())
    {
        return false;
    }
    const std::string code =
        envelope["error"].value("code", std::string{});
    return code == "client_session_expired"
        || code == "client_session_not_found";
}

std::optional<std::size_t> HeaderEnd(const std::string& response)
{
    const auto position = response.find("\r\n\r\n");
    return position == std::string::npos
        ? std::nullopt
        : std::optional<std::size_t>(position + 4);
}

bool ParseUnsignedHex(std::string_view text, std::size_t& value)
{
    value = 0;
    const auto semicolon = text.find(';');
    text = text.substr(0, semicolon);
    while (!text.empty()
        && std::isspace(static_cast<unsigned char>(text.front())))
    {
        text.remove_prefix(1);
    }
    while (!text.empty()
        && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.remove_suffix(1);
    }
    if (text.empty())
    {
        return false;
    }
    const auto parsed = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value,
        16);
    return parsed.ec == std::errc{}
        && parsed.ptr == text.data() + text.size();
}

} // namespace

struct Client::Impl
{
    explicit Impl(std::string endpoint, const std::uint32_t timeout_ms)
        : endpoint(std::move(endpoint))
        , timeout_ms(timeout_ms)
        , parsed(ParseEndpoint(this->endpoint))
    {
#if defined(_WIN32)
        runtime_available =
            WSAStartup(MAKEWORD(2, 2), &winsock_data) == 0;
#endif
    }

    ~Impl()
    {
        UnregisterBestEffortSession();
        Disconnect();
#if defined(_WIN32)
        if (runtime_available)
        {
            WSACleanup();
        }
#endif
    }

    void ConfigureBestEffortCliSession(CliSessionOptions options)
    {
        if (options.invocation_id.empty())
        {
            options.invocation_id = NewInvocationId();
        }
        if (options.instance_id.empty())
        {
            options.instance_id = options.invocation_id;
        }
        if (options.process_id == 0)
        {
            options.process_id = CurrentProcessId();
        }

        session = CliSessionState{ std::move(options) };
        const auto& identity = session->options;
        request_headers["X-UEAI-Caller-Type"] = "cli";
        request_headers["X-UEAI-Caller"] = identity.name;
        request_headers["X-UEAI-Caller-Version"] = identity.version;
        request_headers["X-UEAI-Invocation-Id"] =
            identity.invocation_id;
        request_headers["X-UEAI-Instance-Id"] = identity.instance_id;
        request_headers["X-UEAI-Process-Id"] =
            std::to_string(identity.process_id);
        request_headers["X-UEAI-Transport"] = "http";
        request_headers["X-UEAI-Command"] = identity.command;
        request_headers.erase("X-UEAI-Session-Id");
    }

    void EnsureBestEffortSession()
    {
        if (!session
            || session->protocol_unsupported
            || !session->session_id.empty())
        {
            return;
        }
        const auto& identity = session->options;
        const json request = {
            { "clientKind", "cli" },
            { "name", identity.name },
            { "version", identity.version },
            { "transport", "http" },
            { "pid", identity.process_id },
            { "instanceId", identity.instance_id },
            { "invocationId", identity.invocation_id },
            { "command", identity.command },
        };
        const HttpResult response = PerformRaw(
            "POST",
            "/api/v1/clients/register",
            request.dump());
        if (!response.ok)
        {
            if (response.status == 404)
            {
                session->protocol_unsupported = true;
            }
            // A legacy endpoint may reject the route without consuming its
            // request body. Reconnect before the real request so fallback
            // cannot inherit a desynchronized keep-alive stream.
            Disconnect();
            return;
        }
        const json envelope =
            json::parse(response.body, nullptr, false, true);
        if (!envelope.is_object()
            || !envelope.value("ok", false)
            || !envelope.contains("data")
            || !envelope["data"].is_object())
        {
            Disconnect();
            return;
        }
        const auto& data = envelope["data"];
        const std::string session_id =
            data.value("sessionId", std::string{});
        if (session_id.empty())
        {
            Disconnect();
            return;
        }
        session->session_id = session_id;
        request_headers["X-UEAI-Session-Id"] = session_id;
    }

    void UnregisterBestEffortSession()
    {
        if (!session || session->session_id.empty())
        {
            return;
        }
        Impl control_connection(
            endpoint,
            std::min<std::uint32_t>(timeout_ms, 1000U));
        control_connection.request_headers = request_headers;
        (void)control_connection.PerformRaw(
            "POST",
            "/api/v1/clients/unregister",
            "{}");
        request_headers.erase("X-UEAI-Session-Id");
        session->session_id.clear();
    }

    void Disconnect()
    {
        CloseSocket(socket);
        socket = kInvalidSocket;
    }

    bool ApplyTimeouts(const Socket candidate) const
    {
#if defined(_WIN32)
        const DWORD value = timeout_ms;
        return setsockopt(
                   candidate,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&value),
                   sizeof(value)) == 0
            && setsockopt(
                   candidate,
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&value),
                   sizeof(value)) == 0;
#else
        const timeval value{
            static_cast<time_t>(timeout_ms / 1000U),
            static_cast<suseconds_t>((timeout_ms % 1000U) * 1000U),
        };
        return setsockopt(
                   candidate,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   &value,
                   sizeof(value)) == 0
            && setsockopt(
                   candidate,
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   &value,
                   sizeof(value)) == 0;
#endif
    }

    bool Connect(std::string& error)
    {
        if (socket != kInvalidSocket)
        {
            return true;
        }
        if (!parsed)
        {
            error =
                "Endpoint must be loopback HTTP, for example "
                "http://127.0.0.1:9847.";
            return false;
        }
        if (!runtime_available)
        {
            error = "Socket runtime initialization failed.";
            return false;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        if (getaddrinfo(
                parsed->host.c_str(),
                parsed->port.c_str(),
                &hints,
                &addresses) != 0)
        {
            error = "Could not resolve the loopback endpoint.";
            return false;
        }

        for (auto* address = addresses; address; address = address->ai_next)
        {
            const Socket candidate = ::socket(
                address->ai_family,
                address->ai_socktype,
                address->ai_protocol);
            if (candidate == kInvalidSocket)
            {
                continue;
            }
            if (!ApplyTimeouts(candidate))
            {
                CloseSocket(candidate);
                continue;
            }
            if (!SetNonBlocking(candidate, true))
            {
                CloseSocket(candidate);
                continue;
            }
            const int connect_result = ::connect(
                    candidate,
                    address->ai_addr,
                    static_cast<int>(address->ai_addrlen));
            bool connected = connect_result == 0;
            if (!connected && ConnectWouldBlock())
            {
                fd_set writable{};
                fd_set failed{};
                FD_ZERO(&writable);
                FD_ZERO(&failed);
                FD_SET(candidate, &writable);
                FD_SET(candidate, &failed);
                const std::uint32_t connect_timeout_ms =
                    std::min(timeout_ms, 5U);
                timeval wait{
                    static_cast<long>(connect_timeout_ms / 1000U),
                    static_cast<long>(
                        (connect_timeout_ms % 1000U) * 1000U),
                };
                const int ready = select(
                    static_cast<int>(candidate + 1),
                    nullptr,
                    &writable,
                    &failed,
                    &wait);
                if (ready > 0 && FD_ISSET(candidate, &writable))
                {
                    int socket_error = 0;
#if defined(_WIN32)
                    int error_length = sizeof(socket_error);
#else
                    socklen_t error_length = sizeof(socket_error);
#endif
                    connected =
                        getsockopt(
                            candidate,
                            SOL_SOCKET,
                            SO_ERROR,
#if defined(_WIN32)
                            reinterpret_cast<char*>(&socket_error),
#else
                            &socket_error,
#endif
                            &error_length) == 0
                        && socket_error == 0;
                }
            }
            if (connected && SetNonBlocking(candidate, false))
            {
                socket = candidate;
                break;
            }
            CloseSocket(candidate);
        }
        freeaddrinfo(addresses);

        if (socket == kInvalidSocket)
        {
            error = "Cannot connect to the running Unreal Editor.";
            return false;
        }
        return true;
    }

    bool ReceiveMore(std::string& response, std::string& error)
    {
        std::array<char, 8192> buffer{};
        const auto count =
            ::recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (count <= 0)
        {
            error = "The Editor closed the HTTP connection.";
            Disconnect();
            return false;
        }
        response.append(buffer.data(), static_cast<std::size_t>(count));
        return true;
    }

    bool DecodeChunked(
        std::string buffer,
        std::string& body,
        std::string& error)
    {
        std::size_t cursor = 0;
        for (;;)
        {
            std::size_t line_end = buffer.find("\r\n", cursor);
            while (line_end == std::string::npos)
            {
                if (!ReceiveMore(buffer, error))
                {
                    return false;
                }
                line_end = buffer.find("\r\n", cursor);
            }
            std::size_t chunk_size = 0;
            if (!ParseUnsignedHex(
                    std::string_view(buffer).substr(
                        cursor,
                        line_end - cursor),
                    chunk_size))
            {
                error = "Editor returned invalid chunked HTTP data.";
                Disconnect();
                return false;
            }
            cursor = line_end + 2;
            if (chunk_size == 0)
            {
                return true;
            }
            while (buffer.size() < cursor + chunk_size + 2)
            {
                if (!ReceiveMore(buffer, error))
                {
                    return false;
                }
            }
            body.append(buffer.data() + cursor, chunk_size);
            cursor += chunk_size;
            if (buffer.compare(cursor, 2, "\r\n") != 0)
            {
                error = "Editor returned malformed chunked HTTP data.";
                Disconnect();
                return false;
            }
            cursor += 2;
        }
    }

    HttpResult PerformRaw(
        const std::string_view method,
        const std::string_view path,
        const std::string_view body)
    {
        std::string connect_error;
        if (!Connect(connect_error))
        {
            return { false, 0, {}, std::move(connect_error) };
        }

        std::ostringstream request;
        request << method << " " << path << " HTTP/1.1\r\n"
                << "Host: " << parsed->host << ":" << parsed->port << "\r\n"
                << "Accept: application/json\r\n"
                << "Connection: keep-alive\r\n";
        for (const auto& [name, value] : request_headers)
        {
            request << name << ": " << value << "\r\n";
        }
        if (method == "POST")
        {
            request << "Content-Type: application/json\r\n"
                    << "Content-Length: " << body.size() << "\r\n";
        }
        request << "\r\n";
        if (method == "POST")
        {
            request << body;
        }
        const std::string request_text = request.str();

        std::size_t sent = 0;
        while (sent < request_text.size())
        {
            const auto count = ::send(
                socket,
                request_text.data() + sent,
                static_cast<int>(request_text.size() - sent),
                0);
            if (count <= 0)
            {
                const bool retryable = sent == 0;
                Disconnect();
                if (retryable && Connect(connect_error))
                {
                    continue;
                }
                return {
                    false,
                    0,
                    {},
                    retryable
                        ? "Failed to send the HTTP request."
                        : "HTTP request was interrupted while sending.",
                };
            }
            sent += static_cast<std::size_t>(count);
        }

        std::string response;
        std::string receive_error;
        while (!HeaderEnd(response))
        {
            if (!ReceiveMore(response, receive_error))
            {
                return { false, 0, {}, std::move(receive_error) };
            }
        }

        const std::size_t header_end = *HeaderEnd(response);
        const std::string header_text = response.substr(0, header_end);
        const auto first_line_end = header_text.find("\r\n");
        const std::string status_line =
            header_text.substr(0, first_line_end);
        const bool http_10 =
            status_line.starts_with("HTTP/1.0");
        const auto first_space = status_line.find(' ');
        if (first_space == std::string::npos
            || first_space + 4 > status_line.size())
        {
            Disconnect();
            return {
                false,
                0,
                {},
                "Editor returned an invalid HTTP status line.",
            };
        }
        int status = 0;
        const auto status_text =
            std::string_view(status_line).substr(first_space + 1, 3);
        const auto parsed_status = std::from_chars(
            status_text.data(),
            status_text.data() + status_text.size(),
            status);
        if (parsed_status.ec != std::errc{})
        {
            Disconnect();
            return {
                false,
                0,
                {},
                "Editor returned an invalid HTTP status.",
            };
        }

        std::map<std::string, std::string> headers;
        std::size_t line_start = first_line_end + 2;
        while (line_start + 2 < header_end)
        {
            const auto line_end = header_text.find("\r\n", line_start);
            if (line_end == std::string::npos || line_end == line_start)
            {
                break;
            }
            const auto colon = header_text.find(':', line_start);
            if (colon != std::string::npos && colon < line_end)
            {
                headers[Lower(Trim(header_text.substr(
                    line_start,
                    colon - line_start)))] =
                    Trim(header_text.substr(
                        colon + 1,
                        line_end - colon - 1));
            }
            line_start = line_end + 2;
        }

        std::string response_body;
        std::string buffered_body = response.substr(header_end);
        const auto transfer = headers.find("transfer-encoding");
        const auto length = headers.find("content-length");
        if (transfer != headers.end()
            && Lower(transfer->second).find("chunked") != std::string::npos)
        {
            if (!DecodeChunked(
                    std::move(buffered_body),
                    response_body,
                    receive_error))
            {
                return { false, 0, {}, std::move(receive_error) };
            }
        }
        else if (length != headers.end())
        {
            std::size_t content_length = 0;
            const auto parsed_length = std::from_chars(
                length->second.data(),
                length->second.data() + length->second.size(),
                content_length);
            if (parsed_length.ec != std::errc{})
            {
                Disconnect();
                return {
                    false,
                    0,
                    {},
                    "Editor returned invalid HTTP Content-Length.",
                };
            }
            while (buffered_body.size() < content_length)
            {
                if (!ReceiveMore(buffered_body, receive_error))
                {
                    return { false, 0, {}, std::move(receive_error) };
                }
            }
            response_body.assign(buffered_body.data(), content_length);
        }
        else
        {
            response_body = std::move(buffered_body);
            std::array<char, 8192> buffer{};
            for (;;)
            {
                const auto count = ::recv(
                    socket,
                    buffer.data(),
                    static_cast<int>(buffer.size()),
                    0);
                if (count <= 0)
                {
                    break;
                }
                response_body.append(
                    buffer.data(),
                    static_cast<std::size_t>(count));
            }
            Disconnect();
        }

        const auto connection = headers.find("connection");
        if (http_10
            || (connection != headers.end()
                && Lower(connection->second) == "close"))
        {
            Disconnect();
        }
        return {
            status >= 200 && status < 300,
            status,
            std::move(response_body),
            {},
        };
    }

    HttpResult Perform(
        const std::string_view method,
        const std::string_view path,
        const std::string_view body)
    {
        EnsureBestEffortSession();
        const bool had_session =
            session && !session->session_id.empty();
        HttpResult response = PerformRaw(method, path, body);
        if (!had_session || !IsExpiredClientSessionResponse(response))
        {
            return response;
        }

        session->session_id.clear();
        request_headers.erase("X-UEAI-Session-Id");
        Disconnect();
        EnsureBestEffortSession();
        return PerformRaw(method, path, body);
    }

    struct CliSessionState
    {
        CliSessionOptions options;
        bool protocol_unsupported = false;
        std::string session_id;
    };

    std::string endpoint;
    std::uint32_t timeout_ms = 300000;
    std::optional<ParsedEndpoint> parsed;
    std::map<std::string, std::string> request_headers;
    std::optional<CliSessionState> session;
    Socket socket = kInvalidSocket;
#if defined(_WIN32)
    WSADATA winsock_data{};
    bool runtime_available = false;
#else
    bool runtime_available = true;
#endif
};

Client::Client(std::string endpoint, const std::uint32_t timeout_ms)
    : impl_(std::make_unique<Impl>(
        std::move(endpoint),
        timeout_ms == 0 ? 1U : timeout_ms))
{
}

Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

HttpResult Client::Get(const std::string_view path)
{
    return impl_->Perform("GET", path, {});
}

HttpResult Client::Post(
    const std::string_view path,
    const std::string_view body)
{
    return impl_->Perform("POST", path, body);
}

bool Client::SetHeader(std::string name, std::string value)
{
    if (!IsHeaderName(name) || !IsHeaderValue(value))
    {
        return false;
    }
    impl_->request_headers[std::move(name)] = std::move(value);
    return true;
}

void Client::RemoveHeader(const std::string_view name)
{
    impl_->request_headers.erase(std::string(name));
}

void Client::ConfigureBestEffortCliSession(CliSessionOptions options)
{
    impl_->ConfigureBestEffortCliSession(std::move(options));
}

const std::string& Client::Endpoint() const
{
    return impl_->endpoint;
}

std::uint32_t Client::TimeoutMs() const
{
    return impl_->timeout_ms;
}

std::string UrlEncode(const std::string_view value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character : value)
    {
        if ((character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '-'
            || character == '_'
            || character == '.')
        {
            encoded.push_back(static_cast<char>(character));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(hex[(character >> 4U) & 0x0fU]);
        encoded.push_back(hex[character & 0x0fU]);
    }
    return encoded;
}

std::string NewInvocationId()
{
    std::random_device seed;
    std::mt19937_64 generator(seed());
    std::uniform_int_distribution<std::uint64_t> distribution;
    const std::uint64_t high = distribution(generator);
    const std::uint64_t low = distribution(generator);
    std::ostringstream stream;
    stream << "cli-" << std::hex << std::setfill('0')
           << std::setw(16) << high
           << std::setw(16) << low;
    return stream.str();
}

std::uint32_t CurrentProcessId()
{
#if defined(_WIN32)
    return static_cast<std::uint32_t>(_getpid());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

} // namespace ue::api
