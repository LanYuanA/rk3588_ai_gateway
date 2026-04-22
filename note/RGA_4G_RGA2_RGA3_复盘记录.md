# RK3588 RGA 4G 问题、RGA2 / RGA3 选择与 DMA32 方案复盘

## 1. 背景

项目在实际运行时，出现了如下内核日志：

```text
rga_mm: RGA_MMU unsupported memory larger than 4G!
rga_mm: scheduler core[4] unsupported mm_flag[0x8]!
rga_mm: rga_mm_map_buffer map virtual address error!
rga_mm: job buffer map failed!
rga_job: rga request[...] submit failed!
BUG: Bad rss-counter state ...
```

现象是：

- 画面在多路流并发时卡死或崩溃
- dmesg 里持续打印 RGA 相关失败日志
- 随后出现 mm/rss-counter 异常，最终引发内核不稳定

## 2. 根因判断

这个问题不是单纯“CPU 太忙”，而是 **RGA 访问的内存路径不对**。

关键点有两个：

1. `scheduler core[4]` 说明有请求最终落到了 **RGA2**。
2. `RGA_MMU unsupported memory larger than 4G` 说明当前 buffer / 映射方式触发了 **4G 以上虚拟地址或不适配的内存映射路径**。

所以真正的问题通常是下面两类之一，或者两者同时存在：

- RGA 调度没有真正固定在 RGA3，库内部回退到了 RGA2
- buffer 仍然使用 `wrapbuffer_virtualaddr` 之类的路径，导致地址映射不可用

## 3. 结论先说

- **只写 RGA3，不一定就能彻底避免 4G 问题。**
- 要想更稳，必须同时处理：
  - RGA 调度核心
  - buffer 分配方式

也就是说：

- 只改调度，不改内存路径，仍可能炸
- 只改 DMA32，不固定调度，仍可能掉到 RGA2
- 最稳的组合是：**RGA3 + DMA32**

## 4. 官方 demo 的正确使用方式

官方提供的 demo 本质上演示的是：

1. 使用 DMA32 申请一块 4G 以内的内存
2. 将这块内存导入 RGA
3. 使用 `wrapbuffer_handle` 构造 `rga_buffer_t`
4. 调用 `imfill` / `imresize` / `imcopy` 等接口

示例关键代码：

```c
ret = dma_buf_alloc(DMA_HEAP_DMA32_UNCACHED_PATH, dst_buf_size, &dst_dma_fd, (void **)&dst_buf);
dst_handle = importbuffer_fd(dst_dma_fd, dst_buf_size);
dst = wrapbuffer_handle(dst_handle, dst_width, dst_height, dst_format);
ret = imfill(dst, dst_rect, 0xff00ff00);
```

### demo 的含义

- `dma_buf_alloc(...)`：申请 DMA32 内存
- `importbuffer_fd(...)`：把 dma fd 导入 RGA
- `wrapbuffer_handle(...)`：让 RGA 通过 handle 使用这块 DMA 内存
- `imfill(...)`：真正执行 RGA 操作

### demo 的核心价值

它避免了 `wrapbuffer_virtualaddr` 这种容易触发 4G/MMU 问题的方式。

## 5. 如果还想使用 RGA2，应该怎么做

如果业务上真的要用 RGA2，建议遵循官方 demo 的思路：

1. 不要直接把普通虚拟地址丢给 RGA
2. 使用 DMA32 分配器申请 4G 以内内存
3. `importbuffer_fd` 导入 RGA
4. 用 `wrapbuffer_handle` 生成 buffer
5. 再调用 `imfill` / `imresize`

也就是：**RGA2 + DMA32 + handle 路径**，而不是 `virtualaddr`。

## 6. 在本项目里的策略

本项目当前采用的策略是：

- 默认优先 RGA3
- 默认也优先 DMA32 作为 RGA 输入输出内存
- 如果你明确允许，才切到 RGA2 + DMA32

这样做的目的不是“追求最短代码”，而是为了稳定：

- 尽量把 CPU 压力交给 RGA
- 尽量把 4G 限制问题隔离在 DMA32 方案里
- 尽量不让 RGA 库偷偷回退到 RGA2 的不稳定路径

## 7. 运行时建议

### 优先推荐

默认直接运行项目，让程序自己按当前配置使用：

```bash
./RK3588_AI_Gateway
```

### 如果想确认是否启用了 DMA32

查看启动日志里是否有类似信息：

```text
[RGA内存] RK_RGA_USE_DMA32=1
```

### 如果要显式允许 RGA2 + DMA32

```bash
export RK_RGA2_WITH_DMA32=1
./RK3588_AI_Gateway
```

## 8. 典型判断方法

如果 dmesg 里出现：

```text
scheduler core[4]
```

基本可以判断：

- 有路径落到了 RGA2
- 或者 RGA 库内部在某次调用时回退到了 RGA2

如果只看到 RGA3 core0/core1，而没有 core[4]，说明调度方向是对的，但还要继续确认 buffer 是否走了 DMA32。

## 9. 面试时可以怎么说

可以这样概括这个问题：

> 在 RK3588 上做多路视频和 RGA 加速时，曾遇到 RGA_MMU 对 4G 以上虚拟地址不支持、调度回退到 RGA2(core[4]) 导致提交失败、最终引发内核内存状态异常的问题。最终通过把调度固定到 RGA3，并把 RGA buffer 改为 DMA32 + importbuffer_fd + wrapbuffer_handle 的方式，把内存映射限制隔离掉，同时保留 RGA2 的显式降级开关，解决了稳定性和 CPU 压力之间的平衡。

## 10. 当前仓库里的注意点

当前仓库中，官方 demo 依赖的头文件/源文件：

- `utils.h`
- `dma_alloc.h`

在这个仓库里并没有找到，所以这个 demo 更像是“官方参考样例”，不是本仓库内可直接独立编译的完整工程。

因此，真正落地时需要：

- 引入官方 RGA sample 里的配套实现
- 或者把 DMA32 分配、导入、释放函数接到你自己的工程里

## 11. 一句话结论

**RGA3 负责尽量避免落到 RGA2，DMA32 负责规避 4G 映射问题；两者要一起做，才足够稳。**