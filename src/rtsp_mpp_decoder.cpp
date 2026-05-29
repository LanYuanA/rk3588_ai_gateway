#include "rtsp_mpp_decoder.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/base/gstbasesink.h>
#include <gst/gst.h>

namespace {

void ensureGstInitialized() {
    static bool initialized = false;
    if (!initialized) {
        gst_init(nullptr, nullptr);
        initialized = true;
    }
}

} // namespace

RtspMppDecoder::~RtspMppDecoder() {
    close();
}

bool RtspMppDecoder::open(const std::string& rtsp_url) {
    close();
    ensureGstInitialized();

    rtsp_url_ = rtsp_url;
    struct Candidate {
        const char* name;
        std::string desc;
    };

    std::vector<Candidate> candidates = {
        {
            "H264",
            "rtspsrc location=\"" + rtsp_url + "\" protocols=tcp latency=50 drop-on-latency=true "
            "! rtph264depay ! h264parse ! mppvideodec "
            "! video/x-raw,format=NV12 "
            "! appsink name=sink emit-signals=false sync=false max-buffers=1 drop=true"
        },
        {
            "H265",
            "rtspsrc location=\"" + rtsp_url + "\" protocols=tcp latency=50 drop-on-latency=true "
            "! rtph265depay ! h265parse ! mppvideodec "
            "! video/x-raw,format=NV12 "
            "! appsink name=sink emit-signals=false sync=false max-buffers=1 drop=true"
        }
    };

    for (const auto& candidate : candidates) {
        GError* error = nullptr;
        GstElement* pipeline = GST_ELEMENT(gst_parse_launch(candidate.desc.c_str(), &error));
        if (!pipeline) {
            if (error != nullptr) {
                std::cerr << "[RTSP-MPP] " << candidate.name << " 管线创建失败: "
                          << error->message << std::endl;
                g_error_free(error);
            }
            continue;
        }
        //找到前面name为sink的参数，获取这些参数
        GstElement* app_sink_element = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
        if (!app_sink_element) {
            std::cerr << "[RTSP-MPP] " << candidate.name << " 管线无法获取 appsink" << std::endl;
            gst_object_unref(pipeline);
            continue;
        }
        //把GstElement转换成GstAppSink
        GstAppSink *app_sink = GST_APP_SINK(app_sink_element);
        //这里下面这四个就是sink里面的那些参数，进行设置就行
        gst_app_sink_set_emit_signals(app_sink, FALSE); //不发信号，用主动pull的方式取帧
        gst_base_sink_set_sync(GST_BASE_SINK(app_sink), FALSE); //不同步时钟，尽快出帧
        gst_app_sink_set_drop(app_sink, TRUE);//缓冲区满时丢弃旧帧
        gst_app_sink_set_max_buffers(app_sink, 1); //只保留最新一帧，其他直接丢

        GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            std::cerr << "[RTSP-MPP] " << candidate.name << " 管线启动失败" << std::endl;
            gst_object_unref(app_sink_element);
            gst_object_unref(pipeline);
            continue;
        }
        //获取管道的消息总线
        GstBus *bus = gst_element_get_bus(pipeline);
        //等两秒，只关注报错或者警告信息，没有就正常返回NULL
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus,
            2 * GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));

        bool failed = false;
        //说明有报错信息了
        if (msg != nullptr) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                GError* gst_error = nullptr;
                gchar* debug_info = nullptr;
                gst_message_parse_error(msg, &gst_error, &debug_info);
                std::cerr << "[RTSP-MPP] " << candidate.name << " 管线错误: "
                          << (gst_error ? gst_error->message : "unknown") << std::endl;
                if (debug_info) {
                    std::cerr << "[RTSP-MPP] debug: " << debug_info << std::endl;
                }
                if (gst_error) {
                    g_error_free(gst_error);
                }
                if (debug_info) {
                    g_free(debug_info);
                }
                failed = true;
            }
            gst_message_unref(msg);
        }
        gst_object_unref(bus);

        if (failed) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(app_sink_element);
            gst_object_unref(pipeline);
            continue;
        }

        pipeline_ = pipeline;
        appsink_ = app_sink_element;
        std::cout << "[RTSP-MPP] " << rtsp_url << " 使用 " << candidate.name << " + mppvideodec" << std::endl;
        return true;
    }

    std::cerr << "[RTSP-MPP] 所有 MPP RTSP 管线尝试失败: " << rtsp_url << std::endl;
    return false;
}

bool RtspMppDecoder::read(cv::Mat& nv12_frame, int64_t& timestamp_ms) {
    if (!appsink_) {
        return false;
    }
    //尝试拉一帧下来，能拉下来才能继续
    GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), 2 * GST_SECOND);
    if (!sample) {
        return false;
    }
    //对拉下来的样本进行分析
    GstCaps* caps = gst_sample_get_caps(sample);
    if (caps != nullptr) {
      //获取样本数据的宽高
        GstStructure* structure = gst_caps_get_structure(caps, 0);
        if (structure != nullptr) {
            int width = 0;
            int height = 0;
            if (gst_structure_get_int(structure, "width", &width)) {
                width_ = width;
            }
            if (gst_structure_get_int(structure, "height", &height)) {
                height_ = height;
            }
        }
    }
    //获取样本数据
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer || width_ <= 0 || height_ <= 0) {
        gst_sample_unref(sample);
        return false;
    }
    //把buffer的内存进行映射，映射后的参数存在map_info里
    GstMapInfo map_info{};
    if (!gst_buffer_map(buffer, &map_info, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return false;
    }

    const size_t expected_size = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 3 / 2;
    if (map_info.size < expected_size) {
        gst_buffer_unmap(buffer, &map_info);
        gst_sample_unref(sample);
        return false;
    }

    nv12_frame.create(height_ * 3 / 2, width_, CV_8UC1);
    //cpu进行数据搬运，把拉流下来的帧搬到nv12_frame
    std::memcpy(nv12_frame.data, map_info.data, expected_size);

    GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (pts != GST_CLOCK_TIME_NONE) {
        timestamp_ms = static_cast<int64_t>(gst_util_uint64_scale(pts, 1, GST_MSECOND));
    } else {
        timestamp_ms = 0; // 不再使用实际时间戳，而是使用0作为占位符
    }

    gst_buffer_unmap(buffer, &map_info);
    gst_sample_unref(sample);
    return true;
}

void RtspMppDecoder::close() {
    if (appsink_) {
        gst_object_unref(appsink_);
        appsink_ = nullptr;
    }
    if (pipeline_) {
        gst_element_set_state(GST_ELEMENT(pipeline_), GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
}
