#pragma once
#include <iostream>
#include <string>
#include <sstream>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

class HttpServer
{
public:
    HttpServer();
    ~HttpServer();
    static bool sendLarkMessage(const std::string& webhookUrl, const std::string& message);
    std::string Post(const std::string& host, int port, const std::string& path, const std::string& postData);
    std::string Get(const std::string& host, int port, const std::string& path, const std::string& queryParams = "") ;
private:
    // 通用请求函数
    std::string request(const std::string& url, const std::string& data, bool isPost); 
    static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);
};
