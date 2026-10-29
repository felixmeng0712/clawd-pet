# Clawd 桌宠

[English](README.md)

一只放在桌上的 Clawd，跟着 Claude Code 的状态做动作：

| 状态 | 什么时候 | 动作 |
| --- | --- | --- |
| thinking | 你发出消息后、两次工具调用之间 | 轻轻一跳，抬眼看，头顶冒点点 |
| working | 调用工具时 | 按工具拿道具（见下），底部显示工具名 |
| waiting | 等你批准权限，或者 Claude 在问你问题 / 计划等你确认（屏幕上写问题标题或要跑的命令） | 先急得跺脚，再举起"!"牌子，有喇叭的设备会"嘀"一声 |
| done | 一轮回答结束 | 跳两下、撒彩纸、眯眼笑、出现对勾，60 秒后回到 idle |
| idle | 平时 | 随机眨眼，隔一阵张望、挥手、吐泡泡或横着走两步 |
| sleep | 没动静 10 分钟或所有会话结束 | 戴睡帽闭眼冒 z，屏幕调暗；醒来先伸懒腰 |

干活时的道具：Bash 敲键盘、Edit/Write 拿笔写字、Read/Grep/Glob 戴眼镜、WebSearch/WebFetch 举望远镜、Task/Agent 分身成三只。

它平时坐在沙滩上，背后有海浪一涨一退；走动、跳、跺脚时会站起来。天色跟着本地时间慢慢变：清晨朝霞、白天太阳和云、傍晚夕阳、夜里月牙和星星，每次换天色前后有一个小时的过渡；太阳从左边升起、中午在头顶、傍晚从右边落进海里。右上角有个小时钟，旁边是当地现在的气温。天气来自 Open-Meteo（每 15 分钟更新，地点在 `secrets.h` 里设）：多云多几朵云，阴天天色发灰、看不见太阳月亮，下雨下雪有雨丝雪花，下雪沙滩会变白，有雾时海平线上飘着雾带，雷暴会打闪电。

偶尔会有小东西路过：远处漂过一艘帆船（夜里挂着灯），白天有海鸥飞过，夜里有流星；傍晚和夜里沙滩上飘着萤火虫。

按本地时间：0–6 点戴睡帽、闲着时爱打哈欠；早上第一次醒来说 good morning。一个会话连续忙 20 分钟开始冒汗，45 分钟后眼皮耷拉。

**多个会话**：每个 Claude Code 会话一只螃蟹，忙着的最多同屏 3 只，围着沙堡（白天）或篝火（傍晚和夜里）坐成半圈，头顶是项目目录名（中文也能显示）；都闲下来后合成一只。新会话的螃蟹从旁边横着走进来，结束的挥挥钳子走掉，其他的走到新座位。

**摸它**：点身体会害羞，点眼睛眨眼，点钳子挥手回应，长按挠痒痒，2 秒内连点 4 下会生气。点正在 waiting / done 的螃蟹表示"知道了"，让它回到 idle；按实体按键会确认所有提醒。

## 硬件

只在 **M5Stack CoreS3 SE** 上测过（2 寸 320×240 触摸屏、喇叭，需要一直插 USB 供电）。代码基于 M5Unified，CoreS3 应该也能用；多只螃蟹的座位等布局是按 320×240 写死的，别的屏幕尺寸大概需要调。

## 1. 烧录固件

1. 把 `firmware/clawd_pet/secrets.example.h` 复制成 `secrets.h`，填上 Wi-Fi 名称和密码（必须是 2.4GHz）、时区（`TZ_INFO`，如中国是 `"CST-8"`），想看天气再填经纬度。
2. 用 Arduino IDE 打开 `firmware/clawd_pet/clawd_pet.ino`：
   - 开发板管理器地址添加 `https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json`，安装 **M5Stack** 板卡包
   - 库管理器里安装 **M5Unified**（会一起装上 M5GFX）
   - 分区方案用板卡默认的（16MB，两个 3MB app 分区）就行，Wi-Fi 更新要用到第二个分区
   - 选对板子（如 `M5CoreS3`）和端口，上传
3. 开机后屏幕底部显示 `clawd.local`，说明已联网。

第一次用 USB 烧好之后，以后可以走 Wi-Fi 更新（设备和电脑在同一个局域网）：

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_cores3 firmware/clawd_pet
arduino-cli upload -p <设备 IP> --protocol network --upload-field password= --fqbn m5stack:esp32:m5stack_cores3 firmware/clawd_pet
```

不设密码时同一局域网里谁都能刷它；想加密码就在 `secrets.h` 里定义 `OTA_PASS`，上传时写成 `--upload-field password=<密码>`。

也可以用命令行（以 CoreS3 为例）：

```bash
arduino-cli compile --upload -p /dev/cu.usbmodem* --fqbn m5stack:esp32:m5stack_cores3 firmware/clawd_pet
```

测试一下：

```bash
curl -4 "http://clawd.local/state?s=waiting"
```

## 2. 接上 Claude Code

```bash
mkdir -p ~/.claude/hooks && cp hooks/clawd.sh ~/.claude/hooks/ && chmod +x ~/.claude/hooks/clawd.sh
```

再把 `hooks/settings-snippet.json` 里的 `hooks` 合并进 `~/.claude/settings.json`。如果已经有 `hooks` 字段，要按事件逐个合并，不要直接覆盖。

所有 hook 都是 `async`，设备不在线也不会拖慢 Claude。

如果 `clawd.local` 解析不了，可以在 shell 配置里设置设备 IP：`export CLAWD_HOST=192.168.x.x`。

## 已知限制

- 超过 3 个会话同时在忙时只显示最急的 3 个（waiting 优先，done 最先让位）。
- 你按 Esc 打断 Claude 时不会触发 Stop，Clawd 会停在 thinking/working，大约 1 分钟后 Claude Code 发出"空闲"通知时回到 idle；万一没发，5 分钟没新消息也会回去。
- 批准权限后，工具执行期间仍显示 waiting，直到工具结束才切走。
