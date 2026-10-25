// Krastor — minimal HTTPS POST abstraction.
// Windows: WinHTTP (built into the OS — works identically with MinGW and MSVC,
//          no OpenSSL/curl deployment).
// Linux:   libcurl.
// Only what the OpenAI-compatible client needs: one blocking POST with headers.

#pragma once

#include <map>
#include <string>

namespace krastor {

struct HttpResponse {
    long status = 0;            // HTTP status code (0 = transport failure)
    std::string body;
    std::string error;          // transport-level error text when status == 0
};

// Blocking HTTPS POST. `url` must be https:// (or http:// for local servers).
// Returns status==0 with `error` set on any transport failure; never throws.
HttpResponse https_post(const std::string& url,
                        const std::map<std::string, std::string>& headers,
                        const std::string& body,
                        const std::string& content_type = "application/json");

} // namespace krastor
