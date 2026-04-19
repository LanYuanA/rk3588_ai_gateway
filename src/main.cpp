#include "common.h"
#include "ThreadSafeQueue.h"
#include "rknn_detector.h"
#include <mutex>
#include <signal.h>

// 增加 RGA 硬件加速接口头文件
#include "im2d_version.h"
#include "im2d_type.h"
#include "im2d_single.h"
#include "im2d_common.h"
#include "im2d_buffer.h"
#include "rga.h"




std::atomic<bool> g_system_running(true);

const int NUM_STREAMS = 4; 

std::vector<DetectResult> g_latest_results[NUM_STREAMS];
std::mutex g_results_mutex[NUM_STREAMS];
// 定义前端拉流缓冲队列
ThreadSafeQueue<VideoFrame> g_inference_queues[NUM_STREAMS]; 
// 为四路输入独立设立 4 条专用的后端推流缓冲队列，彻底杜绝多路互挤导致掉帧！
ThreadSafeQueue<VideoFrame> g_push_queues[NUM_STREAMS];

// 真实的拉流与解码线程 (生产者 - Aquí 我们先使用 OpenCV 替代 MPP 来拉流获得真实像素)
void streamPullerAndDecoderThread(int streamId, const std::string& stream_url) {
    std::cout << "[拉流/解码线程 " << streamId << "] 准备拉取媒体源: " << stream_url << std::endl;
    
    // 猛药 1：对于物理摄像头节点，强制要求 OpenCV 抛弃 GStreamer，直接使用最底层的 V4L2 硬件驱动！
    cv::VideoCapture cap;
    if (stream_url.find("/dev/video") != std::string::npos) {
        cap.open(stream_url, cv::CAP_V4L2);
        // 关键修复：强行限制硬件驱动堆积的残影数量，大幅降低 USB 摄像头延迟
        cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
        // 强行统一分辨率为最稳定的监控比例尺寸以防画面变绿或破碎
        cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    } 
    // 猛药 2：回退到稳定且支持自动丢弃音频防死锁的 FFmpeg 后端，结合我们刚写的 RGA 硬件来全自动缩放！
    else if (stream_url.find("rtsp://") != std::string::npos) {
        // [极重要修复]：RK3588 上的系统级 FFmpeg 默认会尝试调用 h264_v4l2m2m 进行硬件解码，但这套驱动节点在多流时往往发生抢占、死锁、黑屏。
        // 我们强制加入 video_codec;h264 来让 FFmpeg 走最可靠的纯 CPU 软解（避开驱动 bug），之后把它吐出来的 BGR 画面喂给 RGA 利用其算力做极限硬缩放！
        setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp|stimeout;2000000|flags;low_delay|video_codec;h264", 1);
        cap.open(stream_url, cv::CAP_FFMPEG);
    } 
    else {
        // 本地 MP4 文件也走 GStreamer 管道 
        std::string gst_pipeline = "uridecodebin uri=file:///home/kylin/kylin/process/" + stream_url + " ! videoconvert ! appsink sync=false max-buffers=5";
        cap.open(gst_pipeline, cv::CAP_GSTREAMER);
        if (!cap.isOpened()) {
            std::cout << "[警告] 启用 GStreamer 加速播放文件失败，回退到普通读取..." << std::endl;
            cap.open(stream_url); // 兜底
        }
    }
    
    if (!cap.isOpened()) {
        std::cerr << "[错误] 无法打开媒体源 " << streamId << ": " << stream_url << std::endl;
        return; // 在真实的商业工程这里会是一直无限重连
    }

    uint64_t frame_seq = 0;
    cv::Mat bgr_frame;
    auto last_read_time = std::chrono::steady_clock::now();

    while (g_system_running) {
        bool ret = cap.read(bgr_frame);
        if (!ret || bgr_frame.empty()) {
            std::cerr << "[警告] 流 " << streamId << " 读取失败或到达末尾！正在尝试重连..." << std::endl;
            // 对于非网络流，我们可以尝试回到开头。如果是网络流，必须进行底层 release 重新 open
            if (stream_url.find("rtsp://") != std::string::npos) {
                cap.release();
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                cap.open(stream_url, cv::CAP_FFMPEG);
            } else {
                cap.set(cv::CAP_PROP_POS_FRAMES, 0); 
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_read_time).count();
        
        // 关键修复：网络流或者物理摄像头属于【真实实时流】，它们自带时间戳和自然的帧率间隔，绝对不能在拉流线程中进行人为休眠！
        // 如果人为休眠 33ms，当网络发生微小抖动（连续到达两帧）时，休眠会导致错过读取窗口，触发 appsink drop=true，大量丢帧使得画面卡死（基本不变）。
        // 仅对本地 MP4（读文件速度极快）使用强制限制。
        bool is_live_stream = (stream_url.find("rtsp://") != std::string::npos || stream_url.find("/dev/") != std::string::npos);
        if (!is_live_stream && elapsed < 32) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33 - elapsed));
        }
        last_read_time = std::chrono::steady_clock::now();

        // 防御性设计：使用 RGA 硬件进行兜底缩放
        // 正常情况下 GStreamer rgaconvert 已经做完硬件缩放，这里将完全不耗 CPU 并在 0 毫秒跳过
        if (bgr_frame.cols != 640 || bgr_frame.rows != 480) {
            cv::Mat resized(480, 640, CV_8UC3);
            // 将 OpenCV 的 virtual buffer 动态送给 RGA（基于刚才跑通的 RK_FORMAT_BGR_888）
            rga_buffer_t src_img = wrapbuffer_virtualaddr(bgr_frame.data, bgr_frame.cols, bgr_frame.rows, RK_FORMAT_BGR_888);
            rga_buffer_t dst_img = wrapbuffer_virtualaddr(resized.data, 640, 480, RK_FORMAT_BGR_888);
            // 执行硬件级缩放
            IM_STATUS ret = imresize(src_img, dst_img);
            
            if (ret == IM_STATUS_SUCCESS) {
                bgr_frame = resized;
            } else {
                // 如果出现页表对齐异常等问题，退火到 CPU 缩放
                cv::resize(bgr_frame, bgr_frame, cv::Size(640, 480));
            }
        }

        VideoFrame frame;
        frame.stream_id = streamId;
        frame.frame_id = frame_seq++;
        frame.image = bgr_frame.clone(); // 深拷贝存入缓存区，以防内存覆写
        
        // 将解码后的帧推入对应流的缓冲队列(给 NPU 使用)
        g_inference_queues[streamId].push(frame);
        
        // 核心解耦：各自放入自己专属的推流队列中，不与别的流抢座位！
        g_push_queues[streamId].push(frame);
    }
    cap.release();
    std::cout << "[拉流/解码线程 " << streamId << "] 退出." << std::endl;
}

// 模拟：推理线程 (RKNN NPU) (消费者)
void inferenceThread(const std::string& model_path, std::vector<int> handled_streams, int npu_thread_id) {
    if (handled_streams.empty()) return;
    
    std::cout << "[推理线程 NPU " << npu_thread_id << "] 启动，接管流: ";
    for (int s : handled_streams) std::cout << s << " ";
    std::cout << "准备加载模型..." << std::endl;
    
    RKNNDetector detector;
    if (!detector.init(model_path)) {
        std::cerr << "[错误] NPU " << npu_thread_id << " 模型初始化失败。" << std::endl;
        return;
    }

    int idx = 0;
    while (g_system_running) {
        int current_stream_id = handled_streams[idx];
        VideoFrame frame;
        
        if (g_inference_queues[current_stream_id].size() > 0) {
            while (g_inference_queues[current_stream_id].size() > 1) {
                VideoFrame dummy;
                g_inference_queues[current_stream_id].pop(dummy);
            }

            if (g_inference_queues[current_stream_id].pop(frame)) {
                std::vector<DetectResult> results = detector.inference(frame.image);
                
                {
                    std::lock_guard<std::mutex> lock(g_results_mutex[current_stream_id]);
                    g_latest_results[current_stream_id] = results;
                }
            }
        }
        
        idx = (idx + 1) % handled_streams.size();
        if (idx == 0) {
           std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::cout << "[推理线程 NPU " << npu_thread_id << "] 退出." << std::endl;
}

// 模拟：推流线程 (以 RK3588 MPP 硬件编码进行流媒体输出或硬件录制)
void streamerThread() {
    std::cout << "[推流线程] 启动，初始化 4 路融合 + GStreamer MPP 硬件编码..." << std::endl;
    
    // 【网络流媒体核心】：GStreamer Push Pipelines 字典 (直接将 OpenCV BGR 抛给 mpph264enc，利用其内部调用的 rga_api 实现硬转！)
    // 1. 测试验证：录制到本地 MP4
    std::string gst_mp4 = "appsrc ! video/x-raw,format=BGR ! mpph264enc ! h264parse ! mp4mux fragment-duration=1000 ! filesink location=gateway_4ch_output.mp4";
    
    // 2. 目标：推流到 RTMP (接直播云或 NGINX-RTMP/SRS)
    // std::string gst_rtmp = "appsrc ! video/x-raw,format=BGR ! mpph264enc ! h264parse ! flvmux ! rtmpsink location=rtmp://127.0.0.1/live/ai_nvr";
    
    // 3. 目标：主动推送 RTSP 到流媒体分发服务器 (例如 MediaMTX 或者 ZLMediaKit)
    // std::string gst_rtsp = "appsrc ! video/x-raw,format=BGR ! mpph264enc ! h264parse ! rtspclientsink location=rtsp://127.0.0.1:8554/live protocols=tcp";
    
    // 4. 目标：对接公安部国标 GB28181 平台 (PS流封包通过 UDP 打给国标平台如 WVP/ZLMediaKit，剩下的 SIP 信令通过 C++ 发起)
    // std::string gst_gb = "appsrc ! video/x-raw,format=BGR ! mpph264enc ! h264parse ! mpegtsmux ! rtpmp2tpay ! udpsink host=10.0.0.1 port=10000";

    // ================================= 主动推送 RTSP 到 MediaMTX =================================
    // 使用 mpph264enc 硬件编码，将 4宫格画面主动推送到本机的 MediaMTX 服务端！
    // 主机端只要使用 VLC 播放器或 OBS 拉取 rtsp://<板子IP>:8554/gateway_out 即可看到流畅拼接画面！
    // （此处的 videoconvert 只负责格式匹配转换，主体编码完全由 mpph264enc 硬件接管处理）
    std::string push_rtsp = "appsrc ! video/x-raw,format=BGR,width=1280,height=960,framerate=30/1 ! videoconvert ! mpph264enc ! h264parse ! rtspclientsink location=rtsp://127.0.0.1:8554/gateway_out protocols=tcp";
    
    // 打开推流管道：这里分辨率从单路 640x480 改为了 2x2 拼图后的四路分辨率 1280x960
    cv::VideoWriter writer(push_rtsp, cv::CAP_GSTREAMER, 0, 30.0, cv::Size(1280, 960), true);
    
    if (!writer.isOpened()) {
        std::cerr << "[推流线程-警告] 无法启动基础录制，尝试退火到无硬件加速的 OpenCV RAW 写入！" << std::endl;
        writer.open("debug_4ch_output.avi", cv::VideoWriter::fourcc('M','J','P','G'), 15.0, cv::Size(1280, 960), true);
        if (!writer.isOpened()) {
            std::cerr << "[推流线程-警告] 无法启动任何本地录制！" << std::endl;
        }
    }

    VideoFrame frame;
    // 维护一个 4 宫格缓冲画布，初始化存放 0.1 秒的背景黑屏
    std::vector<cv::Mat> canvas(NUM_STREAMS, cv::Mat::zeros(480, 640, CV_8UC3));
    int frame_count = 0;

    auto last_write_time = std::chrono::steady_clock::now();

    while (g_system_running) {
        VideoFrame frame;
        // 先尽可能把当前时刻所有已经弹出的队列画面更新到画板中
        // 修改为 for 循环从 4 条独立轨道依次提取最新画面！
        for (int i = 0; i < NUM_STREAMS; ++i) {
            // 防积压：抽干除了最后1张以外所有的过期废片
            while (g_push_queues[i].size() > 1) {
                VideoFrame dummy;
                g_push_queues[i].pop(dummy);
            }
            if (g_push_queues[i].size() > 0 && g_push_queues[i].pop(frame)) {
                canvas[i] = frame.image;
            }
        }

        // 以绝对的真实世界时钟来触发合并与编码（保证绝对同步的 30FPS，不快也不慢）
        auto now = std::chrono::steady_clock::now();
        auto ms_passed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_write_time).count();
        
        // 30FPS 大约是 33 毫秒写一次
        if (ms_passed >= 33) {
            if (writer.isOpened()) {
                auto t1_merge = std::chrono::steady_clock::now();
                
                // 零拷贝/低显存操作优化：预先创建 1280x960 巨大画布底布
                cv::Mat final_grid(960, 1280, CV_8UC3, cv::Scalar(0, 0, 0));
                cv::Rect rois[4] = {
                    cv::Rect(0, 0, 640, 480),
                    cv::Rect(640, 0, 640, 480),
                    cv::Rect(0, 480, 640, 480),
                    cv::Rect(640, 480, 640, 480)
                };

                for (int i = 0; i < NUM_STREAMS; ++i) {
                    if (!canvas[i].empty()) {
                        // 1. 将底图直接拷贝进入最终画布对应的格子内 (极快的内存小范围写入，无需全量再建 hconcat/vconcat 图)
                        cv::Mat roi_mat = final_grid(rois[i]);
                        canvas[i].copyTo(roi_mat);
                        
                        // 2. 提取 NPU 最新出炉的结果
                        std::vector<DetectResult> current_res;
                        {
                            std::lock_guard<std::mutex> lock(g_results_mutex[i]);
                            current_res = g_latest_results[i]; // 立刻提取最新的结果
                        }
                        
                        // 3. 将框“就地”画在这份大图中的目标网格上，不污染原画布，省去所有不必要的 clone 操作！
                        for (const auto& res : current_res) {
                            cv::rectangle(roi_mat, res.box, cv::Scalar(0, 255, 0), 2);
                            std::string label = "Face: " + std::to_string(res.confidence).substr(0, 4);
                            cv::putText(roi_mat, label, cv::Point(res.box.x, res.box.y - 5), 
                                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);
                        }
                    }
                }
                
                auto t2_merge = std::chrono::steady_clock::now();
                writer.write(final_grid);
                auto t3_write = std::chrono::steady_clock::now();

                frame_count++;
                if (frame_count % 30 == 0) {
                    int merge_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2_merge - t1_merge).count();
                    int write_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t3_write - t2_merge).count();
                    std::cout << "[四路流-推流线程] 已同步推送第 " << frame_count 
                              << " 帧。 前期拼图耗时: " << merge_ms 
                              << " ms，GStreamer编码写入耗时: " << write_ms << " ms" << std::endl;
                }
            }
            // 核心修复：把下一次的基准时间增加 33 毫秒，而不是单纯的等于 = now
            // 这样能补偿因为拼图或编码花费的额外时间，保证严格的宏观 30FPS（丢帧折叠补偿）
            last_write_time += std::chrono::milliseconds(33); 
            
            // 如果堆积太严重（系统卡死了），防止时钟被过度透支导致未来疯狂快进
            if (std::chrono::steady_clock::now() - last_write_time > std::chrono::milliseconds(100)) {
                last_write_time = std::chrono::steady_clock::now();
            }
        } else {
            // 防空转
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    
    if (writer.isOpened()) writer.release();
    std::cout << "[推流线程] 退出并安全释放硬件写入句柄." << std::endl;
}

int main() {
    // 屏蔽极其恶心的 SIGPIPE 信号 (Exit Code 141 断网自动闪退)。
    // Linux 在写入一个已经被服务器关闭的 Socket 时会抛出 SIGPIPE 直接杀死整个 C++ 进程，这对跑流媒体致命。
    signal(SIGPIPE, SIG_IGN); 

    std::cout << "========== RK3588 AI Gateway 初始化 ==========\n";

    // 自定义 4 路媒体源 (这里兼容 USB 相机 /dev/videoX，RTSP 流地址，或者本地 MP4)
    std::vector<std::string> stream_sources = {
         "/dev/video74", // 第一路：刚刚插入的真实 USB 摄像头 (暂时注释掉排查)
        // 在 RK3588 上自己拉取本地 MediaMTX 转换好送过来的流
        "rtsp://127.0.0.1:8554/host_cam",   // 第二路：你的电脑主机的网络摄像头流
        // "/dev/video83",
        "test.mp4",     // 第三路：保持读取本地测试视频...
        "test.mp4"      // 第四路：保持读取本地测试视频...
    };

    std::vector<std::thread> pullers;

    // 1. 启动各路拉流与解码线程 (生产者)
    for (int i = 0; i < NUM_STREAMS; ++i) {
        pullers.emplace_back(streamPullerAndDecoderThread, i, stream_sources[i]);
    }

    // 2. 启动核心 AI 推理线程 (消费者)，并传入真实 RKNN 离线模型路径
    std::string model_path = "/home/kylin/kylin/process/yolov8_face_fp.rknn"; // 修改为您真实的模型名称
    
    // 使用独立的多核 NPU 线程！上半部分给 NPU 0 跑，下半部分给 NPU 1 跑！
    std::thread infer1(inferenceThread, model_path, std::vector<int>{0, 1}, 1);
    std::thread infer2(inferenceThread, model_path, std::vector<int>{2, 3}, 2);

    // 3. 启动拼接与推流线程
    std::thread streamer(streamerThread);

    std::cout << "所有子线程已启动，按 Enter 键安全退出系统...\n";
    std::cin.get();
    
    // 优雅退出机制：避免直接 terminate 导致硬件资源死锁
    std::cout << "正在通知所有线程退出...\n";
    g_system_running = false;
    
    // 唤醒所有可能在阻塞等待的队列
    for (int i = 0; i < NUM_STREAMS; ++i) {
        g_inference_queues[i].wake_up_all();
    }

    for (auto& t : pullers) { if(t.joinable()) t.join(); }
    if (infer1.joinable()) infer1.join();
    if (infer2.joinable()) infer2.join();
    if (streamer.joinable()) streamer.join();

    std::cout << "系统安全关闭. Bye!\n";
    return 0;
}
