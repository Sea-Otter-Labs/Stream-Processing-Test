#include "StreamTest.h"
#include "Logger.h"
#include <sys/mman.h>
#include <sys/wait.h>
static std::mutex g_mutex;
static std::mutex g_mutexSuccess;

// Logger::getInstance()->debug("This is debug message");
// Logger::getInstance()->info("This is info message");
// Logger::getInstance()->warn("This is warning");
// Logger::getInstance()->error("This is error");

StreamTest::StreamTest()
{

}

StreamTest::~StreamTest()
{

}

std::string GetCurrentTimeString() 
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return std::string(buf);
}

void custom_ffmpeg_log(void* ptr, int level, const char* fmt, va_list vl) 
{
    if (level <= AV_LOG_INFO) {  // 只关心 info 级别以上
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, vl);
        // 这里你可以写到文件、输出到程序日志、或者自己解析 URL
        printf("[FFmpegLog] %s", buf);
    }
}

bool isLastLoop(int nWaitTimeMinutes)
{
    using namespace std::chrono;

    // 当前 UTC 时间点
    system_clock::time_point now = system_clock::now();
    time_t now_c = system_clock::to_time_t(now);
    std::tm utc_tm = *gmtime(&now_c); // UTC 时间

    // 计算明天零点（UTC）
    std::tm tomorrow_tm = utc_tm;
    tomorrow_tm.tm_hour = 0;
    tomorrow_tm.tm_min = 0;
    tomorrow_tm.tm_sec = 0;
    tomorrow_tm.tm_mday += 1;  // 明天
    time_t tomorrow_zero_c = timegm(&tomorrow_tm);
    system_clock::time_point tomorrow_zero = system_clock::from_time_t(tomorrow_zero_c);

    // 加上等待时间
    system_clock::time_point next_time = now + minutes(nWaitTimeMinutes);

    // 判断是否到达/跨过明天零点
    return next_time >= tomorrow_zero;
}

std::string getUtcTimeString(bool now = true)
{
    std::time_t t = std::time(nullptr);       // 当前 UTC 时间戳
    std::tm gmt_tm = *std::gmtime(&t);        // 转换为 UTC 时间结构体

    if (!now) 
    {
        //置零：小时、分钟、秒
        gmt_tm.tm_hour = 0;
        gmt_tm.tm_min  = 0;
        gmt_tm.tm_sec  = 0;
    }

    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &gmt_tm);

    return std::string(buffer);
}

std::string CSVLinesToMarkdown(const std::vector<std::string>& lines) {
    if (lines.empty()) return "";

    std::ostringstream oss;

    // 取表头
    oss << "|" << lines[0] << "|\n";

    // 表头分隔符
    size_t colCount = std::count(lines[0].begin(), lines[0].end(), ',') + 1;
    oss << "|";
    for (size_t i = 0; i < colCount; ++i) oss << "---|";
    oss << "\n";

    // 内容行
    for (size_t i = 1; i < lines.size(); ++i) {
        oss << "|" << lines[i] << "|\n";
    }

    return oss.str();
}

struct TimeoutData {
    int timeout_ms; // 超时时间（毫秒）
    std::chrono::steady_clock::time_point start_time;
};

// 回调函数
int interrupt_cb(void* ctx) 
{
    TimeoutData* data = (TimeoutData*)ctx;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - data->start_time).count();
    if (elapsed > data->timeout_ms) 
    {
        Logger::getInstance()->error("[interrupt_cb] timeout triggered");
        // 超时，返回非零表示中断
        return 1;
    }
    return 0; // 不中断
}


void StreamTest::WriteSqlDbData(const StreamInfo &stStreamInfo)
{
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) 
    {
        Logger::getInstance()->error("mysql_init failed");
        return;
    }

    if (!mysql_real_connect(conn, SQL_HOST, SQL_USER, SQL_PASSWD, SQL_DBNAME, SQL_PORT, nullptr, 0)) 
    {
        Logger::getInstance()->error("mysql_real_connect failed: {}", mysql_error(conn));
        mysql_close(conn);
        return;
    }

    // 用 map 存储待更新的字段和值
    std::map<std::string, std::string> updateFields;

    if (!stStreamInfo.strStreamVideoFormat.empty())
        updateFields["video_format"] = stStreamInfo.strStreamVideoFormat;

    if (!stStreamInfo.strStreamVideoResolution.empty())
        updateFields["video_resolution"] = stStreamInfo.strStreamVideoResolution;

    if (!stStreamInfo.strStreamAudioFormat.empty())
        updateFields["audio_format"] = stStreamInfo.strStreamAudioFormat;

    if (!stStreamInfo.target_matching_id.empty())
        updateFields["target_matching_id"] = stStreamInfo.target_matching_id;

    // resolution_type 和 flow_score 0 也可能有效，所以直接更新
    updateFields["resolution_type"] = std::to_string(stStreamInfo.nVideoResolutiontype);

    updateFields["flow_score"] = std::to_string((int)stStreamInfo.nFlowScore);

    if (!stStreamInfo.strStreamAudioSamplingRate.empty())
        updateFields["audio_sampling_rate"] = stStreamInfo.strStreamAudioSamplingRate;

    if (updateFields.empty())
    {
        Logger::getInstance()->warn("No fields to update for id: {}", stStreamInfo.id);
        mysql_close(conn);
        return;
    }

    // 拼接 SQL
    std::ostringstream oss;
    oss << "UPDATE live_stream_sources SET ";
    bool first = true;
    for (const auto& kv : updateFields)
    {
        if (!first) oss << ", ";
        oss << kv.first << "='" << kv.second << "'";
        first = false;
    }
    oss << " WHERE id='" << stStreamInfo.id << "';";

    std::string sql = oss.str();

    if (mysql_query(conn, sql.c_str()))
    {
        Logger::getInstance()->error("mysql_query failed: {}", mysql_error(conn));
    }
    else
    {
        Logger::getInstance()->debug("Update success, id: {}", stStreamInfo.id);
    }

    mysql_close(conn);
}

void StreamTest::addSqlDbData(const json &j)
{
    // 1. 初始化 MySQL
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) 
    {
        Logger::getInstance()->error("mysql_init failed");
        return;
    }

    // 2. 连接数据库
    if (!mysql_real_connect(conn, SQL_HOST, SQL_USER, SQL_PASSWD, SQL_DBNAME, SQL_PORT, nullptr, 0)) 
    {
        Logger::getInstance()->error("Connection failed: {}", mysql_error(conn));
        mysql_close(conn);
        return;
    }

    // 3. 遍历 JSON，生成字段和值
    std::ostringstream fields;
    std::ostringstream values;

    bool first = true;
    for (auto it = j.begin(); it != j.end(); ++it) 
    {
        if (!first) 
        {
            fields << ", ";
            values << ", ";
        }
        first = false;

        // 字段名
        fields << it.key();

        // 值（需要转义）
        std::string val = it.value().is_string() ? it.value().get<std::string>()
                                                 : it.value().dump();

        std::vector<char> escaped(val.size() * 2 + 1);
        mysql_real_escape_string(conn, escaped.data(), val.c_str(), val.size());

        values << "'" << escaped.data() << "'";
    }

    // 4. 拼接 SQL
    std::ostringstream oss;
    oss << "INSERT INTO ffmpeg_flow_detection (" 
        << fields.str() << ") VALUES (" 
        << values.str() << ")";

    std::string sql = oss.str();

    // 5. 执行 SQL
    if (mysql_query(conn, sql.c_str())) 
    {
        Logger::getInstance()->error("MySQL insert failed: {}", mysql_error(conn));
    } 
    // else 
    // {
    //     Logger::getInstance()->info("MySQL insert success: {}", sql);
    // }

    // 6. 关闭连接
    mysql_close(conn);
}

void StreamTest::HttpPostOperation(StreamError enStreamError, std::string strLog,int ret, StreamInfo stStreamInfo)
{
    //提交到数据库
    // if(stStreamInfo.strStreamVideoFormat != "" && stStreamInfo.strStreamVideoResolution != "" 
    //     && stStreamInfo.strStreamAudioFormat != "" && stStreamInfo.strStreamAudioSamplingRate != "")
    // {
    //     WriteSqlDbData(stStreamInfo);
    // }

    //http请求
    json j;
    j["flow_address"]       = stStreamInfo.strStreamPath;               // 流地址
    j["item"]               = std::to_string(enStreamError);            // 项目名称，可自定义
    j["return_value"]       = std::to_string(ret);                      // 返回值
    j["lag_details"]        = strLog;                                   // 日志详情
    j["streaming_protocol"] = stStreamInfo.strStreamProtocol;           
    j["bitrate"]            = stStreamInfo.strStreamBitrate;             
    j["stream_length"]      = stStreamInfo.strStreamLength;              
    j["video_format"]       = stStreamInfo.strStreamVideoFormat;          
    j["video_resolution"]   = stStreamInfo.strStreamVideoResolution;       
    j["audio_format"]       = stStreamInfo.strStreamAudioFormat;           
    j["audio_sampling_rate"]= stStreamInfo.strStreamAudioSamplingRate;

    j["created_time"] = getUtcTimeString();     
    j["target_matching_id"]= stStreamInfo.target_matching_id;     
    j["target_matching"]= stStreamInfo.strStreamName;     
    j["url_id"] = stStreamInfo.id;     

    //写入数据库
    addSqlDbData(j);

    // 转成字符串
    // HttpServer server;
    // std::string postData = j.dump();
    // // 调用已有的 POST 接口 8001
    // std::string response = server.Post(SERVER_HOST, SERVER_PORT, "/api/v1/flow_detection/get_ffmpeg_detection", postData);
    // // 可打印返回值
    // std::cout << "HTTP Response: " << response << std::endl;
    // Logger::getInstance()->info("HTTP Response: {}",response.c_str());
}

void StreamTest::print_ffmpeg_error(StreamInfo stStreamInfo,int ret, StreamError enStreamError, const std::string &msg,bool bState)
{
    std::string strError = msg;

    //补充日志
    if (bState)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        strError = stStreamInfo.strStreamPath + " "+ msg + ":" + errbuf + " ret=" + std::to_string(ret);
    }

    HttpPostOperation(enStreamError,strError,ret,stStreamInfo);

}

void StreamTest::print_ffmpeg_success(StreamInfo stStreamInfo)
{
    //多线程加锁操作文件
    HttpPostOperation(OPERATION_OK,"",0,stStreamInfo);
}

//打印编码器列表
void list_encoders() 
{
    void* iter = nullptr;
    const AVCodec* codec = nullptr;
    while ((codec = av_codec_iterate(&iter))) {
        if (av_codec_is_encoder(codec)) {
            std::cout << codec->name << " : " << codec->long_name << std::endl;
        }
    }
}

std::vector<StreamRecord> StreamTest::queryStreamRecords(const std::string& startTime,const std::string& endTime)
{
    std::vector<StreamRecord> results;

    // 1. 初始化 MySQL
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) 
    {
        std::cerr << "mysql_init failed\n";
        return results;
    }

    // 2. 连接数据库
    if (!mysql_real_connect(conn, SQL_HOST, SQL_USER, SQL_PASSWD, SQL_DBNAME, SQL_PORT, nullptr, 0)) 
    {
        std::cerr << "Connection failed: " << mysql_error(conn) << "\n";
        mysql_close(conn);
        return results;
    }

    // 3. 拼接 SQL
    std::ostringstream oss;
    oss << "SELECT flow_address, item, return_value, lag_details, streaming_protocol, bitrate, "
        << "stream_length, video_format, video_resolution, audio_format, audio_sampling_rate, "
        << "created_time, target_matching_id, target_matching, url_id "
        << "FROM ffmpeg_flow_detection "
        << "WHERE created_time BETWEEN '" << startTime << "' AND '" << endTime << "';";

    std::string sql = oss.str();

    // 4. 执行查询
    if (mysql_query(conn, sql.c_str())) 
    {
        std::cerr << "MySQL query failed: " << mysql_error(conn) << "\n";
        mysql_close(conn);
        return results;
    }

    // 5. 获取结果
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) 
    {
        std::cerr << "mysql_store_result failed: " << mysql_error(conn) << "\n";
        mysql_close(conn);
        return results;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) 
    {
        StreamRecord record;
        record.flow_address        = row[0] ? row[0] : "";
        record.item                = row[1] ? atoi(row[1]) : 0;
        record.return_value        = row[2] ? row[2] : "";
        record.lag_details         = row[3] ? row[3] : "";
        record.streaming_protocol  = row[4] ? row[4] : "";
        record.bitrate             = row[5] ? row[5] : "";
        record.stream_length       = row[6] ? row[6] : "";
        record.video_format        = row[7] ? row[7] : "";
        record.video_resolution    = row[8] ? row[8] : "";
        record.audio_format        = row[9] ? row[9] : "";
        record.audio_sampling_rate = row[10] ? row[10] : "";
        record.created_time        = row[11] ? row[11] : "";
        record.target_matching_id  = row[12] ? row[12] : "";
        record.target_matching     = row[13] ? row[13] : "";
        record.url_id              = row[14] ? row[14] : "";
        results.push_back(record);
    }

    // 6. 清理
    mysql_free_result(res);
    mysql_close(conn);

    return results;
}

std::vector<StreamInfo> StreamTest::GetStreamInfoSqlDbData()
{
    std::vector<StreamInfo> resultList;

    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {

        Logger::getInstance()->error("mysql_init failed");
        return resultList;
    }

    if (!mysql_real_connect(conn, SQL_HOST, SQL_USER, SQL_PASSWD, SQL_DBNAME, SQL_PORT, nullptr, 0)) 
    {
        Logger::getInstance()->error("Connection failed : {}",mysql_error(conn));
        return resultList;
    }

    // 查询所需字段
    const char* query = "SELECT id, url, target_matching ,target_matching_id "
        "FROM live_stream_sources "
        "WHERE is_del = 0 "
        "AND target_matching_id >= 237 ";
        
    if (mysql_query(conn, query)) 
    {
        Logger::getInstance()->error("Query failed : {}",mysql_error(conn));
        mysql_close(conn);
        return resultList;
    }

    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) 
    {
        Logger::getInstance()->error("mysql_store_result failed : {}",mysql_error(conn));
        mysql_close(conn);
        return resultList;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) 
    {
        StreamInfo stStreamInfo;

        // row[0] -> id
        if (row[0]) 
        {
            stStreamInfo.id = row[0];
        }

        // row[1] -> url
        if (row[1]) 
        {
            stStreamInfo.strStreamPath = row[1];
        }

        // row[1] -> target_matching
        if (row[2]) 
        {
            stStreamInfo.strStreamName = row[2];
        }

        if (row[3]) 
        {
            stStreamInfo.target_matching_id = row[3];
        }

        resultList.push_back(std::move(stStreamInfo));
    }

    mysql_free_result(res);
    mysql_close(conn);

    return resultList;
}

std::vector<BroadcastDetailsInfo> StreamTest::GetBroadcastDetailsInfoSqlDbData()
{
    std::vector<BroadcastDetailsInfo> resultList;

    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {

        Logger::getInstance()->error("mysql_init failed");
        return resultList;
    }

    if (!mysql_real_connect(conn, SQL_HOST, SQL_USER, SQL_PASSWD, SQL_DBNAME, SQL_PORT, nullptr, 0)) 
    {
        Logger::getInstance()->error("Connection failed : {}",mysql_error(conn));
        return resultList;
    }

    // 查询所需字段
    const char* query = "SELECT id, stream_name "
        "FROM live_broadcast_details ";
        
    if (mysql_query(conn, query)) 
    {
        Logger::getInstance()->error("Query failed : {}",mysql_error(conn));
        mysql_close(conn);
        return resultList;
    }

    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) 
    {
        Logger::getInstance()->error("mysql_store_result failed : {}",mysql_error(conn));
        mysql_close(conn);
        return resultList;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) 
    {
        BroadcastDetailsInfo stStreamInfo;

        // row[0] -> id
        if (row[0]) 
        {
            stStreamInfo.id = row[0];
        }

        // row[1] -> stream_name
        if (row[1]) 
        {
            stStreamInfo.stream_name = row[1];
        }

        resultList.push_back(std::move(stStreamInfo));
    }

    mysql_free_result(res);
    mysql_close(conn);

    return resultList;
}

void StreamTest::start()
{
        // === 创建共享内存 stopFlag ===
    stopFlag = (bool*)mmap(nullptr, sizeof(bool),
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (stopFlag == MAP_FAILED) 
    {
        Logger::getInstance()->error("mmap");
        return;
    }

    //av_log_set_level(AV_LOG_INFO);  // 设置日志级别
    //av_log_set_callback(custom_ffmpeg_log);
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    //list_encoders();

    // 获取文本参数
    // 成功请求
    int nIdx = 0;
    int nWhileIdx = 0; //循环次数
    int nTestNum = 20;//一次测试数量
    int nMinTime = 20;//一次测试时间

    std::thread([this]() 
    {
        int nWaitTime = 120;//两个小时

        while (true)
        {
            //查询结果进行上告
            std::string start = getUtcTimeString(false);  // 当天 UTC 零点
            std::string end   = getUtcTimeString();  // 当前 UTC 时间
            auto records = queryStreamRecords(start, end);
            std::map<std::string, OutStreamInfo> mapOutStreamInfo;//详细报告

            for (const auto& r : records)
            {
                //url_id
                OutStreamInfo& outInfo = mapOutStreamInfo[r.url_id];

                // 填充 StreamInfo，只要第一次填充即可
                if (outInfo.stStreamInfo.id.empty()) 
                {
                    outInfo.stStreamInfo.id = r.url_id;
                }

                if (outInfo.stStreamInfo.strStreamPath.empty()) 
                {
                    outInfo.stStreamInfo.strStreamPath = r.flow_address;
                }

                if (outInfo.stStreamInfo.strStreamName.empty()) 
                {
                    outInfo.stStreamInfo.strStreamName = r.target_matching;
                }

                if (outInfo.stStreamInfo.target_matching_id.empty()) 
                {
                    outInfo.stStreamInfo.target_matching_id = r.target_matching_id;
                }

                if (outInfo.stStreamInfo.strStreamProtocol.empty()) 
                {
                    outInfo.stStreamInfo.strStreamProtocol = r.streaming_protocol;
                }

                if (outInfo.stStreamInfo.strStreamBitrate.empty()) 
                {
                    outInfo.stStreamInfo.strStreamBitrate = r.bitrate;
                }

                if (outInfo.stStreamInfo.strStreamLength.empty()) 
                {
                    outInfo.stStreamInfo.strStreamLength = r.stream_length;
                }

                if (outInfo.stStreamInfo.strStreamVideoFormat.empty()) 
                {
                    outInfo.stStreamInfo.strStreamVideoFormat = r.video_format;
                }

                if (outInfo.stStreamInfo.strStreamAudioFormat.empty()) 
                {
                    outInfo.stStreamInfo.strStreamAudioFormat = r.audio_format;
                }

                if (outInfo.stStreamInfo.strStreamAudioSamplingRate.empty()) 
                {
                    outInfo.stStreamInfo.strStreamAudioSamplingRate = r.audio_sampling_rate;
                }

                if( outInfo.stStreamInfo.strStreamVideoResolution.empty())
                {
                    outInfo.stStreamInfo.strStreamVideoResolution = r.video_resolution;

                }
                else
                {
                    //强制更新视频分辨率
                    if(outInfo.stStreamInfo.strStreamVideoResolution == "0x0")
                    {
                        outInfo.stStreamInfo.strStreamVideoResolution = r.video_resolution;
                    }
                }

                // 如果 return_value != 0，说明失败
                if (r.item != OPERATION_OK) 
                {
                    // 根据 item 字段增加对应 ErrorItemInfo 的计数
                    if (r.item == OPEN_INPUT_FAILED)               outInfo.stErrorItemInfo.nOpenInputNum++;
                    else if (r.item == STREAM_INFO_FAILED)        outInfo.stErrorItemInfo.nGetStreamInfoNum++;
                    else if (r.item == FIND_VIDEO_STREAM_FAILED)  outInfo.stErrorItemInfo.nFindVideoStreamNum++;
                    else if (r.item == FIND_AUDIO_STREAM_FAILED)  outInfo.stErrorItemInfo.nFindAudioStreamNum++;
                    else if (r.item == VIDEO_DECODER_NOT_FOUND)   outInfo.stErrorItemInfo.nVideoDecordNotFoundNum++;
                    else if (r.item == ALLOC_VIDEO_CTX_FAILED)    outInfo.stErrorItemInfo.nAllocVideoParamsNum++;
                    else if (r.item == COPY_VIDEO_PARAMS_FAILED)  outInfo.stErrorItemInfo.nCopyVideoParamsNum++;
                    else if (r.item == OPEN_VIDEO_DECODER_FAILED) outInfo.stErrorItemInfo.nOpenVideoDecoderNum++;
                    else if (r.item == AUDIO_DECODER_NOT_FOUND)   outInfo.stErrorItemInfo.nAudioDecordNotFoundNum++;
                    else if (r.item == ALLOC_AUDIO_CTX_FAILED)    outInfo.stErrorItemInfo.nAllocAudioParamsNum++;
                    else if (r.item == COPY_AUDIO_PARAMS_FAILED)  outInfo.stErrorItemInfo.nCopyAudioParamsNum++;
                    else if (r.item == OPEN_AUDIO_DECODER_FAILED) outInfo.stErrorItemInfo.nOpenAudioDecoderNum++;
                    else if (r.item == READ_PACKET_FAILED)        outInfo.stErrorItemInfo.nReadPacketNum++;
                    else if (r.item == DECODE_FRAME_FAILED)       outInfo.stErrorItemInfo.nDecodeFrameNum++;
                    else if (r.item == READ_FRAME_FAILED)         outInfo.stErrorItemInfo.nReadFrameNum++;
                    else if (r.item == DTS_PTS_FAILED)            outInfo.stErrorItemInfo.nDtsPtsErrorNum++;
                    // 总错误数 +1
                    outInfo.nErrorNum++;
                }
                // 总检测次数 +1
                outInfo.nDetectionNum++;
            }
            
            //同一个节目放在附近
            std::vector<std::pair<std::string, OutStreamInfo>> vec(mapOutStreamInfo.begin(), mapOutStreamInfo.end());

            std::sort(vec.begin(), vec.end(), [](const auto& a, const auto& b) {
                return a.second.stStreamInfo.target_matching_id < b.second.stStreamInfo.target_matching_id;
            });

            //SendCSVAsMarkdownToLark(vec, 30); // 每批发送 30 行

            //写入文件
            std::string strFileName;
            auto data = WriteStreamInfoCSVWithContent(vec,strFileName);
            if(data.empty())
            {
                Logger::getInstance()->error("生成检测结果文件失败");
            }
            else
            {
                //上传lark
                std::string strMessage = "生成检测结果文件:" + strFileName;
                Logger::getInstance()->info("✅ 生成检测结果文件: {}", strFileName);

                // 上传文件到 Lark，路径就是当前目录的文件名
                if(HttpServer::sendLarkMessage(WEB_HOOK_STREAM_FAIL, strMessage))
                {
                    Logger::getInstance()->info("✅ 已上传到 Lark");
                }
                else
                {
                    Logger::getInstance()->error("❌ 上传到 Lark 失败");
                }
            }

            //最后一次
            if(isLastLoop(nWaitTime))
            {
                //获取节目单ID进行匹配
                auto veBroadcastDetailsInfo = GetBroadcastDetailsInfoSqlDbData();
                if(veBroadcastDetailsInfo.empty())
                {
                    Logger::getInstance()->info("未获取到节目单");
                    continue;
                }
                
                for (const auto& [url_id, info] : vec) 
                {
                    StreamInfo si = info.stStreamInfo;
                    bool b1080p = false;
                    //批量更新数据库
                    if(info.nDetectionNum > 0 && info.nErrorNum <= info.nDetectionNum)
                    {
                        double score = (double)(info.nDetectionNum - info.nErrorNum) / info.nDetectionNum * 100.0;
                        if (score < 0) score = 0;      // 避免异常数据算成负数
                        si.nFlowScore = (int)score;    // 如果必须存int
                    }
                    else
                    {
                        si.nFlowScore = 0;
                    }

                    //根据分辨率更新ID
                    si.nVideoResolutiontype = (int)getResolutionType(si.strStreamVideoResolution);

                    if(si.nVideoResolutiontype >= (int)VideoResolutionType::FHD1080)
                        b1080p = true;

                    std::string format = b1080p ? " FHD":" HD";
                    std::string strStreamNameFormat = si.strStreamName + format;
                    std::string id;
                    for(auto & cfg :veBroadcastDetailsInfo)
                    {
                        if(cfg.stream_name == strStreamNameFormat)
                        {
                            id = cfg.id;
                            break;
                        }
                    }

                    if(id.empty())
                    {
                        Logger::getInstance()->info("节目：{}未匹配到对应节目,id:{}",si.strStreamName,si.id);
                    }
                    else
                    {
                        si.target_matching_id = id;
                    }
                    // Logger::getInstance()->info("id:{}, 节目：{} ,节目id:{}, 分辨率；{}"
                    //     ,si.id,si.strStreamName, si.target_matching_id
                    //     ,si.nVideoResolutiontype);
                    //更新数据库
                    WriteSqlDbData(si);
                }

                std::string strMessage = "更新分数项目数量:" + std::to_string(vec.size());
                Logger::getInstance()->info("✅ 更新分数: {}", vec.size());

                // 上传文件到 Lark，路径就是当前目录的文件名
                if(HttpServer::sendLarkMessage(WEB_HOOK_STREAM_FAIL, strMessage))
                {
                    Logger::getInstance()->info("✅ 已上传到 Lark");
                }
                else
                {
                    Logger::getInstance()->error("❌ 上传到 Lark 失败");
                }
            }

            for (int i = 0; i < nWaitTime; ++i)
            {
                std::this_thread::sleep_for(std::chrono::minutes(1));
            }
            
        }
    }).detach();

    std::vector<StreamInfo> streamInfo;

    while (true) 
    {
        Logger::getInstance()->info("开始批次:{}检测第:{}",nWhileIdx,nIdx);
        // std::this_thread::sleep_for(std::chrono::minutes(1));
        // continue;

        if (nWhileIdx == 0)
        {
            m_whileNum ++;
            streamInfo = GetStreamInfoSqlDbData();
            if (streamInfo.empty()) 
            {
                Logger::getInstance()->warn("未解析到任何流");
                std::this_thread::sleep_for(std::chrono::seconds(nMinTime));
                continue;
            }
            else
            {
                Logger::getInstance()->info("解析到流:{}条",streamInfo.size());
            }
        }

        *stopFlag = false;
        //std::vector<std::thread> workers;
        // 当前批次的 [nIdx, nIdx+nTestNum)
        int endIdx = std::min((int)streamInfo.size(), nIdx + nTestNum);

        std::vector<pid_t> workers;

        for (int i = nIdx; i < endIdx; i++) 
        {
            pid_t pid = fork();
            if (pid == 0) {
                // ===== 子进程逻辑 =====
                const auto &cfg = streamInfo[i];
                Logger::getInstance()->debug("检测流:{} -> {}", cfg.strStreamName, cfg.strStreamPath);

                // 拉流解码
                OperationStream(cfg);

                int counter = 0;
                while (!*stopFlag) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    counter++;
                }

                Logger::getInstance()->debug("退出检测:{} -> {}", cfg.strStreamName, cfg.strStreamPath);
                _exit(0); // 子进程退出
            } 
            else if (pid > 0) 
            {
                // 父进程逻辑：保存 PID
                workers.push_back(pid);
            } 
            else 
            {
                perror("fork");
            }
        }

        // 等待 nMinTime 秒
        std::this_thread::sleep_for(std::chrono::seconds(nMinTime));
        *stopFlag = true; // 通知线程停止

        // 等待所有线程退出
        for (pid_t pid : workers) {
            waitpid(pid, nullptr, 0);
        }

        // 更新下一个批次的起始索引
        nIdx += nTestNum;
        nWhileIdx++;
        if (nIdx >= (int)streamInfo.size()) 
        {
            nIdx = 0; // 循环
            nWhileIdx = 0;
        } 

    }

    //munmap(stopFlag, sizeof(bool));
    curl_global_cleanup();
}

void StreamTest::SendCSVAsMarkdownToLark(const std::vector<std::pair<std::string, OutStreamInfo>>& vec ,int nMaxLinesPerBatch) // 通过引用返回生成的文件名
{

    // 生成 CSV 行
    std::vector<std::string> csvLines;

    // 表头
    std::string header = 
        "源ID,节目ID,节目名,源地址,流协议,码率,流长度,视频格式,视频分辨率,音频格式,采样率,"
        "打开输入流失败,获取流信息失败,查找视频流失败,查找音频流失败,未找到视频解码器,"
        "无法分配视频解码器上下文,视频解码器参数拷贝失败,打开视频解码器失败,未找到音频解码器,无法分配音频解码器上下文,"
        "音频解码器参数拷贝失败,打开音频解码器失败,获取流包失败,解码失败,读帧失败,时间戳异常,"
        "总错误数,检测次数";
    csvLines.push_back(header);

    for (const auto& [url_id, info] : vec) 
    {
        const auto& si = info.stStreamInfo;
        const auto& ei = info.stErrorItemInfo;

        std::ostringstream line;
        line << url_id << ","
             << si.target_matching_id << ","
             << si.strStreamName << ","
             << si.strStreamPath << ","
             << si.strStreamProtocol << ","
             << si.strStreamBitrate << ","
             << si.strStreamLength << ","
             << si.strStreamVideoFormat << ","
             << si.strStreamVideoResolution << ","
             << si.strStreamAudioFormat << ","
             << si.strStreamAudioSamplingRate << ","
             << ei.nOpenInputNum << ","
             << ei.nGetStreamInfoNum << ","
             << ei.nFindVideoStreamNum << ","
             << ei.nFindAudioStreamNum << ","
             << ei.nVideoDecordNotFoundNum << ","
             << ei.nAllocVideoParamsNum << ","
             << ei.nCopyVideoParamsNum << ","
             << ei.nOpenVideoDecoderNum << ","
             << ei.nAudioDecordNotFoundNum << ","
             << ei.nAllocAudioParamsNum << ","
             << ei.nCopyAudioParamsNum << ","
             << ei.nOpenAudioDecoderNum << ","
             << ei.nReadPacketNum << ","
             << ei.nDecodeFrameNum << ","
             << ei.nReadFrameNum << ","
             << ei.nDtsPtsErrorNum << ","
             << info.nErrorNum << ","
             << info.nDetectionNum;

        csvLines.push_back(line.str());
    }

    // 分批发送
    size_t totalLines = csvLines.size();
    for (size_t i = 0; i < totalLines; i += nMaxLinesPerBatch) 
    {
        std::vector<std::string> batchLines;
        batchLines.push_back(csvLines[0]); // 表头
        for (size_t j = i + 1; j < std::min(i + nMaxLinesPerBatch, totalLines); ++j) {
            batchLines.push_back(csvLines[j]);
        }

        std::string markdown = CSVLinesToMarkdown(batchLines);

        if (HttpServer::sendLarkMessage(WEB_HOOK_STREAM_FAIL, markdown)) 
        {
            Logger::getInstance()->info("✅ 已上传 {} 行到 Lark", batchLines.size() - 1);
        } 
        else 
        {
            Logger::getInstance()->error("❌ 上传到 Lark 失败");
        }
    }
}

std::string StreamTest::WriteStreamInfoCSVWithContent(const std::vector<std::pair<std::string, OutStreamInfo>>& vec, std::string& outFileName) // 通过引用返回生成的文件名
{
    outFileName = "logs/"+GetCurrentTimeString() + ".csv"; // 生成文件名
    std::ofstream ofs(outFileName);
    if(!ofs.is_open()) {
        Logger::getInstance()->warn("无法创建 CSV 文件：{}", outFileName);
        return "";
    }

    std::ostringstream oss; // 字符串流，用于保存 CSV 内容

    // 中文索引头
    std::string header = 
        "源ID,节目ID,节目名,源地址,流协议,码率,流长度,视频格式,视频分辨率,音频格式,采样率,"
        "打开输入流失败,获取流信息失败,查找视频流失败,查找音频流失败,未找到视频解码器,"
        "无法分配视频解码器上下文,视频解码器参数拷贝失败,打开视频解码器失败,未找到音频解码器,无法分配音频解码器上下文,"
        "音频解码器参数拷贝失败,打开音频解码器失败,获取流包失败,解码失败,读帧失败,时间戳异常,"
        "总错误数,检测次数\n";

    ofs << header;
    oss << header;

    for (const auto& [url_id, info] : vec) {
        const auto& si = info.stStreamInfo;
        const auto& ei = info.stErrorItemInfo;

        std::ostringstream line;
        line << url_id << ","
             << si.target_matching_id << ","
             << si.strStreamName << ","
             << si.strStreamPath << ","
             << si.strStreamProtocol << ","
             << si.strStreamBitrate << ","
             << si.strStreamLength << ","
             << si.strStreamVideoFormat << ","
             << si.strStreamVideoResolution << ","
             << si.strStreamAudioFormat << ","
             << si.strStreamAudioSamplingRate << ","
             << ei.nOpenInputNum << ","
             << ei.nGetStreamInfoNum << ","
             << ei.nFindVideoStreamNum << ","
             << ei.nFindAudioStreamNum << ","
             << ei.nVideoDecordNotFoundNum << ","
             << ei.nAllocVideoParamsNum << ","
             << ei.nCopyVideoParamsNum << ","
             << ei.nOpenVideoDecoderNum << ","
             << ei.nAudioDecordNotFoundNum << ","
             << ei.nAllocAudioParamsNum << ","
             << ei.nCopyAudioParamsNum << ","
             << ei.nOpenAudioDecoderNum << ","
             << ei.nReadPacketNum << ","
             << ei.nDecodeFrameNum << ","
             << ei.nReadFrameNum << ","
             << ei.nDtsPtsErrorNum << ","
             << info.nErrorNum << ","
             << info.nDetectionNum
             << "\n";

        ofs << line.str(); // 写入文件
        oss << line.str(); // 写入字符串
    }

    ofs.close();
    return oss.str(); // 返回 CSV 内容
}

//拉流重试 
bool StreamTest::open_input(AVFormatContext*& in_fmt_ctx, const std::string& input_url, int timeout_ms)
{
    // 清理旧的输入上下文
    if (in_fmt_ctx) 
    {
        avformat_close_input(&in_fmt_ctx);
        in_fmt_ctx = nullptr;
    }

    // 设置输入参数（比如超时）
    AVDictionary* options = nullptr;

    // 超时（毫秒 -> 微秒）
    // av_dict_set_int(&options, "rw_timeout", (int64_t)timeout_ms * 1000, 0);
    // av_dict_set(&options, "stimeout", std::to_string((int64_t)timeout_ms * 1000).c_str(), 0);

    // 网络缓存（可选）
    av_dict_set(&options, "buffer_size", "1024000", 0);   // 1MB 缓存
    av_dict_set(&options, "max_delay", "500000", 0);      // 最大 0.5s 延迟

    TimeoutData timeout_data;
    timeout_data.timeout_ms = timeout_ms;
    timeout_data.start_time = std::chrono::steady_clock::now();
    AVIOInterruptCB int_cb = {interrupt_cb, &timeout_data};
    in_fmt_ctx = avformat_alloc_context();
    in_fmt_ctx->interrupt_callback = int_cb;

    // 打开输入
    if (avformat_open_input(&in_fmt_ctx, input_url.c_str(), nullptr, &options) < 0) 
    {
        Logger::getInstance()->error("Cannot open input URL: {}",input_url);
        av_dict_free(&options);
        return false;
    }

    av_dict_free(&options);

    // 读取流信息
    if (avformat_find_stream_info(in_fmt_ctx, nullptr) < 0) 
    {
        Logger::getInstance()->error("Failed to get stream info: {}",input_url);
        avformat_close_input(&in_fmt_ctx);
        return false;
    }

    Logger::getInstance()->error("Input opened OK :{}",input_url);
    return true;
}

//负载测试
void StreamTest::OperationStream(StreamInfo stStreamInfo)
{
    AVFormatContext *fmt_ctx = nullptr;
    int video_stream_index = -1,audio_stream_index= -1;
    std::string strAudioInfo,strVideoInfo;

    Logger::getInstance()->info("[INFO] 开始拉流:{}  -> {}",stStreamInfo.strStreamName,stStreamInfo.strStreamPath);

    AVDictionary* options = nullptr;
    av_dict_set(&options, "stimeout", "5000000", 0);    // 5秒超时
    av_dict_set(&options, "buffer_size", "1024000", 0); // 增大缓冲

    TimeoutData timeout_data;
    timeout_data.timeout_ms = 10000;
    timeout_data.start_time = std::chrono::steady_clock::now();
    AVIOInterruptCB int_cb = {interrupt_cb, &timeout_data};

    fmt_ctx = avformat_alloc_context();
    //fmt_ctx->interrupt_callback = int_cb;

   // 打开输入流
    int ret = avformat_open_input(&fmt_ctx, stStreamInfo.strStreamPath.c_str(), nullptr, &options);
    if (ret < 0) 
    {
        print_ffmpeg_error(stStreamInfo,ret,OPEN_INPUT_FAILED,"无法打开流 ");
        return;
    }

    // 获取流信息
    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) 
    {
        print_ffmpeg_error(stStreamInfo,ret, STREAM_INFO_FAILED,"无法获取流信息");
        avformat_close_input(&fmt_ctx);
        return;
    }

    //打印基本信息
    // std::cout << "[INFO] 流媒体信息: " << stStreamInfo.strStreamPath << std::endl;
    // std::cout << "[INFO] 格式: " << fmt_ctx->iformat->name
    //           << ", 流数量: " << fmt_ctx->nb_streams << std::endl;
    
    stStreamInfo.strStreamProtocol = fmt_ctx->iformat->name;

    if (fmt_ctx->duration != AV_NOPTS_VALUE) 
    {
        //std::cout << "长度: " << fmt_ctx->duration / AV_TIME_BASE << " 秒" << std::endl;
        stStreamInfo.strStreamLength = std::to_string(fmt_ctx->duration / AV_TIME_BASE);
    }
    else 
    {
        stStreamInfo.strStreamLength = "N/A";
        //std::cout << "长度: N/A" << std::endl;
    }

    // 可选：直接使用 FFmpeg 内置打印（调试用）
    //av_dump_format(fmt_ctx, 0, stStreamInfo.strStreamPath.c_str(), 0);

    // 查找视频流
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) 
    {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) 
        {
            strVideoInfo = "Stream #" + std::to_string(i) + " : Video (" 
                + avcodec_get_name(fmt_ctx->streams[i]->codecpar->codec_id) + ") Resolution: " 
                + std::to_string(fmt_ctx->streams[i]->codecpar->height) + "x" + std::to_string(fmt_ctx->streams[i]->codecpar->width);

            stStreamInfo.strStreamVideoFormat = avcodec_get_name(fmt_ctx->streams[i]->codecpar->codec_id);
            stStreamInfo.strStreamVideoResolution = std::to_string(fmt_ctx->streams[i]->codecpar->width) + "x" + std::to_string(fmt_ctx->streams[i]->codecpar->height);

            video_stream_index = i;
        }
        else if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            strAudioInfo = "Stream #" + std::to_string(i) + " : Audio (" 
                + avcodec_get_name(fmt_ctx->streams[i]->codecpar->codec_id) + ") Sample Rate: "
                + std::to_string(fmt_ctx->streams[i]->codecpar->sample_rate);

            stStreamInfo.strStreamAudioFormat = avcodec_get_name(fmt_ctx->streams[i]->codecpar->codec_id);
            stStreamInfo.strStreamAudioSamplingRate = std::to_string(fmt_ctx->streams[i]->codecpar->sample_rate);

            audio_stream_index = i;
        }
    }

    // 获取码率
    if (fmt_ctx->bit_rate > 0) 
    {
        stStreamInfo.strStreamBitrate = std::to_string(fmt_ctx->bit_rate / 1000) + " kb/s";
    } 
    else if (video_stream_index >= 0 && fmt_ctx->streams[video_stream_index]->codecpar->bit_rate > 0) 
    {
        stStreamInfo.strStreamBitrate = std::to_string(fmt_ctx->streams[video_stream_index]->codecpar->bit_rate / 1000) + " kb/s";
    } 
    else 
    {
        stStreamInfo.strStreamBitrate = "N/A";
    }
    //std::cout << "码率:" << stStreamInfo.strStreamBitrate << std::endl;

    if (video_stream_index == -1) 
    {
        print_ffmpeg_error(stStreamInfo,0, FIND_VIDEO_STREAM_FAILED,"没有找到视频流",false);
        avformat_close_input(&fmt_ctx);
        return;
    }

    if (audio_stream_index == -1) 
    {
        print_ffmpeg_error(stStreamInfo,0, FIND_AUDIO_STREAM_FAILED,"没有找到音频流",false);
        avformat_close_input(&fmt_ctx);
        return;
    }

    // ---------------- 视频解码器 ----------------
    AVCodecParameters* video_codecpar = fmt_ctx->streams[video_stream_index]->codecpar;
    if (!video_codecpar) {
        print_ffmpeg_error(stStreamInfo,0,VIDEO_DECODER_NOT_FOUND,"视频流 codecpar 为空",false);
        avformat_close_input(&fmt_ctx);
        return;
    }

    const AVCodec* video_codec = avcodec_find_decoder(video_codecpar->codec_id);
    if (!video_codec) 
    {
        std::cerr << "[ERROR] 未找到视频解码器, codec_id = " << video_codecpar->codec_id << std::endl;
        print_ffmpeg_error(stStreamInfo,0,VIDEO_DECODER_NOT_FOUND,"未找到视频解码器ID:" + std::to_string(video_codecpar->codec_id),false);
        avformat_close_input(&fmt_ctx);
        return;
    }

    AVCodecContext* video_codec_ctx = avcodec_alloc_context3(video_codec);
    if (!video_codec_ctx) 
    {
        print_ffmpeg_error(stStreamInfo,0, ALLOC_VIDEO_CTX_FAILED,"无法分配视频 codec context",false);
        avformat_close_input(&fmt_ctx);
        return;
    }

    ret = avcodec_parameters_to_context(video_codec_ctx, video_codecpar);
    if (ret < 0) 
    {
        print_ffmpeg_error(stStreamInfo,ret,COPY_VIDEO_PARAMS_FAILED, "视频 codec 参数拷贝失败");
        avcodec_free_context(&video_codec_ctx);
        avformat_close_input(&fmt_ctx);
        return;
    }

    ret = avcodec_open2(video_codec_ctx, video_codec, nullptr);
    if (ret < 0) 
    {
        print_ffmpeg_error(stStreamInfo,ret,OPEN_VIDEO_DECODER_FAILED, "打开视频解码器失败");
        avcodec_free_context(&video_codec_ctx);
        avformat_close_input(&fmt_ctx);
        return;
    }

    // ---------------- 音频解码器 ----------------
    AVCodecParameters* audio_codecpar = fmt_ctx->streams[audio_stream_index]->codecpar;
    const AVCodec* audio_codec = avcodec_find_decoder(audio_codecpar->codec_id);
    if (!audio_codec) 
    {
        print_ffmpeg_error(stStreamInfo,0,AUDIO_DECODER_NOT_FOUND, "未找到音频解码器:" + std::to_string(audio_codecpar->codec_id),false);
        avcodec_free_context(&video_codec_ctx);
        avformat_close_input(&fmt_ctx);
        return;
    }

    AVCodecContext* audio_codec_ctx = avcodec_alloc_context3(audio_codec);
    if (!audio_codec_ctx) 
    {
        print_ffmpeg_error(stStreamInfo,0, ALLOC_AUDIO_CTX_FAILED,"无法分配音频 codec context",false);
        avcodec_free_context(&video_codec_ctx);
        avformat_close_input(&fmt_ctx);
        return;
    }

    ret = avcodec_parameters_to_context(audio_codec_ctx, audio_codecpar);
    if (ret < 0) 
    {
        print_ffmpeg_error(stStreamInfo,ret, COPY_AUDIO_PARAMS_FAILED,"音频 codec 参数拷贝失败");
        avcodec_free_context(&video_codec_ctx);
        avcodec_free_context(&audio_codec_ctx);
        avformat_close_input(&fmt_ctx);
        return;
    }

    ret = avcodec_open2(audio_codec_ctx, audio_codec, nullptr);
    if (ret < 0) 
    {
        print_ffmpeg_error(stStreamInfo,ret, OPEN_AUDIO_DECODER_FAILED,"打开音频解码器失败");
        avcodec_free_context(&video_codec_ctx);
        avcodec_free_context(&audio_codec_ctx);
        avformat_close_input(&fmt_ctx);
        return;
    }

    //std::cout << "[INFO] 视频解码器和音频解码器初始化成功" << std::endl;

    // 准备读取帧
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    int video_frame_count = 0;
    int audio_frame_count = 0;

    bool bDecodePacketState = true;//解码结果状态
    int retRead = 0;
    int64_t last_dts  = AV_NOPTS_VALUE;
    int64_t last_pts  = AV_NOPTS_VALUE;

    while (retRead >= 0 && bDecodePacketState) 
    {
        retRead = av_read_frame(fmt_ctx, pkt);
        if (retRead < 0) 
        {
            print_ffmpeg_error(stStreamInfo,retRead, READ_FRAME_FAILED,"av_read_frame 失败:");
            bDecodePacketState = false;
            break; // 跳出循环
        }

        //if (pkt->stream_index == video_stream_index || pkt->stream_index == audio_stream_index) 
        if (false) 
        {
            // 检查 DTS 单调递增
            if (pkt->dts != AV_NOPTS_VALUE) 
            {
                if (last_dts != AV_NOPTS_VALUE && pkt->dts <= last_dts) 
                {
                    std::string strError = "DTS 异常回退，退出解码 dts:" + std::to_string(pkt->dts) + "last_dts" + std::to_string(last_dts);
                    print_ffmpeg_error(stStreamInfo,retRead, DTS_PTS_FAILED, strError);
                    bDecodePacketState = false;
                    av_packet_unref(pkt);
                    break; // 直接跳出
                }
                last_dts = pkt->dts;
            }

            // 检查 PTS ≥ DTS
            if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) 
            {
                std::string strError = "PTS < DTS 异常，退出解码 dts:" + std::to_string(pkt->dts) + "pts:" + std::to_string(pkt->pts);
                print_ffmpeg_error(stStreamInfo,retRead, DTS_PTS_FAILED,strError);
                bDecodePacketState = false;
                av_packet_unref(pkt);
                break; // 直接跳出
            }

            // 检查 PTS 单调递增（可选）
            if (pkt->pts != AV_NOPTS_VALUE) 
            {
                if (last_pts != AV_NOPTS_VALUE && pkt->pts < last_pts) 
                {
                    std::string strError = "PTS 异常回退，退出解码pts:" + std::to_string(pkt->pts) + "last_pts:" + std::to_string(last_pts);
                    print_ffmpeg_error(stStreamInfo,retRead, DTS_PTS_FAILED,strError);
                    bDecodePacketState = false;
                    av_packet_unref(pkt);
                    break; // 直接跳出
                }
                last_pts = pkt->pts;
            }
        }

        if (pkt->stream_index == video_stream_index) 
        {
            DecodePacket(bDecodePacketState,stStreamInfo,video_codec_ctx, pkt, video_frame_count);
        } 
        else if (pkt->stream_index == audio_stream_index) 
        {
            DecodePacket(bDecodePacketState,stStreamInfo,audio_codec_ctx, pkt, audio_frame_count);
        }

        av_packet_unref(pkt);

        if (*stopFlag) 
        {
            break;
        }
    }

    //提交解码检测结果
    if (bDecodePacketState)
    {
        //记录成功
        print_ffmpeg_success(stStreamInfo);
    }


    //std::cout << "[INFO] 视频解码完成，共 " << video_frame_count << " 帧" << std::endl;
    //std::cout << "[INFO] 音频解码完成，共 " << audio_frame_count << " 帧" << std::endl;

    // 释放资源
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&video_codec_ctx);
    avcodec_free_context(&audio_codec_ctx);
    avformat_close_input(&fmt_ctx);

}

void StreamTest::DecodePacket(bool & bDecodePacketState,StreamInfo stStreamInfo,AVCodecContext* codec_ctx, AVPacket* pkt, int& frame_count)
{
    int ret = avcodec_send_packet(codec_ctx, pkt);
    if (ret < 0) 
    {
        bDecodePacketState = false;
        print_ffmpeg_error(stStreamInfo,ret, READ_PACKET_FAILED,"avcodec_send_packet 失败:");
        return;
    } 

    AVFrame* frame = av_frame_alloc();
    while (true) {
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == 0) 
        {
            frame_count++;
            if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) 
            {
                // char buf[256];
                // snprintf(buf, sizeof(buf),
                //         "[VIDEO] 帧 #%d 格式=%s 分辨率=%dx%d",
                //         frame_count,
                //         av_get_pix_fmt_name((AVPixelFormat)frame->format),
                //         frame->width,
                //         frame->height);
            } 
            else if (codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) 
            {
                // char buf[256];
                // snprintf(buf, sizeof(buf),
                //         "[AUDIO] 帧 #%d 格式=%s 采样率=%d 采样点数=%d",
                //         frame_count,
                //         av_get_sample_fmt_name((AVSampleFormat)frame->format),
                //         frame->sample_rate,
                //         frame->nb_samples);
            }
        } 
        else if (ret == AVERROR(EAGAIN)) 
        {
            // std::cout << "[INFO] avcodec_receive_frame: 需要更多数据 (EAGAIN)" << std::endl;
            break;
        } 
        else if (ret == AVERROR_EOF) 
        {
            //std::cout << "[INFO] avcodec_receive_frame: 解码结束 (EOF)" << std::endl;
            break;
        } 
        else 
        {
            bDecodePacketState = false;
            print_ffmpeg_error(stStreamInfo,ret,DECODE_FRAME_FAILED, "avcodec_receive_frame 失败:");
            break;
        }
    }
    av_frame_free(&frame);
}