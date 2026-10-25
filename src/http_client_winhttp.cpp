// Windows implementation of https_post using WinHTTP.
// TLS is provided by the OS (Schannel), so no third-party crypto ships with us.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <memory>

#include "http_client.h"

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")  // MSVC; MinGW links via CMake
#endif

namespace krastor {

namespace {

constexpr DWORD kConnectTimeoutMs = 10'000;
constexpr DWORD kReadTimeoutMs = 120'000;   // LLM responses can take a while

struct HandleDeleter {
    void operator()(HINTERNET h) const { if (h) WinHttpCloseHandle(h); }
};
using HInternet = std::unique_ptr<void, HandleDeleter>;

} // namespace

HttpResponse https_post(const std::string& url,
                        const std::map<std::string, std::string>& headers,
                        const std::string& body,
                        const std::string& content_type)
{
    HttpResponse out;

    // WinHTTP wants wide strings.
    auto wstr = [](const std::string& s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(static_cast<size_t>(n ? n - 1 : 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1,
                            n ? &w[0] : nullptr, n);
        return w;
    };

    const std::wstring wurl = wstr(url);

    URL_COMPONENTS comps{};
    comps.dwStructSize = sizeof(comps);
    wchar_t host[256]{};
    wchar_t path[1024]{};
    comps.lpszHostName = host;
    comps.dwHostNameLength = std::size(host) - 1;
    comps.lpszUrlPath = path;
    comps.dwUrlPathLength = std::size(path) - 1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &comps)) {
        out.error = "Invalid URL: " + url;
        return out;
    }

    const bool https = comps.nScheme == INTERNET_SCHEME_HTTPS;

    HInternet session(WinHttpOpen(L"Krastor/0.3",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { out.error = "WinHttpOpen failed"; return out; }

    HInternet connect(WinHttpConnect(session.get(), host, comps.nPort, 0));
    if (!connect) { out.error = "WinHttpConnect failed"; return out; }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HInternet request(WinHttpOpenRequest(connect.get(), L"POST", path,
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) { out.error = "WinHttpOpenRequest failed"; return out; }

    WinHttpSetTimeouts(request.get(), kConnectTimeoutMs, kConnectTimeoutMs,
                       kConnectTimeoutMs, kReadTimeoutMs);

    std::wstring header_blob;
    for (const auto& [name, value] : headers)
        header_blob += wstr(name) + L": " + wstr(value) + L"\r\n";
    header_blob += L"Content-Type: " + wstr(content_type) + L"\r\n";

    if (!WinHttpSendRequest(request.get(), header_blob.c_str(),
                            static_cast<DWORD>(header_blob.size()),
                            const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0)) {
        out.error = "WinHttpSendRequest failed (" +
                    std::to_string(GetLastError()) + ")";
        return out;
    }

    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        out.error = "WinHttpReceiveResponse failed (" +
                    std::to_string(GetLastError()) + ")";
        return out;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.get(),
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    out.status = status;

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available))
            break;
        if (available == 0)
            break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), available, &read))
            break;
        chunk.resize(read);
        out.body += chunk;
    }
    return out;
}

} // namespace krastor
