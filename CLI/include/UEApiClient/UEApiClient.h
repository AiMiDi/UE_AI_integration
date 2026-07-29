#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ue::api
{

struct HttpResult
{
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

class Client
{
public:
    explicit Client(
        std::string endpoint = "http://127.0.0.1:9847",
        std::uint32_t timeout_ms = 300000);
    ~Client();

    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    HttpResult Get(std::string_view path);
    HttpResult Post(std::string_view path, std::string_view body);

    const std::string& Endpoint() const;
    std::uint32_t TimeoutMs() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string UrlEncode(std::string_view value);

} // namespace ue::api
