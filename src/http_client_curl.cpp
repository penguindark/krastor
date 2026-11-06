// Linux implementation of https_post/https_get using libcurl.

#include <curl/curl.h>

#include <memory>
#include <mutex>

#include "http_client.h"

namespace krastor {

namespace {

std::once_flag curl_global_init_flag;

size_t write_callback(void* data, size_t size, size_t nmemb, void* userp)
{
    auto* body = static_cast<std::string*>(userp);
    body->append(static_cast<char*>(data), size * nmemb);
    return size * nmemb;
}

// Progress callback: non-zero return aborts the transfer. curl invokes it
// frequently (roughly every 100 ms even while waiting on a slow server), so a
// cancel() is noticed almost immediately.
int progress_callback(void* clientp, curl_off_t, curl_off_t, curl_off_t,
                      curl_off_t)
{
    auto* token = static_cast<CancelToken*>(clientp);
    return token && token->load() ? 1 : 0;
}

// Shared POST/GET core. Empty body + null content_type => plain GET.
HttpResponse request(const char* method, const std::string& url,
                     const std::map<std::string, std::string>& headers,
                     const std::string& body, const std::string& content_type,
                     CancelToken* cancel)
{
    HttpResponse out;
    std::call_once(curl_global_init_flag,
                   [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) { out.error = "curl_easy_init failed"; return out; }

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    if (body.empty() && content_type.empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L);
    } else {
        curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(body.size()));
    }
    if (method && std::string(method) != "POST")
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &out.body);
    if (cancel) {
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, cancel);
    }

    struct curl_slist* header_list = nullptr;
    auto add_header = [&header_list](const std::string& line) {
        header_list = curl_slist_append(header_list, line.c_str());
    };
    if (!content_type.empty())
        add_header("Content-Type: " + content_type);
    for (const auto& [name, value] : headers)
        add_header(name + ": " + value);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, header_list);

    const CURLcode rc = curl_easy_perform(curl.get());
    if (header_list)
        curl_slist_free_all(header_list);

    if (rc != CURLE_OK) {
        out.error = rc == CURLE_ABORTED_BY_CALLBACK
                        ? "cancelled"
                        : curl_easy_strerror(rc);
        return out;
    }

    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    out.status = status;
    char* ctype = nullptr;
    curl_easy_getinfo(curl.get(), CURLINFO_CONTENT_TYPE, &ctype);
    if (ctype)
        out.content_type = ctype;
    return out;
}

} // namespace

HttpResponse https_post(const std::string& url,
                        const std::map<std::string, std::string>& headers,
                        const std::string& body,
                        const std::string& content_type,
                        CancelToken* cancel)
{
    return request("POST", url, headers, body, content_type, cancel);
}

HttpResponse https_get(const std::string& url,
                       const std::map<std::string, std::string>& headers,
                       CancelToken* cancel)
{
    return request("GET", url, headers, std::string(), std::string(), cancel);
}

} // namespace krastor
