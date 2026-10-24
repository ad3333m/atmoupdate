#include "http.hpp"
#include <curl/curl.h>
#include <cstdio>

namespace {

size_t write_string(void* data, size_t n, size_t m, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(static_cast<char*>(data), n * m);
    return n * m;
}

size_t write_file(void* data, size_t n, size_t m, void* ud) {
    return std::fwrite(data, n, m, static_cast<FILE*>(ud)) * n;
}

int xfer_cb(void* ud, curl_off_t dltotal, curl_off_t dlnow,
            curl_off_t, curl_off_t) {
    auto* p = static_cast<http::ProgressFn*>(ud);
    if (p && *p) (*p)(static_cast<double>(dlnow), static_cast<double>(dltotal));
    return 0;
}

void common_opts(CURL* c) {
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "AtmoUpdate/1.0 (nx)");
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 100L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
}

}

namespace http {

bool get_string(const std::string& url, std::string& out, std::string& err) {
    CURL* c = curl_easy_init();
    if (!c) { err = "curl init"; return false; }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_string);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    common_opts(c);
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) { err = curl_easy_strerror(r); return false; }
    if (code >= 400) { err = "HTTP " + std::to_string(code); return false; }
    return true;
}

bool get_file(const std::string& url, const std::string& dest_path,
              const ProgressFn& progress, std::string& err) {
    FILE* f = std::fopen(dest_path.c_str(), "wb");
    if (!f) { err = "open " + dest_path; return false; }
    CURL* c = curl_easy_init();
    if (!c) { std::fclose(f); err = "curl init"; return false; }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void*)&progress);
    common_opts(c);
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    std::fclose(f);
    if (r != CURLE_OK) { curl_easy_cleanup(c); err = curl_easy_strerror(r); return false; }
    if (code >= 400) { curl_easy_cleanup(c); err = "HTTP " + std::to_string(code); return false; }
    curl_easy_cleanup(c);
    return true;
}

}
