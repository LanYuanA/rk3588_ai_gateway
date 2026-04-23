# MPP硬件编码详细机制解析

## 1. MPP框架概述

MPP (Media Process Platform) 是Rockchip系列芯片的多媒体处理平台，提供统一的硬件加速接口，支持视频编解码、图像处理等功能。MPP框架将底层硬件抽象为统一的服务接口，使应用程序可以通过标准API调用硬件加速功能。

## 2. RKVENC2硬件编码器架构

### 2.1 核心结构体

#### rkvenc_dev 结构体
```c
struct rkvenc_dev {
    struct mpp_dev mpp;                    // MPP设备基类
    struct rkvenc_hw_info *hw_info;       // 硬件信息
    struct mpp_clock_info aclk_info;      // AXI时钟信息
    struct mpp_clock_info hclk_info;      // AHB时钟信息
    struct mpp_clock_info core_clk_info;  // 核心时钟信息
    struct clk *aclk;                     // AXI时钟指针
    struct clk *hclk;                     // AHB时钟指针
    struct clk *core_clk;                 // 核心时钟指针
    struct rkvenc_ccu *ccu;               // CCU控制器指针
    struct list_head core_link;           // 核心链表节点
    struct page *rcb_page;                // RCB页面指针
    u32 sram_size;                        // SRAM大小
    u32 sram_used;                        // 使用的SRAM大小
    u32 sram_iova;                        // SRAM IOVA地址
    int sram_enabled;                     // SRAM使能状态
};
```

#### rkvenc_task 结构体
```c
struct rkvenc_task {
    struct mpp_task base;                 // MPP任务基类
    struct mpp_frame *frame;              // 输入帧
    struct mpp_packet *packet;            // 输出包
    struct mpp_meta *meta;                // 元数据
    struct mpp_buf_slot *input;           // 输入槽位
    struct mpp_buf_slot *output;          // 输出槽位
    struct mpp_meta meta_in;              // 输入元数据
    struct mpp_meta meta_out;             // 输出元数据
    struct rkvenc_enc_cfg cfg;            // 编码配置
    struct rkvenc_roi_cfg roi_cfg;        // ROI配置
    struct rkvenc_osd_cfg osd_cfg;        // OSD配置
    struct rkvenc_hdr_cfg hdr_cfg;        // HDR配置
    struct rkvenc_user_data_cfg ud_cfg;   // 用户数据配置
    struct rkvenc_user_data_info ud_info; // 用户数据信息
    struct rkvenc_user_data_info ud_info_backup; // 用户数据备份
    struct rkvenc_user_data_info ud_info_cur;    // 当前用户数据
    struct rkvenc_user_data_info ud_info_next;   // 下一个用户数据
    struct rkvenc_user_data_info ud_info_prev;   // 上一个用户数据
    struct rkvenc_user_data_info ud_info_temp;   // 临时用户数据
    struct rkvenc_user_data_info ud_info_pool[MPP_ARRAY_ELEMS(ud_info_pool)]; // 用户数据池
    struct kfifo slice_info;              // 切片信息FIFO
    u32 slice_rd_cnt;                     // 切片读取计数
    u32 slice_wr_cnt;                     // 切片写入计数
    u32 task_split;                       // 任务切分标志
    u32 task_split_done;                  // 任务切分完成标志
    u32 frame_type;                       // 帧类型
    u32 qp;                              // 量化参数
    u32 bitrate;                         // 比特率
    u32 fps;                             // 帧率
    u32 gop;                             // GOP大小
    u32 width;                           // 宽度
    u32 height;                          // 高度
    u32 format;                          // 格式
    u32 profile;                         // 配置文件
    u32 level;                           // 级别
    u32 rc_mode;                         // 码率控制模式
    u32 quality;                         // 质量
    u32 priority;                        // 优先级
    u32 timeout;                         // 超时时间
    u32 clk_mode;                        // 时钟模式
    u32 reserved[8];                     // 保留字段
};
```

### 2.2 设备操作结构体

#### mpp_dev_ops 结构体
```c
static struct mpp_dev_ops rkvenc_dev_ops_v2 = {
    .wait_result = rkvenc2_wait_result,    // 等待结果
    .alloc_task = rkvenc_alloc_task,       // 分配任务
    .run = rkvenc_run,                     // 运行任务
    .irq = rkvenc_irq,                     // 中断处理
    .isr = rkvenc_isr,                     // 中断服务例程
    .finish = rkvenc_finish,               // 完成任务
    .result = rkvenc_result,               // 处理结果
    .free_task = rkvenc_free_task,         // 释放任务
    .ioctl = rkvenc_control,               // 控制命令
    .init_session = rkvenc_init_session,   // 初始化会话
    .free_session = rkvenc_free_session,   // 释放会话
    .dump_session = rkvenc_dump_session,   // 转储会话
};
```

## 3. 硬件编码流程

### 3.1 设备初始化流程

#### rkvenc_probe() - 设备探测与初始化
```c
static int rkvenc_probe(struct platform_device *pdev)
{
    int ret = 0;
    struct device *dev = &pdev->dev;
    struct device_node *np = dev->of_node;

    dev_info(dev, "probing start\n");

    if (strstr(np->name, "ccu"))
        ret = rkvenc_ccu_probe(pdev);      // CCU控制器初始化
    else if (strstr(np->name, "core"))
        ret = rkvenc_core_probe(pdev);     // 核心初始化
    else
        ret = rkvenc_probe_default(pdev);  // 默认初始化

    dev_info(dev, "probing finish\n");

    return ret;
}
```

#### rkvenc_core_probe() - 核心设备初始化
```c
static int rkvenc_core_probe(struct platform_device *pdev)
{
    int ret = 0;
    struct device *dev = &pdev->dev;
    struct rkvenc_dev *enc = NULL;
    struct mpp_dev *mpp = NULL;

    // 1. 分配设备内存
    enc = devm_kzalloc(dev, sizeof(*enc), GFP_KERNEL);
    if (!enc)
        return -ENOMEM;

    mpp = &enc->mpp;
    platform_set_drvdata(pdev, mpp);

    // 2. 解析设备树信息
    if (pdev->dev.of_node) {
        struct device_node *np = pdev->dev.of_node;
        const struct of_device_id *match = NULL;

        match = of_match_node(mpp_rkvenc_dt_match, np);
        if (match)
            mpp->var = (struct mpp_dev_var *)match->data;

        mpp->core_id = of_alias_get_id(np, "rkvenc");
    }

    // 3. MPP设备探测
    ret = mpp_dev_probe(mpp, pdev);
    if (ret)
        return ret;

    // 4. 附加到CCU
    ret = rkvenc_attach_ccu(dev, enc);
    if (ret) {
        dev_err(dev, "attach ccu failed\n");
        return ret;
    }

    // 5. 分配RCB缓冲区
    rkvenc2_alloc_rcbbuf(pdev, enc);

    // 6. 注册中断处理程序
    ret = devm_request_threaded_irq(dev, mpp->irq,
                    mpp_dev_irq,
                    NULL,
                    IRQF_ONESHOT,
                    dev_name(dev), mpp);
    if (ret) {
        dev_err(dev, "register interrupter runtime failed\n");
        return -EINVAL;
    }

    // 7. 设置会话最大缓冲区数量
    mpp->session_max_buffers = RKVENC_SESSION_MAX_BUFFERS;
    enc->hw_info = to_rkvenc_info(mpp->var->hw_info);
    mpp->fault_handler = rkvenc2_iommu_fault_handle;
    rkvenc_procfs_init(mpp);
    rkvenc_procfs_ccu_init(mpp);

    // 8. 如果是主核心，则注册到MPP服务
    if (mpp == enc->ccu->main_core)
        mpp_dev_register_srv(mpp, mpp->srv);

    return 0;
}
```

### 3.2 任务分配与配置流程

#### rkvenc_alloc_task() - 任务分配
```c
static int rkvenc_alloc_task(struct mpp_task *mpp_task, struct mpp_session *session)
{
    struct rkvenc_task *task = NULL;
    struct mpp_frame *frame = NULL;
    struct mpp_packet *packet = NULL;
    struct mpp_meta *meta = NULL;
    int ret = 0;

    // 1. 分配任务结构体
    task = kzalloc(sizeof(*task), GFP_KERNEL);
    if (!task)
        return -ENOMEM;

    // 2. 初始化任务基础信息
    mpp_task_init(&task->base, session, MPP_CTX_ENC, MPP_ENC_TASK_NUM);

    // 3. 获取输入帧和输出包
    ret = mpp_task_meta_get_frame(mpp_task, KEY_INPUT_FRAME, &frame);
    if (ret) {
        mpp_err("failed to get input frame\n");
        goto ERR_RET;
    }

    ret = mpp_task_meta_get_packet(mpp_task, KEY_OUTPUT_PACKET, &packet);
    if (ret) {
        mpp_err("failed to get output packet\n");
        goto ERR_RET;
    }

    // 4. 获取元数据
    meta = mpp_task_get_meta(mpp_task);

    // 5. 初始化任务参数
    task->frame = frame;
    task->packet = packet;
    task->meta = meta;

    // 6. 解析编码参数
    rkvenc_parse_enc_params(task, meta);

    // 7. 初始化切片信息FIFO
    kfifo_init(&task->slice_info, task->slice_info_pool, 
               sizeof(task->slice_info_pool));

    // 8. 设置任务完成回调
    mpp_task_set_complete_cb(mpp_task, rkvenc_task_complete);

    return 0;

ERR_RET:
    kfree(task);
    return ret;
}
```

### 3.3 硬件编码执行流程

#### rkvenc_run() - 编码任务执行
```c
static int rkvenc_run(struct mpp_dev *mpp, struct mpp_task *mpp_task)
{
    struct rkvenc_dev *enc = to_rkvenc_dev(mpp);
    struct rkvenc_task *task = to_rkvenc_task(mpp_task);
    struct mpp_frame *frame = task->frame;
    struct mpp_packet *packet = task->packet;
    struct mpp_meta *meta = task->meta;
    u32 reg_base = 0;
    u32 reg_val = 0;
    int ret = 0;

    // 1. 时钟使能
    ret = rkvenc_clk_on(mpp);
    if (ret) {
        mpp_err("failed to enable clock\n");
        return ret;
    }

    // 2. 设置时钟频率
    rkvenc_set_freq(mpp, task);

    // 3. 配置硬件寄存器
    // 3.1 配置输入缓冲区地址
    mpp_write(mpp, RKVENC_INPUT_ADDR_L, 
              mpp_frame_get_buf_fd(frame, 0) & 0xFFFFFFFF);
    mpp_write(mpp, RKVENC_INPUT_ADDR_H, 
              (mpp_frame_get_buf_fd(frame, 0) >> 32) & 0xFF);

    // 3.2 配置输出缓冲区地址
    mpp_write(mpp, RKVENC_OUTPUT_ADDR_L, 
              mpp_packet_get_buf_fd(packet) & 0xFFFFFFFF);
    mpp_write(mpp, RKVENC_OUTPUT_ADDR_H, 
              (mpp_packet_get_buf_fd(packet) >> 32) & 0xFF);

    // 3.3 配置图像尺寸
    reg_val = (task->height << 16) | task->width;
    mpp_write(mpp, RKVENC_PIC_SIZE, reg_val);

    // 3.4 配置编码参数
    rkvenc_setup_encode_params(enc, task);

    // 3.5 配置码率控制
    rkvenc_setup_rate_control(enc, task);

    // 3.6 配置ROI区域
    rkvenc_setup_roi_params(enc, task);

    // 4. 启动硬件编码
    reg_val = mpp_read(mpp, RKVENC_CTRL);
    reg_val |= RKVENC_CTRL_START;
    mpp_write(mpp, RKVENC_CTRL, reg_val);

    // 5. 启用中断
    reg_val = mpp_read(mpp, RKVENC_INT_EN);
    reg_val |= RKVENC_INT_FRAME_DONE;
    mpp_write(mpp, RKVENC_INT_EN, reg_val);

    return 0;
}
```

### 3.4 中断处理流程

#### rkvenc_isr() - 中断服务例程
```c
static irqreturn_t rkvenc_isr(int irq, void *dev_id)
{
    struct mpp_dev *mpp = (struct mpp_dev *)dev_id;
    struct rkvenc_dev *enc = to_rkvenc_dev(mpp);
    u32 int_status = 0;
    u32 int_clear = 0;

    // 1. 读取中断状态
    int_status = mpp_read(mpp, RKVENC_INT_STATUS);

    // 2. 检查中断类型
    if (int_status & RKVENC_INT_FRAME_DONE) {
        // 帧完成中断
        int_clear |= RKVENC_INT_FRAME_DONE;
        
        // 通知上层完成
        mpp_dev_complete(mpp);
    }
    else if (int_status & RKVENC_INT_ERROR) {
        // 错误中断
        int_clear |= RKVENC_INT_ERROR;
        
        mpp_err("rkvenc hardware error occurred\n");
        mpp_dev_error(mpp);
    }
    else if (int_status & RKVENC_INT_TIMEOUT) {
        // 超时中断
        int_clear |= RKVENC_INT_TIMEOUT;
        
        mpp_err("rkvenc hardware timeout occurred\n");
        mpp_dev_timeout(mpp);
    }

    // 3. 清除中断状态
    mpp_write(mpp, RKVENC_INT_STATUS, int_clear);

    return IRQ_HANDLED;
}
```

### 3.5 结果处理流程

#### rkvenc_result() - 结果处理
```c
static int rkvenc_result(struct mpp_dev *mpp, struct mpp_task *mpp_task, void *result)
{
    struct rkvenc_task *task = to_rkvenc_task(mpp_task);
    struct mpp_frame *frame = task->frame;
    struct mpp_packet *packet = task->packet;
    struct mpp_meta *meta = task->meta;
    u32 encoded_size = 0;
    u32 frame_type = 0;
    int ret = 0;

    // 1. 读取编码结果
    encoded_size = mpp_read(mpp, RKVENC_BS_SIZE);
    frame_type = mpp_read(mpp, RKVENC_FRAME_TYPE);

    // 2. 更新输出包信息
    mpp_packet_set_length(packet, encoded_size);
    mpp_packet_set_pos(packet, encoded_size);

    // 3. 设置帧类型元数据
    mpp_meta_set_s32(meta, KEY_OUTPUT_FRAME_TYPE, frame_type);

    // 4. 设置时间戳
    mpp_meta_set_s64(meta, KEY_OUTPUT_TIME_ORDER, 
                     mpp_frame_get_pts(frame));
    mpp_meta_set_s64(meta, KEY_OUTPUT_TIME_DISP, 
                     mpp_frame_get_dts(frame));

    // 5. 更新统计信息
    mpp_meta_set_s32(meta, KEY_OUTPUT_BITRATE, task->bitrate);
    mpp_meta_set_s32(meta, KEY_OUTPUT_QP, task->qp);

    // 6. 时钟关闭
    rkvenc_clk_off(mpp);

    return 0;
}
```

### 3.6 等待结果流程

#### rkvenc2_wait_result() - 等待编码结果
```c
static int rkvenc2_wait_result(struct mpp_session *session,
                              struct mpp_task_msgs *msgs)
{
    struct rkvenc_poll_slice_cfg cfg;
    struct rkvenc_task *enc_task;
    struct mpp_request *req;
    struct mpp_task *task;
    struct mpp_dev *mpp;
    union rkvenc2_slice_len_info slice_info;
    u32 task_id;
    int ret = 0;

    // 1. 从待处理列表获取任务
    mutex_lock(&session->pending_lock);
    task = list_first_entry_or_null(&session->pending_list,
                    struct mpp_task,
                    pending_link);
    mutex_unlock(&session->pending_lock);
    if (!task) {
        mpp_err("session %p pending list is empty!\n", session);
        return -EIO;
    }

    mpp = mpp_get_task_used_device(task, session);
    enc_task = to_rkvenc_task(task);
    task_id = task->task_id;

    req = cmpxchg(&msgs->poll_req, msgs->poll_req, NULL);

    // 2. 检查是否需要切片处理
    if (!enc_task->task_split || enc_task->task_split_done) {
task_done_ret:
        // 等待任务完成
        ret = wait_event_interruptible(task->wait, test_bit(TASK_STATE_DONE, &task->state));
        if (ret == -ERESTARTSYS)
            mpp_err("wait task break by signal in normal mode\n");

        return rkvenc2_task_default_process(mpp, task);
    }

    // 3. 处理切片返回
    if (!req) {
        do {
            // 等待切片信息可用
            ret = wait_event_interruptible(task->wait, kfifo_out(&enc_task->slice_info,
                                         &slice_info, 1));
            if (ret == -ERESTARTSYS) {
                mpp_err("wait task break by signal in slice all mode\n");
                return 0;
            }
            mpp_dbg_slice("task %d rd %3d len %d %s\n",
                    task_id, enc_task->slice_rd_cnt, slice_info.slice_len,
                    slice_info.last ? "last" : "");

            enc_task->slice_rd_cnt++;

            if (slice_info.last)
                goto task_done_ret;
        } while (1);
    }

    // 4. 处理切片轮询
    if (copy_from_user(&cfg, req->data, sizeof(cfg))) {
        mpp_err("copy_from_user failed\n");
        return -EINVAL;
    }

    mpp_dbg_slice("task %d poll irq %d:%d\n", task->task_id,
              cfg.count_max, cfg.count_ret);
    cfg.count_ret = 0;

    // 5. 处理单个切片轮询返回
    do {
        ret = wait_event_interruptible(task->wait, kfifo_out(&enc_task->slice_info,
                                     &slice_info, 1));
        if (ret == -ERESTARTSYS) {
            mpp_err("wait task break by signal in slice one mode\n");
            return 0;
        }
        mpp_dbg_slice("core %d task %d rd %3d len %d %s\n", task_id,
                mpp->core_id, enc_task->slice_rd_cnt, slice_info.slice_len,
                slice_info.last ? "last" : "");
        enc_task->slice_rd_cnt++;
        if (cfg.count_ret < cfg.count_max) {
            struct rkvenc_poll_slice_cfg __user *ucfg =
                (struct rkvenc_poll_slice_cfg __user *)(req->data);
            u32 __user *dst = (u32 __user *)(ucfg + 1);

            /* Do NOT return here when put_user error. Just continue */
            if (put_user(slice_info.val, dst + cfg.count_ret))
                ret = -EFAULT;

            cfg.count_ret++;
            if (put_user(cfg.count_ret, &ucfg->count_ret))
                ret = -EFAULT;
        }

        if (slice_info.last) {
            enc_task->task_split_done = 1;
            goto task_done_ret;
        }

        if (cfg.count_ret >= cfg.count_max)
            return 0;

        if (ret < 0)
            return ret;
    } while (!ret);

    rkvenc2_task_timeout_process(session, task);

    return ret;
}
```

## 4. 硬件寄存器配置

### 4.1 主要寄存器定义
```c
#define RKVENC_CTRL                 0x0000  // 控制寄存器
#define RKVENC_INT_EN               0x0004  // 中断使能寄存器
#define RKVENC_INT_STATUS           0x0008  // 中断状态寄存器
#define RKVENC_PIC_SIZE             0x000C  // 图像尺寸寄存器
#define RKVENC_INPUT_ADDR_L         0x0010  // 输入地址低32位
#define RKVENC_INPUT_ADDR_H         0x0014  // 输入地址高32位
#define RKVENC_OUTPUT_ADDR_L        0x0018  // 输出地址低32位
#define RKVENC_OUTPUT_ADDR_H        0x001C  // 输出地址高32位
#define RKVENC_BS_SIZE              0x0020  // 码流大小寄存器
#define RKVENC_FRAME_TYPE           0x0024  // 帧类型寄存器
#define RKVENC_RC_CFG               0x0028  // 码率控制配置
#define RKVENC_QP_CFG               0x002C  // QP配置寄存器
#define RKVENC_GOP_CFG              0x0030  // GOP配置寄存器
#define RKVENC_ROI_CFG              0x0034  // ROI配置寄存器
#define RKVENC_HDR_CFG              0x0038  // HDR配置寄存器
```

### 4.2 控制寄存器位定义
```c
#define RKVENC_CTRL_START           (1 << 0)   // 启动编码
#define RKVENC_CTRL_RESET           (1 << 1)   // 硬件复位
#define RKVENC_CTRL_FLUSH           (1 << 2)   // 刷新缓存
#define RKVENC_CTRL_INTERRUPT_EN    (1 << 3)   // 中断使能
#define RKVENC_CTRL_LOW_DELAY       (1 << 4)   // 低延迟模式
#define RKVENC_CTRL_HDR_EN          (1 << 5)   // HDR使能
```

### 4.3 中断类型定义
```c
#define RKVENC_INT_FRAME_DONE       (1 << 0)   // 帧完成中断
#define RKVENC_INT_ERROR            (1 << 1)   // 错误中断
#define RKVENC_INT_TIMEOUT          (1 << 2)   // 超时中断
#define RKVENC_INT_SLICE_DONE       (1 << 3)   // 切片完成中断
#define RKVENC_INT_BUF_EMPTY        (1 << 4)   // 缓冲区空中断
#define RKVENC_INT_BUF_FULL         (1 << 5)   // 缓冲区满中断
```

## 5. 内存管理机制

### 5.1 IOMMU内存映射
```c
static int rkvenc2_alloc_rcbbuf(struct platform_device *pdev, struct rkvenc_dev *enc)
{
    int ret;
    u32 vals[2];
    dma_addr_t iova;
    u32 sram_used, sram_size;
    struct device_node *sram_np;
    struct resource sram_res;
    resource_size_t sram_start, sram_end;
    struct iommu_domain *domain;
    struct device *dev = &pdev->dev;

    // 1. 获取RCB IOVA起始地址和大小
    ret = device_property_read_u32_array(dev, "rockchip,rcb-iova", vals, 2);
    if (ret)
        return ret;

    iova = PAGE_ALIGN(vals[0]);
    sram_used = PAGE_ALIGN(vals[1]);
    if (!sram_used) {
        dev_err(dev, "sram rcb invalid.\n");
        return -EINVAL;
    }

    // 2. 为RCB预留IOVA地址空间
    ret = iommu_dma_reserve_iova(dev, iova, sram_used);
    if (ret) {
        dev_err(dev, "alloc rcb iova error.\n");
        return ret;
    }

    // 3. 获取SRAM设备节点
    sram_np = of_parse_phandle(dev->of_node, "rockchip,sram", 0);
    if (!sram_np) {
        dev_err(dev, "could not find phandle sram\n");
        return -ENODEV;
    }

    // 4. 获取SRAM起始地址和大小
    ret = of_address_to_resource(sram_np, 0, &sram_res);
    of_node_put(sram_np);
    if (ret) {
        dev_err(dev, "find sram res error\n");
        return ret;
    }

    // 5. 检查SRAM地址和大小是否按页对齐
    sram_start = round_up(sram_res.start, PAGE_SIZE);
    sram_end = round_down(sram_res.start + resource_size(&sram_res), PAGE_SIZE);
    if (sram_end <= sram_start) {
        dev_err(dev, "no available sram, phy_start %pa, phy_end %pa\n",
            &sram_start, &sram_end);
        return -ENOMEM;
    }
    sram_size = sram_end - sram_start;
    sram_size = sram_used < sram_size ? sram_used : sram_size;

    // 6. IOVA映射到SRAM
    domain = enc->mpp.iommu_info->domain;
    ret = iommu_map(domain, iova, sram_start, sram_size, IOMMU_READ | IOMMU_WRITE);
    if (ret) {
        dev_err(dev, "sram iommu_map error.\n");
        return ret;
    }

    // 7. 为剩余缓冲区分配DMA内存(SRAM + DMA混合方案)
    if (sram_size < sram_used) {
        struct page *page;
        size_t page_size = PAGE_ALIGN(sram_used - sram_size);

        page = alloc_pages(GFP_KERNEL | __GFP_ZERO, get_order(page_size));
        if (!page) {
            dev_err(dev, "unable to allocate pages\n");
            ret = -ENOMEM;
            goto err_sram_map;
        }

        // 8. IOVA映射到DMA内存
        ret = iommu_map(domain, iova + sram_size, page_to_phys(page),
                page_size, IOMMU_READ | IOMMU_WRITE);
        if (ret) {
            dev_err(dev, "page iommu_map error.\n");
            __free_pages(page, get_order(page_size));
            goto err_sram_map;
        }
        enc->rcb_page = page;
    }

    enc->sram_size = sram_size;
    enc->sram_used = sram_used;
    enc->sram_iova = iova;
    enc->sram_enabled = -1;
    dev_info(dev, "sram_start %pa\n", &sram_start);
    dev_info(dev, "sram_iova %pad\n", &enc->sram_iova);
    dev_info(dev, "sram_size %u\n", enc->sram_size);
    dev_info(dev, "sram_used %u\n", enc->sram_used);

    return 0;

err_sram_map:
    iommu_unmap(domain, iova, sram_size);

    return ret;
}
```

## 6. 时钟管理机制

### 6.1 时钟使能
```c
static int rkvenc_clk_on(struct mpp_dev *mpp)
{
    struct rkvenc_dev *enc = to_rkvenc_dev(mpp);

    clk_prepare_enable(enc->aclk_info.clk);
    clk_prepare_enable(enc->hclk_info.clk);
    clk_prepare_enable(enc->core_clk_info.clk);

    return 0;
}
```

### 6.2 时钟关闭
```c
static int rkvenc_clk_off(struct mpp_dev *mpp)
{
    struct rkvenc_dev *enc = to_rkvenc_dev(mpp);

    clk_disable_unprepare(enc->aclk_info.clk);
    clk_disable_unprepare(enc->hclk_info.clk);
    clk_disable_unprepare(enc->core_clk_info.clk);

    return 0;
}
```

### 6.3 频率设置
```c
static int rkvenc_set_freq(struct mpp_dev *mpp, struct mpp_task *mpp_task)
{
    struct rkvenc_dev *enc = to_rkvenc_dev(mpp);
    struct rkvenc_task *task = to_rkvenc_task(mpp_task);

    mpp_clk_set_rate(&enc->aclk_info, task->clk_mode);
    mpp_clk_set_rate(&enc->core_clk_info, task->clk_mode);

    return 0;
}
```

## 7. CCU (Core Control Unit) 机制

### 7.1 CCU结构体
```c
struct rkvenc_ccu {
    struct mutex lock;              // 互斥锁
    struct list_head core_list;     // 核心列表
    spinlock_t lock_dchs;           // DCHS锁
    struct mpp_dev *main_core;      // 主核心
    u32 core_num;                   // 核心数量
};
```

### 7.2 CCU核心附加
```c
static int rkvenc_attach_ccu(struct device *dev, struct rkvenc_dev *enc)
{
    struct device_node *np;
    struct platform_device *pdev;
    struct rkvenc_ccu *ccu;

    mpp_debug_enter();

    // 1. 解析CCU设备节点
    np = of_parse_phandle(dev->of_node, "rockchip,ccu", 0);
    if (!np || !of_device_is_available(np))
        return -ENODEV;

    pdev = of_find_device_by_node(np);
    of_node_put(np);
    if (!pdev)
        return -ENODEV;

    ccu = platform_get_drvdata(pdev);
    if (!ccu)
        return -ENOMEM;

    // 2. 初始化核心链接
    INIT_LIST_HEAD(&enc->core_link);
    mutex_lock(&ccu->lock);
    ccu->core_num++;
    list_add_tail(&enc->core_link, &ccu->core_list);
    mutex_unlock(&ccu->lock);

    // 3. 附加CCU域到当前核心
    if (!ccu->main_core) {
        /**
         * 设置第一个设备为主核心，
         * 然后主核心的域命名为CCU域
         */
        ccu->main_core = &enc->mpp;
    } else {
        struct mpp_iommu_info *ccu_info, *cur_info;

        // 设置CCU域到当前设备
        ccu_info = ccu->main_core->iommu_info;
        cur_info = enc->mpp.iommu_info;

        if (cur_info) {
            cur_info->domain = ccu_info->domain;
            cur_info->rw_sem = ccu_info->rw_sem;
        }
        mpp_iommu_attach(cur_info);

        // 增加主核心消息容量
        ccu->main_core->msgs_cap++;
        enc->mpp.msgs_cap = 0;
    }
    enc->ccu = ccu;

    dev_info(dev, "attach ccu as core %d\n", enc->mpp.core_id);
    mpp_debug_enter();

    return 0;
}
```

## 8. 故障处理机制

### 8.1 IOMMU故障处理
```c
static int rkvenc2_iommu_fault_handle(struct iommu_domain *iommu,
                      struct device *iommu_dev,
                      unsigned long iova, int status, void *arg)
{
    struct mpp_dev *mpp = (struct mpp_dev *)arg;
    struct rkvenc_dev *enc = to_rkvenc_dev(mpp);
    struct mpp_task *mpp_task;
    struct rkvenc_ccu *ccu = enc->ccu;

    // 1. 查找对应的MPP设备
    if (ccu) {
        struct rkvenc_dev *core = NULL, *n;

        list_for_each_entry_safe(core, n, &ccu->core_list, core_link) {
            if (core->mpp.iommu_info &&
                (&core->mpp.iommu_info->pdev->dev == iommu_dev)) {
                mpp = &core->mpp;
                break;
            }
        }
    }
    mpp_task = mpp->cur_task;
    dev_info(mpp->dev, "core %d page fault found dchs %08x\n",
         mpp->core_id, mpp_read_relaxed(&enc->mpp, DCHS_REG_OFFSET));

    // 2. 转储内存区域信息
    if (mpp_task)
        mpp_task_dump_mem_region(mpp, mpp_task);

    /*
     * 屏蔽IOMMU中断，防止IOMMU重复触发页错误
     * 直到页错误任务通过硬件超时完成
     */
    rockchip_iommu_mask_irq(mpp->dev);

    return 0;
}
```

## 9. 面试要点总结

### 9.1 MPP整体架构
MPP (Media Process Platform) 是瑞芯微针对其SoC设计的统一媒体处理平台，主要包含以下几个部分：
- **硬件抽象层**: 将底层的视频编解码、图像处理等硬件单元抽象为统一接口
- **任务调度层**: 管理多个编码/解码任务的排队和执行
- **内存管理层**: 通过IOMMU实现高效的内存映射和管理
- **驱动接口层**: 提供用户空间的标准API接口

### 9.2 MPP硬件编码工作流程（面试版）

在面试时，您可以按照以下流程清晰地描述MPP硬件编码的工作过程：

1. **初始化阶段**:
   - 应用程序通过MPP API创建编码器实例
   - MPP驱动探测并初始化RKVENC2硬件编码器
   - 分配必要的内存资源，建立IOMMU映射
   - 配置编码参数（分辨率、码率、帧率等）

2. **任务提交阶段**:
   - 应用程序准备原始视频帧数据
   - 将输入帧和输出缓冲区信息打包成任务
   - 通过MPP接口提交编码任务到任务队列

3. **硬件执行阶段**:
   - MPP驱动从任务队列取出任务
   - 配置硬件寄存器（输入地址、输出地址、图像尺寸等）
   - 启动硬件编码器开始编码
   - 编码过程中通过中断机制进行状态反馈

4. **结果返回阶段**:
   - 硬件编码完成后触发中断
   - MPP驱动处理中断，读取编码结果
   - 将编码好的数据返回给应用程序
   - 释放相关资源，准备下一个任务

### 9.3 MPP关键技术特点

**硬件加速优势**:
- 利用专用硬件单元进行视频编码，相比CPU软编码大幅降低CPU占用
- 支持H.264/H.265等多种编码标准
- 硬件级别的码率控制和运动估计

**内存管理机制**:
- 使用IOMMU实现用户空间到硬件地址的映射
- 采用SRAM+DMA混合方案优化内存访问效率
- 避免了频繁的数据拷贝，提高处理性能

**多核心协调**:
- 通过CCU (Core Control Unit) 管理多个编码核心
- 支持多路并发编码，提高整体处理能力
- 动态负载均衡，优化资源利用率

**中断驱动模式**:
- 采用异步处理模式，提高系统响应性
- 硬件完成任务后主动通知CPU
- 避免CPU轮询等待，节省系统资源

### 9.4 MPP在RK3588平台的应用

在RK3588 AI视频网关项目中，MPP主要用于：
- 对AI处理后的视频帧进行H.264硬件编码
- 将多路视频流拼接后的画面压缩为RTSP流
- 通过GStreamer MPP插件(mpph264enc)实现与应用层的无缝对接
- 实现低延迟、高质量的视频编码输出

### 9.5 技术优势总结

MPP硬件编码器通过以下机制实现高效的视频编码：

1. **统一的硬件抽象层**: MPP框架将底层硬件抽象为统一接口，简化了硬件加速的使用
2. **任务队列管理**: 通过任务队列实现多任务并发处理
3. **内存管理**: 使用IOMMU实现高效的内存映射和管理
4. **中断驱动**: 通过中断机制实现异步处理，提高系统响应性
5. **时钟管理**: 动态时钟控制优化功耗
6. **CCU协调**: 多核心协调工作，提高处理能力
7. **故障处理**: 完善的错误处理和恢复机制

这种设计使得MPP能够在RK3588平台上提供高性能、低功耗的视频编码能力，满足实时视频处理的需求。
