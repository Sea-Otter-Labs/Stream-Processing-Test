#include "HttpServer.h"
#include "Logger.h"
#include <sstream>

HttpServer::HttpServer()
{
}

HttpServer::~HttpServer()
{
}

size_t HttpServer::writeCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t total = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    str->append((char*)contents, total);
    return total;
}

std::string HttpServer::Get(const std::string& host, int port, const std::string& path, const std::string& queryParams) 
{
    std::ostringstream url;
    url << host << ":" << port << path;
    if (!queryParams.empty()) {
        url << "?" << queryParams;
    }
    return request(url.str(), "", false);
}

std::string HttpServer::Post(const std::string& host, int port, const std::string& path, const std::string& postData) 
{
    std::ostringstream url;
    url << host << ":" << port << path;

    CURL* curl = curl_easy_init();
    if (!curl) return "Failed to init curl";

    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // HTTPS 时忽略证书
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) 
    {
        response = std::string("curl_easy_perform() failed: ") + curl_easy_strerror(res);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return response;
}

std::string HttpServer::request(const std::string& url, const std::string& data, bool isPost) 
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        return "Failed to init curl";
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    if (isPost) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
    }

    // HTTPS 时忽略证书（生产环境要改为严格验证）
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    // 写回调函数
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) 
    {
        response = std::string("curl_easy_perform() failed: ") + curl_easy_strerror(res);
    }

    curl_easy_cleanup(curl);
    return response;
}

bool HttpServer::sendLarkMessage(const std::string& webhookUrl, const std::string& message) 
{
       if (message.empty()) {
        Logger::getInstance()->warn("sendLarkMessage: message is empty, skip sending");
        return true;
    }

    // ✅ 1. 定义安全替换函数（无 regex，更快更稳）
    auto replaceAll = [](std::string &str, const std::string &from, const std::string &to) {
        if (from.empty()) return;
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, from.length(), to);
            pos += to.length();
        }
    };

    // ✅ 2. 转义消息文本，防止 JSON 格式错误
    std::string safeMsg = message;
    replaceAll(safeMsg, "\\", "\\\\");  // 转义反斜杠
    replaceAll(safeMsg, "\"", "\\\"");  // 转义引号
    replaceAll(safeMsg, "\n", "\\n");   // 转义换行
    replaceAll(safeMsg, "\r", "");      // 删除回车

    // ✅ 3. Lark 字符长度限制处理（官方最大约 20,000）
    if (safeMsg.size() > 18000) {
        Logger::getInstance()->warn("⚠️ Lark message too long ({} chars), trimming...", safeMsg.size());
        safeMsg = safeMsg.substr(0, 17900) + "...(内容过长已截断)";
    }

    // ✅ 4. 组装 JSON 数据
    std::ostringstream jsonStream;
    jsonStream << "{"
               << "\"msg_type\":\"text\","
               << "\"content\":{\"text\":\"" << safeMsg << "\"}"
               << "}";
    std::string jsonData = jsonStream.str();

    // ✅ 5. 输出调试日志
    Logger::getInstance()->debug("👉 即将发送的 JSON 数据 ({} 字节): {}", jsonData.size(), jsonData);

    // ✅ 6. 发送 HTTP POST
    CURL* curl = curl_easy_init();
    if (!curl) {
        Logger::getInstance()->error("curl_easy_init failed");
        return false;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

    curl_easy_setopt(curl, CURLOPT_URL, webhookUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);

    bool success = false;
    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code == 200) {
            Logger::getInstance()->info("✅ Lark 消息发送成功");
            success = true;
        } else {
            Logger::getInstance()->error("❌ Lark 返回 HTTP 状态码: {}", http_code);
        }
    } else {
        Logger::getInstance()->error("❌ curl_easy_perform failed: {}", curl_easy_strerror(res));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return success;
}
