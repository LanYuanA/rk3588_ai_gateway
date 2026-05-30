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
#include "stitcher.h"

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

} // namespace

void streamerThread() {
    std::cout << "[推流线程] 启动，初始化 4 路融合 + GStreamer MPP 硬件编码..." << std::endl;

    GstPushContext gst_ctx;
    bool use_gst_direct = gstPushOpen(gst_ctx);

    // 备用方案：OpenCV VideoWriter（BGR + videoconvert）
    cv::VideoWriter fallback_writer;
    if (!use_gst_direct) {
        std::cerr << "[推流线程-警告] GStreamer NV12 直推失败，回退到 BGR + videoconvert" << std::endl;
        std::string fallback_pipeline =
            "appsrc is-live=true block=false "
            "! videoconvert "
            "! video/x-raw,format=NV12,width=1280,height=960,framerate=30/1 "
            "! mpph264enc "
            "! h264parse config-interval=1 "
            "! rtspclientsink location=rtsp://127.0.0.1:8554/gateway_out protocols=tcp";
        fallback_writer.open(fallback_pipeline, cv::CAP_GSTREAMER, 0, 30.0, cv::Size(1280, 960), true);
        if (!fallback_writer.isOpened()) {
            std::cerr << "[推流线程-警告] 无法启动任何录制，尝试本地 AVI" << std::endl;
            fallback_writer.open("debug_4ch_output.avi", cv::VideoWriter::fourcc('M','J','P','G'),
                                 15.0, cv::Size(1280, 960), true);
        }
    }

    std::array<VideoFrame, NUM_STREAMS> latest_frames{};
    int frame_count = 0;
    bool rga_ok_last = false;
    auto last_frame_start = std::chrono::steady_clock::now();

    while (g_system_running) {
        auto frame_start = std::chrono::steady_clock::now();
        int interval_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            frame_start - last_frame_start).count();
        last_frame_start = frame_start;

        // 检测帧间隔抖动（>40ms 说明卡了）
        if (frame_count > 0 && interval_ms > 40) {
            std::cerr << "[卡顿检测] 帧间隔 " << interval_ms << "ms (期望≤33ms)" << std::endl;
        }

        updateCanvasFromPushQueues(latest_frames);

        bool has_valid_frame = false;
        for (const auto& frame : latest_frames) {
            if (frame.image && !frame.image->empty()) {
                has_valid_frame = true;
                break;
            }
        }

        if (!has_valid_frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        auto t1_merge = std::chrono::steady_clock::now();
        cv::Mat final_grid = composeGridWithDetections(latest_frames, 0);

        auto t2_merge = std::chrono::steady_clock::now();

        if (use_gst_direct) {
            // RGA 直接写入 pool buffer（零拷贝），或 CPU 回退写入 pool buffer
            rga_ok_last = convertGridBgrToNv12WithRga(
                final_grid, gst_ctx.pool.data, gst_ctx.pool.capacity);
            if (!rga_ok_last) {
                // CPU 回退：BGR→I420→NV12，直接写入 pool buffer
                cv::Mat i420;
                cv::cvtColor(final_grid, i420, cv::COLOR_BGR2YUV_I420);
                const int y_size = 1280 * 960;
                const int uv_size = y_size / 4;
                std::memcpy(gst_ctx.pool.data, i420.data, y_size);
                const uint8_t* u_plane = i420.data + y_size;
                const uint8_t* v_plane = i420.data + y_size + uv_size;
                guint8* uv_dst = gst_ctx.pool.data + y_size;
                for (int i = 0; i < uv_size; ++i) {
                    uv_dst[i * 2]     = u_plane[i];
                    uv_dst[i * 2 + 1] = v_plane[i];
                }
            }
            // 数据已在 pool buffer 中，只需更新 PTS 并推送
            GstClockTime duration = gst_util_uint64_scale(GST_SECOND, 1, 30);
            GST_BUFFER_PTS(gst_ctx.gst_buf) = gst_ctx.frame_count * duration;
            GST_BUFFER_DTS(gst_ctx.gst_buf) = gst_ctx.frame_count * duration;
            GST_BUFFER_DURATION(gst_ctx.gst_buf) = duration;
            gst_ctx.frame_count++;
            gst_app_src_push_buffer(GST_APP_SRC(gst_ctx.appsrc),
                                     gst_buffer_ref(gst_ctx.gst_buf));
        } else if (fallback_writer.isOpened()) {
            // 备用管道（BGR + videoconvert）
            fallback_writer.write(final_grid);
        }

        auto t3_write = std::chrono::steady_clock::now();

        frame_count++;
        if (frame_count % 30 == 0) {
            int merge_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2_merge - t1_merge).count();
            int write_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t3_write - t2_merge).count();
            std::cout << "[四路流-推流线程] 已推送第 " << frame_count
                      << " 帧。 拼图+RGA转NV12耗时: " << merge_ms
                      << " ms，编码推流耗时: " << write_ms
                      << " ms，RGA=" << (rga_ok_last ? "成功" : "回退CPU")
                      << "，管道=" << (use_gst_direct ? "GStreamer直推NV12" : "OpenCV BGR") << std::endl;
        }

        // 从帧开始算起，确保帧间隔稳定在 33ms（30fps）
        // 当前帧开始于 frame_start，下一帧应开始于 frame_start + 33ms
        auto next_frame_time = frame_start + std::chrono::milliseconds(33);
        auto now = std::chrono::steady_clock::now();
        if (next_frame_time > now) {
            std::this_thread::sleep_until(next_frame_time);
        }
    }

    if (use_gst_direct) {
        gstPushClose(gst_ctx);
    } else if (fallback_writer.isOpened()) {
        fallback_writer.release();
    }
    std::cout << "[推流线程] 退出并安全释放硬件写入句柄." << std::endl;
}
