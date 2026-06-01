#include "streamer_thread.h"

#include <chrono>
#include <array>
#include <cstring>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <thread>
#include <vector>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include "app_context.h"
#include "realtime_composer.h"
#include "stitcher.h"
#include "mpp_encoder.h"

namespace {

// 预分配内存池：g_malloc 一次，GStreamer buffer 复用，零分配零拷贝
struct Nv12BufferPool {
    guint8* data = nullptr;    // g_malloc 预分配
    gsize capacity = 0;

    bool init(gsize size) {
        data = static_cast<guint8*>(g_malloc(size));
        if (!data) return false;
        capacity = size;
        return true;
    }

    void destroy() {
        if (data) { g_free(data); data = nullptr; }
        capacity = 0;
    }
};

// GStreamer 推送上下文
struct GstPushContext;
void gstPushClose(GstPushContext& ctx);

struct GstPushContext {
    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    Nv12BufferPool pool;       // 预分配 NV12 内存池
    GstBuffer* gst_buf = nullptr;  // 复用的 GstBuffer 对象
    guint64 frame_count = 0;
};

// GstBuffer 销毁时的回调：把内存归还给池而不是释放
static void poolFreeCallback(gpointer data) {
    g_free(data);
}

bool gstPushOpen(GstPushContext& ctx) {
    gst_init(nullptr, nullptr);

    // 预分配 NV12 内存池（1280×960×1.5 = 1843200 字节）
    const gsize nv12_size = 1280 * 960 * 3 / 2;
    if (!ctx.pool.init(nv12_size)) {
        std::cerr << "[推流线程] NV12 内存池分配失败" << std::endl;
        return false;
    }

    // 创建可复用的 GstBuffer，指向预分配的内存
    ctx.gst_buf = gst_buffer_new_wrapped_full(
        static_cast<GstMemoryFlags>(0),
        ctx.pool.data, ctx.pool.capacity,
        0, ctx.pool.capacity,
        nullptr, nullptr);  // 不设 destroy notify，内存由池管理
    if (!ctx.gst_buf) {
        std::cerr << "[推流线程] GstBuffer 创建失败" << std::endl;
        ctx.pool.destroy();
        return false;
    }

    GError* error = nullptr;
    const char* desc =
        "appsrc name=src is-live=true block=false format=time "
        "caps=video/x-raw,format=NV12,width=1280,height=960,framerate=30/1 "
        "! mpph264enc "
        "! h264parse config-interval=1 "
        "! rtspclientsink location=rtsp://127.0.0.1:8554/gateway_out protocols=tcp";

    ctx.pipeline = gst_parse_launch(desc, &error);
    if (!ctx.pipeline) {
        if (error) {
            std::cerr << "[推流线程] GStreamer 管线创建失败: " << error->message << std::endl;
            g_error_free(error);
        }
        gst_buffer_unref(ctx.gst_buf);
        ctx.pool.destroy();
        return false;
    }

    ctx.appsrc = gst_bin_get_by_name(GST_BIN(ctx.pipeline), "src");
    if (!ctx.appsrc) {
        std::cerr << "[推流线程] 无法获取 appsrc 元素" << std::endl;
        gst_buffer_unref(ctx.gst_buf);
        gst_object_unref(ctx.pipeline);
        ctx.pool.destroy();
        ctx.gst_buf = nullptr;
        ctx.pipeline = nullptr;
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(ctx.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[推流线程] GStreamer 管线启动失败" << std::endl;
        gstPushClose(ctx);
        return false;
    }

    GstBus* bus = gst_element_get_bus(ctx.pipeline);
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, 2 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
    if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        std::cerr << "[推流线程] GStreamer 管线错误: "
                  << (err ? err->message : "unknown") << std::endl;
        if (err) g_error_free(err);
        if (dbg) g_free(dbg);
        gst_message_unref(msg);
        gst_object_unref(bus);
        gstPushClose(ctx);
        return false;
    }
    if (msg) gst_message_unref(msg);
    gst_object_unref(bus);

    std::cout << "[推流线程] GStreamer NV12 推流管线就绪 (内存池已预分配 "
              << nv12_size << " 字节)" << std::endl;
    return true;
}

bool gstPushNv12Frame(GstPushContext& ctx, const cv::Mat& nv12_frame) {
    if (!ctx.appsrc || !ctx.gst_buf || nv12_frame.empty()) return false;

    const gsize data_size = nv12_frame.total() * nv12_frame.elemSize();

    // 直接写入预分配的内存池（零分配）
    std::memcpy(ctx.pool.data, nv12_frame.data, data_size);

    // 更新 GstBuffer 的时间戳（每帧重新设置）
    GstClockTime duration = gst_util_uint64_scale(GST_SECOND, 1, 30);
    GST_BUFFER_PTS(ctx.gst_buf) = ctx.frame_count * duration;
    GST_BUFFER_DTS(ctx.gst_buf) = ctx.frame_count * duration;
    GST_BUFFER_DURATION(ctx.gst_buf) = duration;
    ctx.frame_count++;

    // ref 后 push（push 接管引用，原始 gst_buf 保留供下帧复用）
    GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(ctx.appsrc),
                                                   gst_buffer_ref(ctx.gst_buf));
    if (flow != GST_FLOW_OK) {
        std::cerr << "[推流线程] 推送失败: " << gst_flow_get_name(flow) << std::endl;
        return false;
    }
    return true;
}

void gstPushClose(GstPushContext& ctx) {
    if (ctx.appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(ctx.appsrc));
        gst_object_unref(ctx.appsrc);
        ctx.appsrc = nullptr;
    }
    if (ctx.gst_buf) {
        gst_buffer_unref(ctx.gst_buf);
        ctx.gst_buf = nullptr;
    }
    if (ctx.pipeline) {
        gst_element_set_state(ctx.pipeline, GST_STATE_NULL);
        gst_object_unref(ctx.pipeline);
        ctx.pipeline = nullptr;
    }
    ctx.pool.destroy();
}

// H265 GStreamer推流上下文
struct GstH265PushContext {
    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    guint64 frame_count = 0;
};

bool gstH265PushOpen(GstH265PushContext& ctx, const char* rtsp_url) {
    gst_init(nullptr, nullptr);

    GError* error = nullptr;
    // H265推流管线：appsrc接收H265裸流 -> h265parse解析 -> rtspclientsink推送
    std::string desc = std::string(
        "appsrc name=src is-live=true block=false format=time "
        "caps=video/x-h265,stream-format=byte-stream,alignment=au,width=1280,height=960,framerate=30/1 "
        "! h265parse config-interval=1 "
        "! rtspclientsink location=") + rtsp_url + " protocols=tcp";

    ctx.pipeline = gst_parse_launch(desc.c_str(), &error);
    if (!ctx.pipeline) {
        if (error) {
            std::cerr << "[H265推流] GStreamer管线创建失败: " << error->message << std::endl;
            g_error_free(error);
        }
        return false;
    }

    ctx.appsrc = gst_bin_get_by_name(GST_BIN(ctx.pipeline), "src");
    if (!ctx.appsrc) {
        std::cerr << "[H265推流] 无法获取appsrc元素" << std::endl;
        gst_object_unref(ctx.pipeline);
        ctx.pipeline = nullptr;
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(ctx.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[H265推流] 管线启动失败" << std::endl;
        gst_object_unref(ctx.appsrc);
        gst_object_unref(ctx.pipeline);
        ctx.appsrc = nullptr;
        ctx.pipeline = nullptr;
        return false;
    }

    // 检查管线是否正常
    GstBus* bus = gst_element_get_bus(ctx.pipeline);
    GstMessage* msg = gst_bus_timed_pop_filtered(bus, 2 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
    if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        std::cerr << "[H265推流] 管线错误: " << (err ? err->message : "unknown") << std::endl;
        if (err) g_error_free(err);
        if (dbg) g_free(dbg);
        gst_message_unref(msg);
        gst_object_unref(bus);
        gst_object_unref(ctx.appsrc);
        gst_object_unref(ctx.pipeline);
        ctx.appsrc = nullptr;
        ctx.pipeline = nullptr;
        return false;
    }
    if (msg) gst_message_unref(msg);
    gst_object_unref(bus);

    std::cout << "[H265推流] GStreamer H265推流管线就绪: " << rtsp_url << std::endl;
    return true;
}

bool gstH265PushFrame(GstH265PushContext& ctx, const std::vector<uint8_t>& h265_data) {
    if (!ctx.appsrc || h265_data.empty()) return false;

    // 创建GstBuffer，包装H265数据
    GstBuffer* buf = gst_buffer_new_allocate(nullptr, h265_data.size(), nullptr);
    gst_buffer_fill(buf, 0, h265_data.data(), h265_data.size());

    // 设置时间戳
    GstClockTime duration = gst_util_uint64_scale(GST_SECOND, 1, 30);
    GST_BUFFER_PTS(buf) = ctx.frame_count * duration;
    GST_BUFFER_DTS(buf) = ctx.frame_count * duration;
    GST_BUFFER_DURATION(buf) = duration;
    ctx.frame_count++;

    // 推送H265数据
    GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(ctx.appsrc), buf);
    if (flow != GST_FLOW_OK) {
        std::cerr << "[H265推流] 推送失败: " << gst_flow_get_name(flow) << std::endl;
        return false;
    }
    return true;
}

void gstH265PushClose(GstH265PushContext& ctx) {
    if (ctx.appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(ctx.appsrc));
        gst_object_unref(ctx.appsrc);
        ctx.appsrc = nullptr;
    }
    if (ctx.pipeline) {
        gst_element_set_state(ctx.pipeline, GST_STATE_NULL);
        gst_object_unref(ctx.pipeline);
        ctx.pipeline = nullptr;
    }
}

} // namespace

void streamerThread() {
    std::cout << "[推流线程] 启动，4路融合 + MPP H265 硬件编码 + RTSP推流" << std::endl;

    // 初始化实时合成器
    RealtimeComposer composer;
    composer.init(1280, 960);
    g_realtime_composer = &composer;

    // 初始化MPP H265编码器
    MppH265Encoder mpp_encoder;
    bool use_mpp_encoder = mpp_encoder.init(1280, 960, 30, 4000000);  // 4Mbps

    // 初始化H265 GStreamer推流
    GstH265PushContext h265_gst_ctx;
    bool use_h265_gst = false;
    if (use_mpp_encoder) {
        use_h265_gst = gstH265PushOpen(h265_gst_ctx, "rtsp://127.0.0.1:8554/gateway_out");
        if (use_h265_gst) {
            std::cout << "[推流线程] 使用MPP H265编码 + GStreamer推流" << std::endl;
        } else {
            std::cerr << "[推流线程-警告] H265 GStreamer推流初始化失败" << std::endl;
        }
    }

    // GStreamer备用方案（H264）
    GstPushContext gst_ctx;
    bool use_gst_direct = false;
    if (!use_mpp_encoder) {
        use_gst_direct = gstPushOpen(gst_ctx);
    }

    int frame_count = 0;
    auto last_frame_start = std::chrono::steady_clock::now();

    while (g_system_running) {
        auto frame_start = std::chrono::steady_clock::now();

        cv::Mat final_grid = composer.getCurrentGrid();
        if (final_grid.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        auto t1 = std::chrono::steady_clock::now();

        if (use_mpp_encoder && use_h265_gst) {
            // MPP H265编码 + GStreamer推流
            cv::Mat nv12_frame;
            convertGridBgrToNv12WithRga(final_grid, nv12_frame);
            std::vector<uint8_t> h265_data;
            if (mpp_encoder.encode(nv12_frame, h265_data)) {
                gstH265PushFrame(h265_gst_ctx, h265_data);
            }
        } else if (use_gst_direct) {
            // GStreamer备用方案（H264）
            cv::Mat nv12_frame;
            convertGridBgrToNv12WithRga(final_grid, nv12_frame);
            if (nv12_frame.empty()) {
                cv::Mat i420;
                cv::cvtColor(final_grid, i420, cv::COLOR_BGR2YUV_I420);
                nv12_frame.create(960 * 3 / 2, 1280, CV_8UC1);
                const int y_size = 1280 * 960;
                const int uv_size = y_size / 4;
                std::memcpy(nv12_frame.data, i420.data, y_size);
                const uint8_t* u_plane = i420.data + y_size;
                const uint8_t* v_plane = i420.data + y_size + uv_size;
                uint8_t* uv_dst = nv12_frame.data + y_size;
                for (int i = 0; i < uv_size; ++i) {
                    uv_dst[i * 2] = u_plane[i];
                    uv_dst[i * 2 + 1] = v_plane[i];
                }
            }
            std::memcpy(gst_ctx.pool.data, nv12_frame.data, nv12_frame.total() * nv12_frame.elemSize());
            GstClockTime duration = gst_util_uint64_scale(GST_SECOND, 1, 30);
            GST_BUFFER_PTS(gst_ctx.gst_buf) = gst_ctx.frame_count * duration;
            GST_BUFFER_DTS(gst_ctx.gst_buf) = gst_ctx.frame_count * duration;
            GST_BUFFER_DURATION(gst_ctx.gst_buf) = duration;
            gst_ctx.frame_count++;
            gst_app_src_push_buffer(GST_APP_SRC(gst_ctx.appsrc),
                                     gst_buffer_ref(gst_ctx.gst_buf));
        }

        frame_count++;
        if (frame_count % 30 == 0) {
            auto t2 = std::chrono::steady_clock::now();
            int ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
            std::string enc = (use_mpp_encoder && use_h265_gst) ? "MPP H265" : "GStreamer H264";
            std::cout << "[推流] 第 " << frame_count << " 帧 编码耗时: " << ms << "ms 编码器=" << enc << std::endl;
        }

        auto next_frame_time = frame_start + std::chrono::milliseconds(33);
        auto now = std::chrono::steady_clock::now();
        if (next_frame_time > now) {
            std::this_thread::sleep_until(next_frame_time);
        }
    }

    if (use_mpp_encoder && use_h265_gst) {
        gstH265PushClose(h265_gst_ctx);
        mpp_encoder.release();
    } else if (use_gst_direct) {
        gstPushClose(gst_ctx);
    }
    g_realtime_composer = nullptr;
    std::cout << "[推流线程] 退出." << std::endl;
}
