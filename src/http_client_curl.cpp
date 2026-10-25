// Linux implementation of https_post using libcurl.

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

} // namespace

HttpResponse https_post(const std::string& url,
                        const std::map<std::string, std::string>& headers,
                        const std::string& body,
                        const std::string& content_type)
{
    HttpResponse out;
    std::call_once(curl_global_init_flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) { out.error = "curl_easy_init failed"; return out; }

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &out.body);

    struct curl_slist* header_list = nullptr;
    auto add_header = [&header_list](const std::string& line) {
        header_list = curl_slist_append(header_list, line.c_str());
    };
    add_header("Content-Type: " + content_type);
    for (const auto& [name, value] : headers)
        add_header(name + ": " + value);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, header_list);

    const CURLcode rc = curl_easy_perform(curl.get());
    if (header_list)
        curl_slist_free_all(header_list);

    if (rc != CURLE_OK) {
        out.error = curl_easy_strerror(rc);
        return out;
    }

    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    out.status = status;
    return out;
}

} // namespace krastor
