# 🤖 智能AI售货机系统

基于 **ESP32-P4** 的边缘AI智能售货机，集成人脸识别、语音交互、触控操作和微信小程序远程管理。

## ✨ 核心特性

- **人脸智能识别** — OV5647 摄像头 + ESP-DL 模型，自动注册新用户，识别准确率 >90%
- **离线语音交互** — ESP-SR MultiNet7 中文命令词（可乐/雪碧/水/薯片），无需网络
- **触控交互** — ST7789 320×480 SPI LCD + XPT2046 触摸屏，LVGL 图形界面
- **舵机出货** — 5 路舵机独立控制，LEDC PWM 驱动
- **Web 服务器** — MJPEG 实时流 + REST API，支持远程监控
- **微信小程序** — 双角色 UI（用户/管理员），库存管理、销售统计、个性化推荐
- **深度睡眠** — 10 秒无人自动休眠，触摸唤醒，功耗 <5mA

## 🏗️ 系统架构

```
OV5647 Camera (800×800 RAW8 MIPI-CSI)
  → WhoFrameCap (帧采集管线)
    → WhoDetect (人脸检测, MSRMNP_S8_V1 @ 5FPS)
      → HumanFaceRecognizer (MFN 特征提取, SD卡数据库)
        → Vending UI (LVGL on ST7789 320×480)
          → Voice Control (ESP-SR MultiNet7)
            → SD Card (CSV 购买日志)
              → Web Server (MJPEG 流)
                → 微信小程序 (远程管理)
```

## 📁 项目结构

```
├── main/                          # 主程序
│   ├── main.cpp                   # 入口点，系统调度
│   ├── web_server.cpp/h           # HTTP 服务器
│   ├── wifi_init.c                # WiFi 初始化
│   └── jpeg_encoder.c/h           # 硬件 JPEG 编码器
├── components/                    # 组件
│   ├── spi_lcd_touch/             # SPI LCD + 触摸驱动
│   ├── vending_machine_ui/        # 售货机 LVGL 界面
│   ├── face_id_manager/           # 人脸 ID 管理
│   ├── voice_control/             # 语音控制
│   └── sd_card/                   # SD 卡驱动
├── mini-program-vending/          # 微信小程序前端
├── partitions.csv                 # 分区表
└── sdkconfig.defaults             # 默认配置
```

## 🛠️ 构建

```bash
# 设置目标芯片
idf.py set-target esp32p4

# 编译
idf.py build

# 烧录并监控
idf.py flash monitor

# 菜单配置
idf.py menuconfig
```

**环境要求：** ESP-IDF v5.5.4, `IDF_PATH` 环境变量

## 📊 主要性能指标

| 功能类型 | 指标 | 参数 |
|----------|------|------|
| AI 视觉 | 检测帧率 | 5 FPS |
| | 识别准确率 | >90% |
| 语音交互 | 识别延迟 | ~600ms |
| | 命令词 | 4 类商品 |
| 显示 | LCD 分辨率 | 320×480 |
| | 刷新率 | ~35 FPS |
| 功耗 | 深度睡眠 | ~3mA |
| | 空闲超时 | 30s |

## 📄 作品报告

- [2026嵌入式大赛应用赛道作品报告_完成版.docx](2026嵌入式大赛应用赛道作品报告_完成版.docx)
- [主要创新点.md](主要创新点.md)

## 📜 License

CC0-1.0
