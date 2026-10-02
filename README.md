# 墨水屏桌面记事本与日程终端

XIAO ESP32S3 Sense + Waveshare 7.5 英寸 800×480 黑白电子墨水屏。当前代码版本为 `2.6.1-serial-wifi-setup`。

屏幕显示日期、记事本、今日日程、计划日程和待办事项。Google Calendar 与 Google Tasks 数据经 Cloudflare Worker 同步，可选 NAS 局域网中继；设备每 15 分钟唤醒，同步成功后执行全屏可见刷新。

## 代码目录

| 目录 | 内容 |
| --- | --- |
| `eink_google_sync/` | ESP32 固件、证书、配置示例和 OTA 打包脚本 |
| `google_calendar_tasks_api/` | Google Apps Script Calendar / Tasks 只读 API |
| `cloudflare_worker/` | 签名认证、加密响应、KV 缓存和定时更新中继 |
| `nas_relay/` | 可选 NAS 局域网缓存中继及测试 |

## 硬件接线

| Driver HAT | XIAO |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| DIN | D10 |
| CLK | D8 |
| CS | D1 |
| DC | D2 |
| RST | D3 |
| BUSY | D4 |
| PWR | D5 |

Interface Config 使用 `0`（4-line SPI）；Display Config 保持已经验证能正常刷新的设置。供电使用 XIAO USB-C 5V。

## 配置与部署

1. 按 `google_calendar_tasks_api/README.md` 配置 Apps Script，启用 Google Tasks 高级服务，设置脚本属性 `DEVICE_KEY` 并部署 Web App。
2. 将 `cloudflare_worker/wrangler.toml.example` 复制为 `wrangler.toml`，创建并填写 KV namespace。将 `DEVICE_KEY` 和 Apps Script `GOOGLE_API_URL` 配置为 Cloudflare 私密变量，部署 Worker。
3. 将 `eink_google_sync/api_config.example.h` 复制为 `api_config.h`，填写 Worker HTTP 地址、同一设备密钥和 OTA 密码。将 sketch 中的 `RELAY_HOST` 改为自己的 Worker 主机名，与 `GOOGLE_API_URL` 一致。仓库中的 `YOUR_SUBDOMAIN` 是占位符。
4. Arduino 开发板选择 `XIAO_ESP32S3`，ESP32 core 使用已验证的 `2.0.17`。安装 `GxEPD2`、`Adafruit GFX Library`、`U8g2_for_Adafruit_GFX`、`WiFiManager` 和 `ArduinoJson`（7.x）。首次安装按固件目录 README 通过 USB 完成。
5. 如使用 NAS，将 `nas_relay/relay.env.example` 复制为 `relay.env`，填写相同密钥和自己的 Worker 主机名，并限制文件权限。运行 `run.sh` 或 `python3 eink_nas_relay.py --config relay.env`，再在固件私密配置中填写 NAS HTTP 地址。

当前固件中固定 Cloudflare IP 是现有网络环境的回退路径，并非长期保证的部署地址。更换 Worker 或网络时应核实这些地址是否适用。

NAS 缓存可能包含个人日程和记事，请保留在私有局域网。项目的 HTTP 中继协议对请求做 HMAC 认证，并对返回数据使用 AES-256-GCM 加密。

## 显示规则

- 名称完全为 `记事本` 的 Google Tasks 清单显示在左侧，最多显示 10 条。
- 今日到期且未完成的任务进入“今日日程”，与当天 Calendar 日程一起显示。
- 明天及后天到期的任务进入“计划日程”。
- 无日期、逾期及更远日期的任务保留在“待办事项”；完成后在下次同步移除。
- Google Tasks API 仅保留到期日期，不提供可靠的任务到期时间；带具体时间的安排使用 Google Calendar。
- 节假日订阅及标准全天节假日条目按 API 规则过滤。

## OTA

按 RST，同步成功后开放两分钟维护窗口。打开 `http://eink-calendar.local/update`，用用户名 `eink` 和本机配置的密码登录，同时上传同一构建生成的 `.bin` 和 `.sha256`。新版本首次正常同步及缓存检查通过后确认版本。详细步骤见固件目录 README。

## 本地测试

```text
cd cloudflare_worker
npm test
```

```text
cd nas_relay
python test_gcm_interop.py
```

NAS 加密互操作测试需要 Python 3 和 Node.js。`test_end_to_end.py` 为使用私密配置及线上 Worker 的手动集成测试，不应在公共 CI 中直接运行。

## 私密文件

仓库仅包含代码和配置示例。`api_config.h`、实际 `wrangler.toml`、`relay.env`、运行日志、日程缓存、编译产物和历史备份都不上传。固件二进制可能嵌入设备密钥和 OTA 密码，请只保留在自己的电脑和设备上。

该副本整理于 2026-10-02，部署主机名替换为占位符；版本号来源于当前本地代码，不代表已经验证设备正在运行同一版本。
