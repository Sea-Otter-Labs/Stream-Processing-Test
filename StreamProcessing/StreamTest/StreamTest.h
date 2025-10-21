#pragma once
#include <fstream>
#include <sstream>
#include <vector>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <map>
#include <nlohmann/json.hpp>
#include "HttpServer.h"
#include <iomanip>
#include <mysql/mysql.h>

using json = nlohmann::json;

#define SERVER_HOST "http://45.134.141.110"
// 服务器端口
#define SERVER_PORT 8003
#define WEB_HOOK_STREAM_FAIL "https://open.larksuite.com/open-apis/bot/v2/hook/de65a92f-2039-4ce4-bfcb-df65bdad1523"

#define SQL_HOST "database-1.cn4csgi60ope.eu-west-3.rds.amazonaws.com"
#define SQL_USER "pplive"
#define SQL_PASSWD "pplive123$"
#define SQL_DBNAME "stream_manage"
//#define SQL_DBNAME_1 "attribution"

#define SQL_PORT 3306

enum HTTP_TYPE
{
    HTTP_POST,
    HTTP_GET
};

enum StreamError {
    OPERATION_OK = 0,            // 成功
    OPEN_INPUT_FAILED = 1001,    // 打开输入流失败
    STREAM_INFO_FAILED,          // 获取流信息失败
    FIND_VIDEO_STREAM_FAILED,    // 查找视频流失败
    FIND_AUDIO_STREAM_FAILED,    // 查找音频流失败
    VIDEO_DECODER_NOT_FOUND,     // 未找到视频解码器
    ALLOC_VIDEO_CTX_FAILED,      // 无法分配视频解码器上下文
    COPY_VIDEO_PARAMS_FAILED,    // 视频解码器参数拷贝失败
    OPEN_VIDEO_DECODER_FAILED,   // 打开视频解码器失败
    AUDIO_DECODER_NOT_FOUND,     // 未找到音频解码器
    ALLOC_AUDIO_CTX_FAILED,      // 无法分配音频解码器上下文
    COPY_AUDIO_PARAMS_FAILED,    // 音频解码器参数拷贝失败
    OPEN_AUDIO_DECODER_FAILED,   // 打开音频解码器失败
    READ_PACKET_FAILED,          // 获取流包失败
    DECODE_FRAME_FAILED,         // 解码失败
    READ_FRAME_FAILED,           // 读帧失败
    DTS_PTS_FAILED,              // 时间戳异常
};

// 错误码映射表
static const std::unordered_map<StreamError, std::string> StreamErrorMap = {
    { OPERATION_OK,              "成功" },
    { OPEN_INPUT_FAILED,         "打开输入流失败" },
    { STREAM_INFO_FAILED,        "获取流信息失败" },
    { FIND_VIDEO_STREAM_FAILED,  "查找视频流失败" },
    { FIND_AUDIO_STREAM_FAILED,  "查找音频流失败" },
    { VIDEO_DECODER_NOT_FOUND,   "未找到视频解码器" },
    { ALLOC_VIDEO_CTX_FAILED,    "无法分配视频解码器上下文" },
    { COPY_VIDEO_PARAMS_FAILED,  "视频解码器参数拷贝失败" },
    { OPEN_VIDEO_DECODER_FAILED, "打开视频解码器失败" },
    { AUDIO_DECODER_NOT_FOUND,   "未找到音频解码器" },
    { ALLOC_AUDIO_CTX_FAILED,    "无法分配音频解码器上下文" },
    { COPY_AUDIO_PARAMS_FAILED,  "音频解码器参数拷贝失败" },
    { OPEN_AUDIO_DECODER_FAILED, "打开音频解码器失败" },
    { READ_PACKET_FAILED,        "获取流包失败" },
    { DECODE_FRAME_FAILED,       "解码失败" },
    { READ_FRAME_FAILED,         "读帧失败" },
    { DTS_PTS_FAILED,            "时间戳异常" }
};

extern "C" {
    #include "../3rdparty/ffmpeg/include/libavformat/avformat.h"
    #include "../3rdparty/ffmpeg/include/libavcodec/avcodec.h"
    #include "../3rdparty/ffmpeg/include/libavutil/imgutils.h"
    #include "../3rdparty/ffmpeg/include/libavutil/opt.h"
    #include "../3rdparty/ffmpeg/include/libavutil/channel_layout.h"
    #include "../3rdparty/ffmpeg/include/libavutil/time.h"
    #include "../3rdparty/ffmpeg/include/libswscale/swscale.h"
    #include "../3rdparty/ffmpeg/include/libswresample/swresample.h"
}

/*
    flow_address
    item
    return_value
    lag_details
    streaming_protocol
    bitrate
    stream_length
    video_format
    video_resolution
    audio_format
    audio_sampling_rate
    created_time
    target_matching_id
    target_matching
    url_id
*/

enum class VideoResolutionType {
    UNKNOWN = 1,
    QQVGA,      // 160x120
    QVGA,       // 320x240
    NHD,        // 640x360
    VGA,        // 640x480
    SD480,      // 720x480
    SD576,      // 720x576
    SVGA,       // 800x600
    FWVGA,      // 854x480
    QHD540,     // 960x540
    WSVGA,      // 1024x576
    XGA,        // 1024x768
    HD720,      // 1280x720
    WXGA,       // 1280x800
    WXGA_PLUS,  // 1366x768
    HD_PLUS,    // 1600x900
    FHD1080,    // 1920x1080
    DCI2K,      // 2048x1080
    QHD1440,    // 2560x1440
    RETINA2880, // 2880x1800
    QHD_PLUS,   // 3200x1800
    UHD4K,      // 3840x2160
    DCI4K,      // 4096x2160
    UHD5K,      // 5120x2880
    UHD8K,      // 7680x4320
    DCI8K,      // 8192x4320
    CUSTOM      // 非标准分辨率
};

// 映射：字符串 -> 枚举类型
static const std::map<std::string, VideoResolutionType> resolutionMap = 
{
    {"160x120",   VideoResolutionType::QQVGA},
    {"320x240",   VideoResolutionType::QVGA},
    {"640x360",   VideoResolutionType::NHD},
    {"640x480",   VideoResolutionType::VGA},
    {"720x480",   VideoResolutionType::SD480},
    {"720x576",   VideoResolutionType::SD576},
    {"800x600",   VideoResolutionType::SVGA},
    {"854x480",   VideoResolutionType::FWVGA},
    {"960x540",   VideoResolutionType::QHD540},
    {"1024x576",  VideoResolutionType::WSVGA},
    {"1024x768",  VideoResolutionType::XGA},
    {"1280x720",  VideoResolutionType::HD720},
    {"1280x800",  VideoResolutionType::WXGA},
    {"1366x768",  VideoResolutionType::WXGA_PLUS},
    {"1600x900",  VideoResolutionType::HD_PLUS},
    {"1920x1080", VideoResolutionType::FHD1080},
    {"2048x1080", VideoResolutionType::DCI2K},
    {"2560x1440", VideoResolutionType::QHD1440},
    {"2880x1800", VideoResolutionType::RETINA2880},
    {"3200x1800", VideoResolutionType::QHD_PLUS},
    {"3840x2160", VideoResolutionType::UHD4K},
    {"4096x2160", VideoResolutionType::DCI4K},
    {"5120x2880", VideoResolutionType::UHD5K},
    {"7680x4320", VideoResolutionType::UHD8K},
    {"8192x4320", VideoResolutionType::DCI8K},
};

inline VideoResolutionType getResolutionType(const std::string& res) 
{
    auto it = resolutionMap.find(res);
    if (it != resolutionMap.end()) 
    {
        return it->second;
    }
    
    if (res == "0x0" || res.empty()) 
    {
        return VideoResolutionType::UNKNOWN;
    }
    return VideoResolutionType::CUSTOM;
}

struct StreamRecord
{
    std::string flow_address;
    int item;
    std::string return_value;
    std::string lag_details;
    std::string streaming_protocol;
    std::string bitrate;
    std::string stream_length;
    std::string video_format;
    std::string video_resolution;
    std::string audio_format;
    std::string audio_sampling_rate;
    std::string created_time;       // UTC 字符串
    std::string target_matching_id;
    std::string target_matching;
    std::string url_id;
};

struct StreamInfo
{
    std::string id;                 //源id
    std::string strStreamPath;      //源地址
    std::string strStreamName;      //节目名
    std::string target_matching_id; //节目id

    std::string strStreamProtocol;          //流协议
    std::string strStreamBitrate;           //码率
    std::string strStreamLength;            //流长度
    std::string strStreamVideoFormat;       //视频格式
    std::string strStreamVideoResolution;   //视频分辨率
    std::string strStreamAudioFormat;       //音频格式
    std::string strStreamAudioSamplingRate; //采样率
    int  nFlowScore;                        //分数质量
    int  nVideoResolutiontype;              //分辨率类型
};

//节目
struct BroadcastDetailsInfo
{
    std::string id;                 //节目ID
    std::string stream_name;        //节目名
};

struct ErrorItemInfo
{
    int nOpenInputNum = 0;
    int nGetStreamInfoNum = 0;
    int nFindVideoStreamNum = 0;
    int nFindAudioStreamNum = 0;
    int nVideoDecordNotFoundNum = 0;
    int nAllocVideoParamsNum = 0;
    int nCopyVideoParamsNum = 0;
    int nOpenVideoDecoderNum = 0;
    int nAudioDecordNotFoundNum = 0;
    int nAllocAudioParamsNum = 0;
    int nCopyAudioParamsNum = 0;
    int nOpenAudioDecoderNum = 0;
    int nReadFrameNum = 0;
    int nDtsPtsErrorNum = 0;
    int nReadPacketNum = 0;
    int nDecodeFrameNum = 0;
};

struct OutStreamInfo
{
    StreamInfo stStreamInfo;
    ErrorItemInfo stErrorItemInfo;
    int nErrorNum = 0;
    int nDetectionNum = 0;//检测次数
};

class StreamTest
{
public:
    StreamTest();
    ~StreamTest();
    void start();

private:
    std::map<std::string ,AVFormatContext*> m_mapAVFormatContext;
    //std::atomic<bool> stopFlag{false};
    bool *stopFlag = nullptr;
    int m_whileNum = 0; //循环次数

    void SendCSVAsMarkdownToLark(const std::vector<std::pair<std::string, OutStreamInfo>>& vec , int nMaxLine);
    std::string WriteStreamInfoCSVWithContent(const std::vector<std::pair<std::string, OutStreamInfo>>& vec, std::string& outFileName);

    std::vector<StreamInfo> GetStreamInfoSqlDbData();
    std::vector<BroadcastDetailsInfo> GetBroadcastDetailsInfoSqlDbData();

    void WriteSqlDbData(const StreamInfo &stOutputStreamInfo);
    void addSqlDbData(const json &j);
    std::vector<StreamRecord> queryStreamRecords(const std::string& startTime,const std::string& endTime);
    bool open_input(AVFormatContext*& in_fmt_ctx, const std::string& input_url, int timeout_ms = 5000);

    //测试推流
    void HttpPostOperation(StreamError enStreamError, std::string strLog,int ret, StreamInfo stStreamInfo);
    void print_ffmpeg_error(StreamInfo stStreamInfo,int ret, StreamError enStreamError, const std::string &msg,bool bState = true);
    void print_ffmpeg_success(StreamInfo stStreamInfo);

    //拉流解码
    void OperationStream(StreamInfo stStreamInfo);
    //输出结果
    void DecodePacket(bool & bDecodePacketState,StreamInfo stStreamInfo,AVCodecContext* codec_ctx, AVPacket* pkt, int& frame_count);
};
