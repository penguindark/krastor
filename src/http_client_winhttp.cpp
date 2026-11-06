// Windows implementation of https_post/https_get using WinHTTP.
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

void close_request(void* h) { WinHttpCloseHandle(static_cast<HINTERNET>(h)); }

// Owns the request handle, honouring a CancelToken: if cancel() already took
// the handle (and thereby aborted the pending call), the exchange yields null
// and we must not close it again.
struct RequestGuard {
    HINTERNET h = nullptr;
    CancelToken* tok = nullptr;
    ~RequestGuard()
    {
        if (tok) {
            void* p = tok->handle.exchange(nullptr);
            if (p)
                close_request(p);
        } else if (h) {
            WinHttpCloseHandle(h);
        }
    }
};

std::wstring wstr(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, n ? &w[0] : nullptr, n);
    return w;
}

// Shared POST/GET core. Empty body + null content_type => plain GET.
HttpResponse request(const wchar_t* method, const std::string& url,
                     const std::map<std::string, std::string>& headers,
                     const std::string& body, const std::string& content_type,
                     CancelToken* cancel)
{
    HttpResponse out;

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
    RequestGuard guard;
    guard.h = WinHttpOpenRequest(connect.get(), method, path,
                                 nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!guard.h) { out.error = "WinHttpOpenRequest failed"; return out; }

    // Register the handle with the cancel token: a cross-thread cancel() now
    // closes it, which makes every pending synchronous call fail at once.
    if (cancel) {
        guard.tok = cancel;
        cancel->closer = &close_request;
        cancel->handle.store(guard.h);
        if (cancel->load()) {  // cancelled before registration took effect
            out.error = "cancelled";
            return out;
        }
    }

    WinHttpSetTimeouts(guard.h, kConnectTimeoutMs, kConnectTimeoutMs,
                       kConnectTimeoutMs, kReadTimeoutMs);

    std::wstring header_blob;
    for (const auto& [name, value] : headers)
        header_blob += wstr(name) + L": " + wstr(value) + L"\r\n";
    if (!content_type.empty())
        header_blob += L"Content-Type: " + wstr(content_type) + L"\r\n";

    if (!WinHttpSendRequest(guard.h, header_blob.c_str(),
                            static_cast<DWORD>(header_blob.size()),
                            body.empty() ? nullptr
                                         : const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0)) {
        out.error = cancel && cancel->load()
                        ? "cancelled"
                        : "WinHttpSendRequest failed (" +
                              std::to_string(GetLastError()) + ")";
        return out;
    }

    if (!WinHttpReceiveResponse(guard.h, nullptr)) {
        out.error = cancel && cancel->load()
                        ? "cancelled"
                        : "WinHttpReceiveResponse failed (" +
                              std::to_string(GetLastError()) + ")";
        return out;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(guard.h,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    out.status = status;

    wchar_t ctype[256]{};
    size = sizeof(ctype);
    if (WinHttpQueryHeaders(guard.h, WINHTTP_QUERY_CONTENT_TYPE,
                            WINHTTP_HEADER_NAME_BY_INDEX, ctype, &size,
                            WINHTTP_NO_HEADER_INDEX)) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, ctype, -1, nullptr, 0,
                                          nullptr, nullptr);
        out.content_type.resize(n > 1 ? static_cast<size_t>(n) - 1 : 0);
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, ctype, -1, out.content_type.data(),
                                n, nullptr, nullptr);
    }

    for (;;) {
        if (cancel && cancel->load()) {
            out.error = "cancelled";
            return out;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(guard.h, &available))
            break;
        if (available == 0)
            break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(guard.h, chunk.data(), available, &read))
            break;
        chunk.resize(read);
        out.body += chunk;
    }
    if (cancel && cancel->load()) {
        out.status = 0;
        out.error = "cancelled";
    }
    return out;
}

} // namespace

HttpResponse https_post(const std::string& url,
                        const std::map<std::string, std::string>& headers,
                        const std::string& body,
                        const std::string& content_type,
                        CancelToken* cancel)
{
    return request(L"POST", url, headers, body, content_type, cancel);
}

HttpResponse https_get(const std::string& url,
                       const std::map<std::string, std::string>& headers,
                       CancelToken* cancel)
{
    return request(L"GET", url, headers, std::string(), std::string(), cancel);
}

} // namespace krastor
