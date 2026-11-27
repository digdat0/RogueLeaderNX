#pragma once
// HTTP(S) requests for RetroAchievements and the updater (http.cpp): WinHTTP on Windows, the
// system's libcurl (loaded when first needed) on Linux, Java's HttpURLConnection on Android.
// Blocking: call from a worker thread.
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "runtime.h"

namespace rt::http {

struct Request {
    std::string url;
    std::string post, contentType;  // a POST with this body when not empty
    std::string userAgent;
    std::vector<std::string> headers;  // more request headers, "Name: value"
    // the transfer may take this long at most (0: as long as data keeps coming: downloads)
    int timeoutSeconds = 30;
    FILE* file = nullptr;  // the body goes here instead of Response::body
    // as data arrives: bytes so far and the total (0 when unknown); false cancels the transfer
    std::function<bool(u64 got, u64 total)> progress;
};

struct Response {
    bool ok = false;  // a response came (any status)
    int status = 0;
    std::string body;
};

Response fetch(const Request& r);

}  // namespace rt::http
