// Krastor — minimal HTTPS POST abstraction.
// Windows: WinHTTP (built into the OS — works identically with MinGW and MSVC,
//          no OpenSSL/curl deployment).
// Linux:   libcurl.
// Only what the OpenAI-compatible client needs: one blocking POST with headers.

#pragma once

#include <atomic>
#include <map>
#include <string>

namespace krastor {

// Cooperative cancellation for a blocking https_post() running on a worker
// thread; the GUI's Stop button calls cancel() from the UI thread.
// - WinHTTP: closes the in-flight request handle (the documented way to abort
//   a synchronous call), so the pending read fails immediately.
// - libcurl: the progress callback polls `cancelled` and aborts the transfer.
// Shared through a shared_ptr so the token outlives whichever thread uses it.
struct CancelToken {
    std::atomic<bool> cancelled{false};
    // Internal bookkeeping set by the platform https_post while a request is
    // in flight; whoever's atomic exchange takes the handle owns the close.
    std::atomic<void*> handle{nullptr};
    void (*closer)(void*) = nullptr;

    bool load() const { return cancelled.load(); }
    void cancel()
    {
        cancelled.store(true);
        void* h = handle.exchange(nullptr);
        if (h && closer)
            closer(h);
    }
};

struct HttpResponse {
    long status = 0;            // HTTP status code (0 = transport failure)
    std::string body;
    std::string error;          // transport-level error text when status == 0
    std::string content_type;   // response Content-Type ("" when absent)
};

// Blocking HTTPS POST. `url` must be https:// (or http:// for local servers).
// Returns status==0 with `error` set on any transport failure; never throws.
// When `cancel` is given, cancel() from another thread aborts the call
// immediately (error text "cancelled").
HttpResponse https_post(const std::string& url,
                        const std::map<std::string, std::string>& headers,
                        const std::string& body,
                        const std::string& content_type = "application/json",
                        CancelToken* cancel = nullptr);

// Blocking HTTPS GET (used by the webfetch tool). Same contract as POST.
HttpResponse https_get(const std::string& url,
                       const std::map<std::string, std::string>& headers,
                       CancelToken* cancel = nullptr);

} // namespace krastor
