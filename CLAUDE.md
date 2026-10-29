# Clawd 桌宠

桌上一台 M5Stack CoreS3 SE 显示像素小螃蟹 Clawd，Claude Code 的 hooks 通过局域网 HTTP 把工作状态推给它。用户说明见 README.md，这里记维护需要知道的事。

## 结构

- `firmware/clawd_pet/clawd_pet.cpp`：全部固件代码。`.ino` 只有一行注释，不要把代码挪回 .ino（见下方"坑"）。
- `firmware/clawd_pet/secrets.h`：Wi-Fi 密码，已 gitignore，永远不要提交或打印内容。模板是 `secrets.example.h`。
- `hooks/clawd.sh`：hook 脚本，源文件在这里，实际运行的是装到 `~/.claude/hooks/` 的副本，改完要重新 `cp`（本机的安装位置见 CLAUDE.local.md）。
- `hooks/settings-snippet.json`：要合并进 `~/.claude/settings.json` 的 hooks 配置。
- `CLAUDE.local.md`（gitignore）：本机部署相关的事（网络、IP、hook 安装位置等），仓库是公开的，个人信息只放那里。
- `promo/`：宣传视频（`clawd-promo.mp4`，1920×1080，52 秒，已 gitignore）的脚本。`capture.py` 用录像模式从设备逐帧抓 7 个场景到 `promo/frames/`，`compose.py` 合成画面（设备外框 + 英文字幕 + hook 日志），`music.py <秒数> <beep 时间...>` 生成芯片音乐和 1800Hz 提示音，最后 ffmpeg 合轨。设备地址取环境变量 `CLAWD_HOST`（默认 clawd.local）。抓帧前把当前会话用的 hook 脚本临时换成空脚本，否则自己这个会话的螃蟹会进画面，抓完 `cp` 回来。
- `docs/*.png`：README 用的截图，从 promo 抓的帧放大 2 倍。

## 硬件

- 设备：M5Stack CoreS3 SE（ESP32-S3，320×240 触摸屏，喇叭；无电池/摄像头/IMU，必须一直插 USB 供电）。
- FQBN：`m5stack:esp32:m5stack_cores3`。板卡包 m5stack:esp32 3.3.10，库 M5Unified 0.2.27、M5GFX 0.2.34。
- 串口用 `arduino-cli board list` 查（macOS 上是 `/dev/cu.usbmodem*`，换口会变）。
- OTA（ArduinoOTA，密码是 secrets.h 里可选的 `OTA_PASS`），平时走 Wi-Fi 烧录，约 40 秒；默认分区方案 app3M_fat9M_16MB 自带两个 3MB 的 app 分区。固件崩到连不上网时才需要 USB。
- `secrets.h` 里还有 `TZ_INFO`（POSIX 时区，默认 UTC0）、`NTP_SERVER`、`WEATHER_LAT`/`WEATHER_LON`（不设就不起天气任务、不显示气温）。

## 常用命令

```bash
# 编译并走 Wi-Fi 烧录。compile --upload 不认 --upload-field，要分两步；空密码也必须传，否则 arduino-cli 要求交互输入
arduino-cli compile --fqbn m5stack:esp32:m5stack_cores3 firmware/clawd_pet
arduino-cli upload -p <设备 IP> --protocol network --upload-field password=<OTA_PASS> --fqbn m5stack:esp32:m5stack_cores3 firmware/clawd_pet
# USB 烧录（备用）
arduino-cli compile --upload -p /dev/cu.usbmodem* --fqbn m5stack:esp32:m5stack_cores3 firmware/clawd_pet
# 截屏（24 位 BMP）和触发动作，检查动画不用盯着设备
curl -4 -o shot.bmp http://clawd.local/shot
curl -4 "http://clawd.local/test?antic=walk"          # look|wave|bubbles|walk|yawn
curl -4 "http://clawd.local/test?react=angry&i=0"     # shy|blink|tickle|angry|wave_l|wave_r，i=屏上第几只
curl -4 "http://clawd.local/test?hour=6.5"            # 假装几点，可带小数看天色过渡（-1 恢复），busy=<分钟> 假装连续忙了多久
curl -4 "http://clawd.local/test?event=boat"          # boat|gull|star，马上放一次；bolt 马上劈一道闪电（只在雷暴时）
curl -4 "http://clawd.local/test?weather=rain"        # clear|cloudy|overcast|fog|rain|snow|storm，auto 恢复真实天气
# 录像模式：时钟冻住，/frame?n=1 往前走 40ms 并返回一帧（[次数][RGB565 大端] 游程编码），抓帧快慢不影响动画；/state 和 /test 也按这个时钟
curl -4 "http://clawd.local/rec?on=1"   # on=0 退出
# 查看/设置状态（必须加 -4，见下）
curl -4 http://clawd.local/
curl -4 "http://clawd.local/state?s=waiting&label=test"
# 更新已安装的 hook
cp hooks/clawd.sh ~/.claude/hooks/clawd.sh
```

## 协议与行为

- `GET /state?s=<state>&sid=<会话>&name=<项目名>&label=<text>&t=<ms>`：`sid` 是 session_id 前 8 位（缺省 "default"），每个 sid 一只螃蟹，最多记 4 个、同屏 3 个；`t` 是毫秒时间戳，按会话丢弃比上次旧的请求（hook 异步，可能乱序）。
- `GET /` 返回 `{"state":..,"label":..,"time":"HH:MM","sessions":[...]}`，state/label 取最急的那只。
- 状态：idle / thinking / working / waiting / done / sleep。thinking~done 的会话才显示；没有时显示一只"宠物"（只有 idle/sleep，会做随机小动作）。done 60 秒后回 idle；thinking/working 5 分钟无消息回 idle；会话 sleep（=结束）后槽位保留不显示，用来挡迟到的旧消息，闲置 30 分钟清掉；所有会话结束或 10 分钟没动静，宠物 sleep（亮度 128→20）；进 waiting 时喇叭 1800Hz 120ms。
- 触摸：点 waiting/done 的螃蟹 = 确认回 idle；否则按格子坐标判断点到眼睛/钳子/身体给反应，长按挠痒，2 秒连点 4 下生气。反应只盖在动作上，不改状态。
- 时间：NTP（`NTP_SERVER`，再加 pool.ntp.org、time.cloudflare.com 兜底），时区 `TZ_INFO`；没同步上时夜间/早安效果不触发。
- hook 映射：UserPromptSubmit→thinking，PreToolUse→working（label=工具名，`mcp__x__tool` 缩成 `tool`），PostToolUse / PostToolUseFailure→thinking，PermissionRequest→waiting，Stop / StopFailure→done，SessionEnd→sleep。Notification 统一调 `clawd.sh notify`，脚本按 `notification_type` 分：permission_prompt / elicitation_* dialog / agent_needs_input→waiting，elicitation_complete|response→thinking，idle_prompt→idle，其余忽略。
- bypass 模式下权限请求很少见。PreToolUse 遇到 AskUserQuestion / ExitPlanMode 改发 waiting（label=第一个问题的 header / "plan ready"），答完由 PostToolUse 切回 thinking。waiting 的 label：Bash 只放命令，改文件的放 `工具: 文件名`，截 60 字符，屏幕上再按宽度截。全部 `async: true`，脚本始终 `exit 0`，设备离线不能影响 Claude。
- 精灵按 20×16 网格画，`U = min(w/20, (h-labelH)/15)`；配色：身体 (217,119,87)，眼睛 (20,20,20)，文字底衬 (24,22,20)。
- 背景是沙滩（`drawBeach`）：海平线 y=100、沙滩 y=132（像素，固定，不随螃蟹大小变）。天色按本地时间分清晨 5–7 / 白天 7–17 / 傍晚 17–19 / 夜里四套（`SCENES`），`SKY_KEYS` 里每个分界前后各半小时逐字节混色，结果在全局 `sky`/`skyW`；沙堡还是篝火按占比最大的那套（`skyPhase()`）。太阳按分钟走一条弧线（5 点左边升、19 点右边落），月牙固定在 (284,46)，右上角留给时钟。影子色和 `C_INK`（天上的点点、z）跟着换。
- 偶发小东西（`Extra`）：帆船 3–7 分钟一次、海鸥 1–3 分钟（白天，雨雪天没有）、流星 40–120 秒（夜里，阴天没有）；萤火虫在傍晚和夜里一直有（雨雪天没有）。
- 天气（`Weather`）：`WEATHER_LAT`/`WEATHER_LON` 处，Open-Meteo 明文 HTTP、不用 key（`WEATHER_URL`），独立 FreeRTOS 任务 `weatherTask` 每 15 分钟拉一次，失败 1 分钟后重试，字符串查找解析 `"current":{` 里的 temperature_2m / weather_code。WMO 代码在 `weatherFromCode` 归成 7 类。`tintWeather` 在 `updateSky` 混完天色后去色调暗；阴天及以上（`gloomy()`）不画太阳月亮星星；雨/雷暴/雪/雾的粒子画在背景里，螃蟹盖在上面。雷暴 5–15 秒一道闪电（天空两闪 + 折线）。气温画在时钟左边，度数符号是手画的小方圈（Font0 没有）。`GET /` 多一个 `weather` 字段（code、temp、shown）。
- 文字（`drawText`）：取 `fonts::Font0`（GLCD 5x7）的字形，去掉左右空列成不等宽，按 k 倍画方块，外面描一圈 C_BG 边。带非 ASCII 字符的整串改用 `fonts::efontCN_12`（占约 200KB flash），8 方向偏移描边。
- 多只同屏：围着中间的沙堡（清晨/白天）或篝火（傍晚/夜里，`drawCenterpiece`）坐成半圈，座位写死在 `SEATS2`/`SEATS3`（按 320×240 调的像素坐标）。3 只时中间那只在后排（z=0，U=6），两边前排 U=7；按脚的 y 从远到近画（沙堡/篝火的排序键是 180），字最后画，走动中的不写字。座位按上屏先后排（`shownAt`），3 只时最新的坐后排中间。触摸按每只的包围盒判断，重叠时前排优先，没点中算最近的。`/test?react=..&i=` 的 i 是从左到右第几只。
- 走动（`Crab::from/to/moveAt/moveMs`）：每帧在 `render` 里算目标位置，`moveTo` 按约 160 像素/秒匀速插值，U 跟着取整变。新上屏的从座位那侧屏幕外走进来；离开的标 `leaving` 走到屏幕外再下屏。从 0 只变 1 只、或剩 0 只时不走动，直接换（和宠物在同一个位置）。
- 坐姿：`Pose.sit` 默认 true，`legPhase>=0` 或 `lift>0` 时自动站起；坐着时身体下移 1 格（`bodyDY`），道具和特效都按 `bodyDY` 定位。

## 坑

- **curl 必须 `-4`**：macOS 解析 `clawd.local` 时先试 IPv6，会卡满超时；hook 只等 1 秒，不加 `-4` 状态永远发不出去。
- **Arduino 自带 ctags 是 x86 二进制**，Apple Silicon 上报 "bad CPU type"。已把 `~/Library/Arduino15/packages/builtin/tools/ctags/5.8-arduino11/ctags` 换成指向 brew universal-ctags 的符号链接（原文件改名 `ctags.x86`）。universal-ctags 生成 .ino 原型会出错，所以代码放 .cpp。重装板卡包后这个链接可能被覆盖。
- 修改 `~/.claude/settings.json` 影响本机所有 Claude Code 会话，动之前要征得用户同意。

## 已知限制 / 可能的后续

- 子代理（Task）的工具调用应该带父会话的 session_id，算在同一只螃蟹上（推测，未验证）。
- 3 只同屏时后排那只的状态字（2 号）会压到前排两只的内侧钳子；前排螃蟹名字长时也会稍微压到后排那只。
- 半圈布局的触摸只按代码逻辑写了，没有用手实际点过重叠区域。
- Esc 打断不触发 Stop（官方文档写明），也没有专门的 hook。现在靠 Notification idle_prompt（停下约 60 秒没打字）回 idle，打断后是否一定发 idle_prompt 未实测；再不行还有 5 分钟超时。
- 批准权限后到工具结束之间没有 hook，仍显示 waiting。bypass 模式下基本遇不到，没处理。
- mDNS 不稳时可设 `CLAWD_HOST=<IP>`。
