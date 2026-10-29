// Clawd 桌宠：通过 HTTP 接收 Claude Code hook 发来的状态，在屏幕上播放对应动画。
// 适用于 M5Stack 系列（CoreS3 / CoreS3 SE / StickC Plus2 / AtomS3 等），依赖 M5Unified。
//
// 接口：GET http://clawd.local/state?s=<state>&sid=<会话>&name=<项目名>&label=<text>&t=<ms 时间戳>
//   state: idle | thinking | working | waiting | done | sleep（sleep = 会话结束）
//   每个 sid 一只螃蟹，忙着的会话最多同屏 3 只；没有会话在忙时显示一只闲逛的 Clawd。
//   t 可选，用于丢弃同一会话乱序到达的旧请求。
// 调试：GET /shot 截屏（BMP），/rec + /frame 逐帧录像；GET /test?antic=walk | react=shy&i=0 | hour=6.5（-1 取消）| busy=<分钟> | event=boat|bolt
//   | weather=rain（clear|cloudy|overcast|fog|rain|snow|storm，auto 取消）
// 天气：每 15 分钟从 Open-Meteo 拉一次 secrets.h 里 WEATHER_LAT/WEATHER_LON 处的天气（没设就不显示）
// 固件可以走 Wi-Fi 更新（ArduinoOTA），见 CLAUDE.md

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <time.h>
#include "secrets.h"

enum State { IDLE, THINKING, WORKING, WAITING, DONE, SLEEP };
static const char* STATE_NAMES[] = {"idle", "thinking", "working", "waiting", "done", "sleep"};

static const uint32_t DONE_TIMEOUT_MS  = 60 * 1000;       // 完成动画保留多久后回到 idle
static const uint32_t STALE_TIMEOUT_MS = 5 * 60 * 1000;   // 思考/工作中太久没消息就当作中断了
static const uint32_t SLEEP_AFTER_MS   = 10 * 60 * 1000;  // 没动静多久后睡觉
static const uint32_t FORGET_AFTER_MS  = 30 * 60 * 1000;  // 闲着的会话多久后忘掉
static const uint32_t SWEAT_AFTER_MS   = 20 * 60 * 1000;  // 连续忙多久开始冒汗
static const uint32_t TIRED_AFTER_MS   = 45 * 60 * 1000;  // 再久就累得睁不开眼
static const uint8_t  BRIGHT = 128, DIM = 20;
static const int MAX_SESSIONS = 4, MAX_VISIBLE = 3;

// 摸它的反应
enum Reaction { R_NONE, R_SHY, R_BLINK, R_TICKLE, R_ANGRY, R_WAVE_L, R_WAVE_R };
static const char* REACTION_NAMES[] = {"none", "shy", "blink", "tickle", "angry", "wave_l", "wave_r"};
static const uint32_t REACTION_MS[] = {0, 1800, 900, 800, 2500, 1200, 1200};

// 闲着时的小动作
enum Antic { A_NONE, A_LOOK, A_WAVE, A_BUBBLES, A_WALK, A_YAWN };
static const char* ANTIC_NAMES[] = {"none", "look", "wave", "bubbles", "walk", "yawn"};
static const uint32_t ANTIC_MS[] = {0, 1900, 1600, 2600, 3000, 2000};

// 干活时按工具拿的道具
enum Prop { P_NONE, P_KEYBOARD, P_PENCIL, P_GLASSES, P_TELESCOPE, P_CLONES };

struct Crab {
  bool used = false;
  String sid, name, label;
  State state = IDLE, prev = IDLE;
  uint32_t since = 0, lastMsg = 0, busySince = 0;
  uint64_t lastStamp = 0;
  Reaction react = R_NONE;
  uint32_t reactSince = 0, reactUntil = 0;
  uint32_t tapWindow = 0;
  int taps = 0;
  // 屏幕上的位置：从 from 匀速走到 to，进出半圈、换座位时用
  struct { float cx, fy, u; } from{}, to{};
  uint32_t moveAt = 0, moveMs = 0, shownAt = 0;
  bool onScreen = false, leaving = false;
};

M5Canvas canvas(&M5.Display);
WebServer server(80);

Crab sessions[MAX_SESSIONS];
Crab pet;                  // 没有会话在忙时显示的那只，只会处于 idle / sleep
uint32_t lastActivity = 0;
bool beepPending = false;
uint8_t brightness = 0;

Antic antic = A_NONE;
uint32_t anticSince = 0, nextAnticAt = 0, nextBlinkAt = 0, blinkUntil = 0;
uint32_t greetUntil = 0;
int greetDay = -1;
int minuteOverride = -1;  // 调试用：假装现在是一天中的第几分钟

uint16_t C_BG, C_BODY, C_EYE, C_SHADOW, C_TEXT, C_ALERT, C_OK, C_DIM;
uint16_t C_BLUSH, C_ANGRY, C_CAP, C_KEY, C_KEYCAP, C_PENCIL, C_BRASS, C_BRASS_D, C_GLASS, C_STEAM;
uint16_t C_INK;  // 画在天空上的点点、z 等，随天色换深浅
uint16_t C_FIRE, C_FLAME, C_LOG, C_LOG_D;

// 精灵按 20x16 的格子设计，U 是每格的像素数；多只螃蟹时每只画之前重设
int U, OX, OY;

// 屏幕上每只螃蟹的位置，触摸时用来判断点到了谁；z 大的在前排
struct Slot { Crab* crab; int z, u, ox, oy; };
Slot slots[MAX_VISIBLE];
int slotCount = 0;

void px(int x, int y, int w, int h, uint16_t c) {
  canvas.fillRect(OX + x * U, OY + y * U, w * U, h * U, c);
}

// 半格精度，画道具细节用
void hpx(int x, int y, int w, int h, uint16_t c) {
  int x0 = OX + x * U / 2, y0 = OY + y * U / 2;
  canvas.fillRect(x0, y0, OX + (x + w) * U / 2 - x0, OY + (y + h) * U / 2 - y0, c);
}

// ---------- 时间（时区见 TZ_INFO，NTP 同步前返回 -1） ----------

int localMinute() {
  if (minuteOverride >= 0) return minuteOverride;
  struct tm tm;
  return getLocalTime(&tm, 0) ? tm.tm_hour * 60 + tm.tm_min : -1;
}

int localHour() {
  int m = localMinute();
  return m < 0 ? -1 : m / 60;
}

int localYDay() {
  struct tm tm;
  return getLocalTime(&tm, 0) ? tm.tm_yday : -1;
}

bool isNight() {
  int h = localHour();
  return h >= 0 && h < 6;
}

// ---------- 画螃蟹 ----------

enum Eyes { EYES_OPEN, EYES_CLOSED, EYES_UP, EYES_HAPPY, EYES_SQUINT, EYES_ANGRY, EYES_TIRED };

struct Pose {
  int lift = 0;      // 整体上移（跳跃）
  int bob = 0;       // 只有身体上下动，腿跟着伸缩
  Eyes eyes = EYES_OPEN;
  int eyeDX = 0;     // 眼珠左右看
  int armL = 0, armR = 0;  // 负数表示抬手，正数放低
  int legPhase = -1;       // -1 站立，0/1 交替迈步
  int shift = 0;           // 整体横移（格）
  bool blush = false, mouth = false, cap = false;
  bool sit = true;         // 坐在沙滩上；迈步、跳起来时自动站起
  uint16_t body = 0;       // 0 表示默认的橙色
};

bool sitting(const Pose& p) { return p.sit && p.lift == 0 && p.legPhase < 0; }

// 身体相对站姿的下移量，眼睛、钳子、道具都跟着它走
int bodyDY(const Pose& p) { return p.bob - p.lift + (sitting(p) ? 1 : 0); }

void drawClawd(const Pose& p) {
  int dy = bodyDY(p);
  uint16_t body = p.body ? p.body : C_BODY;

  // 影子：跳起来时变小，坐下时摊开
  int sw = p.lift > 0 ? 8 : sitting(p) ? 14 : 12;
  px(10 - sw / 2, 13, sw, 1, C_SHADOW);

  // 腿：站着时脚踩地面、身体 bob 时拉长；坐着时只露出一截脚尖
  const int legX[] = {5, 7, 12, 14};
  for (int i = 0; i < 4; i++) {
    if (sitting(p)) {
      px(legX[i], 12, 1, 1, body);
      continue;
    }
    int top = 11 + dy;
    int bottom = 13 - p.lift;
    if (p.legPhase >= 0 && (i % 2) == p.legPhase) bottom -= 1;
    if (bottom > top) px(legX[i], top, 1, bottom - top, body);
  }

  // 身体和钳子
  px(4, 3 + dy, 12, 8, body);
  px(2, 6 + dy + p.armL, 2, 2, body);
  px(16, 6 + dy + p.armR, 2, 2, body);

  // 眼睛
  int ex = p.eyeDX;
  switch (p.eyes) {
    case EYES_OPEN:
      px(7 + ex, 5 + dy, 1, 2, C_EYE);
      px(12 + ex, 5 + dy, 1, 2, C_EYE);
      break;
    case EYES_UP:
      px(7 + ex, 4 + dy, 1, 2, C_EYE);
      px(12 + ex, 4 + dy, 1, 2, C_EYE);
      break;
    case EYES_CLOSED:
      px(6, 6 + dy, 2, 1, C_EYE);
      px(11, 6 + dy, 2, 1, C_EYE);
      break;
    case EYES_HAPPY:
      px(6, 6 + dy, 1, 1, C_EYE); px(7, 5 + dy, 1, 1, C_EYE); px(8, 6 + dy, 1, 1, C_EYE);
      px(11, 6 + dy, 1, 1, C_EYE); px(12, 5 + dy, 1, 1, C_EYE); px(13, 6 + dy, 1, 1, C_EYE);
      break;
    case EYES_SQUINT:  // > <
      px(6, 4 + dy, 1, 1, C_EYE); px(7, 5 + dy, 1, 1, C_EYE); px(6, 6 + dy, 1, 1, C_EYE);
      px(13, 4 + dy, 1, 1, C_EYE); px(12, 5 + dy, 1, 1, C_EYE); px(13, 6 + dy, 1, 1, C_EYE);
      break;
    case EYES_ANGRY:   // 眉毛往中间压
      px(6, 4 + dy, 1, 1, C_EYE); px(7, 5 + dy, 1, 1, C_EYE); px(7, 6 + dy, 1, 1, C_EYE);
      px(13, 4 + dy, 1, 1, C_EYE); px(12, 5 + dy, 1, 1, C_EYE); px(12, 6 + dy, 1, 1, C_EYE);
      break;
    case EYES_TIRED:   // 眼皮耷拉
      px(6, 5 + dy, 2, 1, C_EYE); px(7, 6 + dy, 1, 1, C_EYE);
      px(11, 5 + dy, 2, 1, C_EYE); px(12, 6 + dy, 1, 1, C_EYE);
      break;
  }

  if (p.blush) {
    px(5, 7 + dy, 2, 1, C_BLUSH);
    px(13, 7 + dy, 2, 1, C_BLUSH);
  }
  if (p.mouth) hpx(18, 2 * (8 + dy), 4, 3, C_EYE);  // 打哈欠张嘴

  if (p.cap) {  // 睡帽，尖儿耷拉到右边
    px(7, 2 + dy, 8, 1, C_TEXT);
    px(8, 1 + dy, 6, 1, C_CAP);
    px(10, 0 + dy, 4, 1, C_CAP);
    px(14, 0 + dy, 1, 1, C_CAP);
    px(15, 1 + dy, 1, 1, C_TEXT);
  }
}

// 本体缩小站中间，两只小分身在两边蹦
void drawClones(const Pose& p, uint32_t now) {
  int u = U, ox = OX, oy = OY;
  U = max(1, u / 3);
  for (int i = 0; i < 2; i++) {
    Pose m;
    int ph = (now / 140 + i) % 2;
    m.legPhase = ph;
    m.lift = ph;
    m.armL = m.armR = ph ? -2 : 0;
    OX = i == 0 ? ox : ox + 20 * u - 20 * U;
    OY = oy + 14 * u - 14 * U;
    drawClawd(m);
  }
  U = max(1, u * 3 / 5);
  OX = ox + (20 * u - 20 * U) / 2;
  OY = oy + 14 * u - 14 * U;
  drawClawd(p);
  U = u; OX = ox; OY = oy;
}

Prop propFor(const String& tool) {
  if (tool == "Bash" || tool == "BashOutput" || tool == "KillShell") return P_KEYBOARD;
  if (tool == "Edit" || tool == "Write" || tool == "MultiEdit" || tool == "NotebookEdit") return P_PENCIL;
  if (tool == "Read" || tool == "Grep" || tool == "Glob" || tool == "LS") return P_GLASSES;
  if (tool == "WebSearch" || tool == "WebFetch") return P_TELESCOPE;
  if (tool == "Task" || tool == "Agent") return P_CLONES;
  return P_NONE;
}

void drawProp(Prop prop, const Pose& p, uint32_t now) {
  int dy = bodyDY(p);
  switch (prop) {
    case P_KEYBOARD:  // 摆在身前的键盘，钳子往下敲
      px(3, 10, 14, 2, C_KEY);
      for (int i = 0; i < 6; i++) {
        hpx(8 + i * 4, 21, 2, 1, C_KEYCAP);
        hpx(10 + i * 4, 23, 2, 1, C_KEYCAP);
      }
      break;

    case P_PENCIL: {  // 右钳握笔，地上一张纸慢慢写满
      px(15, 11, 4, 2, C_TEXT);
      int lines = (now / 500) % 4;
      for (int i = 0; i < lines && i < 3; i++)
        canvas.fillRect(OX + 15 * U + U / 3, OY + 11 * U + U / 3 + i * U / 2, (i == 2 ? 2 : 3) * U, max(1, U / 7), C_DIM);
      int b = 2 * (dy + p.armR);
      hpx(34, 13 + b, 1, 1, C_BLUSH);   // 橡皮
      hpx(34, 14 + b, 1, 6, C_PENCIL);
      hpx(34, 20 + b, 1, 1, C_EYE);     // 笔尖
      break;
    }

    case P_GLASSES: {
      int y = 2 * dy;
      for (int x : {12, 22}) {
        hpx(x, 8 + y, 6, 1, C_EYE);
        hpx(x, 15 + y, 6, 1, C_EYE);
        hpx(x, 8 + y, 1, 8, C_EYE);
        hpx(x + 5, 8 + y, 1, 8, C_EYE);
      }
      hpx(18, 10 + y, 4, 1, C_EYE);  // 鼻梁
      hpx(8, 10 + y, 4, 1, C_EYE);   // 镜腿
      hpx(28, 10 + y, 4, 1, C_EYE);
      break;
    }

    case P_TELESCOPE:  // 右眼贴着望远镜往右看，镜片一闪一闪
      px(11, 5 + dy, 7, 2, C_BRASS);
      px(14, 5 + dy, 1, 2, C_BRASS_D);
      px(18, 4 + dy, 2, 4, C_BRASS_D);
      if ((now / 600) % 3 == 0) hpx(38, 2 * (4 + dy) + 1, 1, 6, C_GLASS);
      break;

    default:
      break;
  }
}

void drawDots(uint32_t t, int x0) {
  int n = (t / 350) % 4;  // 冒出 0~3 个点
  for (int i = 0; i < n; i++) px(x0 + i * 2, 0, 1, 1, C_INK);
}

void drawSign(int dy) {  // 两只钳子托着写了 "!" 的牌子
  px(2, dy, 16, 3, C_ALERT);
  hpx(19, 2 * dy + 1, 2, 2, C_EYE);
  hpx(19, 2 * dy + 4, 2, 1, C_EYE);
}

void drawConfetti(uint32_t t) {
  const uint16_t colors[] = {C_OK, C_ALERT, C_BLUSH, C_GLASS, C_TEXT};
  for (int i = 0; i < 18; i++) {
    uint32_t h = (i + 1) * 2654435761u;
    uint32_t delay = (h >> 8) % 700;
    if (t < delay) continue;
    int y = -2 + (int)((t - delay) * (6 + (h >> 16) % 6) / 300);
    if (y > 25) continue;
    int x = (h >> 4) % 40 + ((t / 150 + i) % 2);
    hpx(x, y, 1, 1, colors[i % 5]);
  }
}

void drawBubbles(uint32_t at) {
  for (int i = 0; i < 3; i++) {
    int s = (int)at - i * 500;
    if (s < 0 || s > 1600) continue;
    int cx = OX + 15 * U + ((s / 200) % 2) * U / 3;
    int cy = OY + 3 * U - s * 3 * U / 1600;
    canvas.drawCircle(cx, cy, U / 4 + s * U / 4000, C_GLASS);
  }
}

String stateLabel(const Crab& c, uint16_t& color) {
  switch (c.state) {
    case THINKING: color = C_DIM;   return c.label.isEmpty() ? "thinking" : c.label;
    case WORKING:  color = C_TEXT;  return c.label;
    case WAITING:  color = C_ALERT; return c.label.isEmpty() ? "need you!" : c.label;
    case DONE:     color = C_OK;    return c.label.isEmpty() ? "done" : c.label;
    case SLEEP:    color = C_DIM;   return "";
    default:       color = C_DIM;   return c.label;
  }
}

void applyAntic(Pose& p, uint32_t at) {
  switch (antic) {
    case A_LOOK:
      p.eyeDX = at < 800 ? -1 : at < 1000 ? 0 : at < 1800 ? 1 : 0;
      break;
    case A_WAVE:
      p.armR = (at / 200) % 2 ? -3 : -2;
      p.eyes = EYES_HAPPY;
      break;
    case A_BUBBLES:
      p.eyes = EYES_UP;
      break;
    case A_WALK: {  // 横着走到左边，张望一下再走回来
      bool moving = at < 1000 || (at >= 1800 && at < 2600);
      if (at < 1000) p.shift = -(int)(at / 400);
      else if (at < 1800) p.shift = -2;
      else p.shift = min(0, -2 + (int)(at - 1800) / 400);
      if (moving) { p.legPhase = (at / 150) % 2; p.bob = p.legPhase; }
      else p.eyeDX = -1;
      break;
    }
    case A_YAWN:
      p.bob = 0;
      if (at > 200 && at < 1600) { p.eyes = EYES_CLOSED; p.mouth = true; p.armL = p.armR = -2; }
      break;
    default:
      break;
  }
}

void drawCrab(Crab& c, bool isPet, bool walking, uint32_t now) {
  uint32_t t = now - c.since;
  uint32_t busy = c.busySince ? now - c.busySince : 0;
  bool busyState = c.state == THINKING || c.state == WORKING || c.state == WAITING;
  Pose p;
  Prop prop = P_NONE;
  bool sign = false;
  p.cap = isNight() || c.state == SLEEP;

  switch (c.state) {
    case IDLE:
      p.bob = (now / 900) % 2 ? 0 : 1;
      if (now < blinkUntil) p.eyes = EYES_CLOSED;
      if (c.prev == SLEEP && t < 1500) {  // 刚醒：伸个懒腰
        p.armL = p.armR = -3;
        p.eyes = EYES_CLOSED;
        p.bob = 0;
        p.lift = (t > 300 && t < 1100) ? 1 : 0;
      } else if (isPet && antic != A_NONE) {
        applyAntic(p, now - anticSince);
      }
      break;

    case THINKING:
      p.eyes = EYES_UP;
      p.bob = (now / 500) % 2;
      if (c.prev != WORKING && t < 300) p.lift = 1;  // 冒出念头，轻轻一跳
      break;

    case WORKING: {
      int phase = (now / 160) % 2;
      prop = propFor(c.label);
      switch (prop) {
        case P_KEYBOARD:  p.armL = phase ? 2 : 1; p.armR = phase ? 1 : 2; break;
        case P_PENCIL:    p.armR = phase; p.bob = (now / 600) % 2; break;
        case P_GLASSES:   p.bob = (now / 700) % 2; break;
        case P_TELESCOPE: p.eyes = EYES_CLOSED; p.armR = -1; p.bob = (now / 900) % 2; break;
        case P_CLONES:    p.armL = phase ? -2 : -1; p.armR = phase ? -1 : -2; break;
        default:          // 两只钳子轮流敲
          p.legPhase = phase;
          p.armL = phase ? -1 : 0;
          p.armR = phase ? 0 : -1;
          p.bob = phase;
          break;
      }
      break;
    }

    case WAITING:
      if (t < 1200 || t % 6000 < 900) {  // 急得跺脚
        int ph = (now / 110) % 2;
        p.legPhase = ph;
        p.bob = ph;
        p.eyes = EYES_SQUINT;
        p.armL = p.armR = -1;
      } else {                           // 举牌子
        p.armL = p.armR = -3;
        p.bob = (now / 400) % 2;
        sign = true;
      }
      break;

    case DONE:
      if (t < 1600) {            // 开头跳两下
        int k = (t % 800) / 100; // 0..7
        static const int arc[] = {0, 1, 2, 3, 3, 2, 1, 0};
        p.lift = arc[k];
        p.armL = p.armR = -2;
      }
      p.eyes = EYES_HAPPY;
      break;

    case SLEEP:
      p.eyes = EYES_CLOSED;
      p.bob = 1;
      break;
  }

  if (busyState && busy > TIRED_AFTER_MS && (p.eyes == EYES_OPEN || p.eyes == EYES_UP)) p.eyes = EYES_TIRED;

  if (walking) {  // 走进来、走开、换座位：横着迈步，走开时挥挥钳子
    p.legPhase = (now / 150) % 2;
    p.bob = p.legPhase;
    p.lift = 0;
    prop = P_NONE;
    sign = false;
    if (c.leaving) { p.armR = (now / 200) % 2 ? -3 : -2; p.eyes = EYES_HAPPY; }
  }

  // 摸它的反应盖在状态动作上面
  uint32_t rt = now - c.reactSince;
  if (c.react != R_NONE && now >= c.reactUntil) c.react = R_NONE;
  if (c.react != R_NONE && c.react != R_BLINK) prop = P_NONE;
  switch (c.react) {
    case R_SHY:
      p.eyes = EYES_HAPPY; p.blush = true; p.armL = p.armR = 1; p.legPhase = -1;
      p.bob = (rt / 450) % 2;
      break;
    case R_BLINK:
      p.eyes = rt < 300 ? EYES_CLOSED : EYES_SQUINT;
      break;
    case R_TICKLE: {
      int ph = (now / 90) % 2;
      p.eyes = EYES_SQUINT; p.blush = true;
      p.armL = ph ? -3 : -1; p.armR = ph ? -1 : -3;
      p.lift = ph; p.bob = 0; p.legPhase = ph;
      break;
    }
    case R_ANGRY: {
      int ph = (now / 100) % 2;
      p.eyes = EYES_ANGRY; p.body = C_ANGRY;
      p.legPhase = ph; p.bob = ph; p.lift = 0;
      p.armL = p.armR = -2;
      break;
    }
    case R_WAVE_L: p.armL = (now / 180) % 2 ? -3 : -2; p.eyes = EYES_HAPPY; break;
    case R_WAVE_R: p.armR = (now / 180) % 2 ? -3 : -2; p.eyes = EYES_HAPPY; break;
    default: break;
  }
  if (c.react != R_NONE) sign = false;

  int baseOX = OX;
  OX += p.shift * U;
  int dy = bodyDY(p);

  if (prop == P_CLONES) drawClones(p, now);
  else drawClawd(p);
  drawProp(prop, p, now);

  if (!walking) switch (c.state) {
    case THINKING:
      drawDots(t, p.cap ? 15 : 13);
      break;
    case WAITING:
      if (sign) drawSign(dy);
      else if ((now / 400) % 2) { px(18, 0, 1, 2, C_ALERT); px(18, 3, 1, 1, C_ALERT); }
      break;
    case DONE:
      if (t < 3000) drawConfetti(t);
      if (t >= 1600) {  // 跳完再出对勾，免得和举起的钳子重叠
        px(15, 1, 1, 1, C_OK); px(16, 2, 1, 1, C_OK); px(17, 1, 1, 1, C_OK); px(18, 0, 1, 1, C_OK);
      }
      break;
    case SLEEP: {
      int z = (now / 700) % 3;   // z 往上飘
      px(17 + z, 3 - z, 1, 1, C_INK);
      if (z > 0) px(16 + z, 4 - z + 1, 1, 1, C_INK);
      break;
    }
    default:
      break;
  }

  if (busyState && busy > SWEAT_AFTER_MS && prop != P_CLONES)
    hpx(33, 6 + 2 * dy + (now / 120) % 5, 1, 2, C_GLASS);  // 汗珠往下淌
  if (c.react == R_TICKLE && (now / 120) % 2) {
    hpx(1, 4 + 2 * dy, 2, 1, C_INK);
    hpx(37, 4 + 2 * dy, 2, 1, C_INK);
  }
  if (c.react == R_ANGRY) {  // 头顶冒气
    int y = 4 - (now / 150) % 4;
    hpx(7, y + 2 * dy, 2, 2, C_STEAM);
    hpx(31, y + 2 * dy, 2, 2, C_STEAM);
  }
  if (isPet && antic == A_BUBBLES) drawBubbles(now - anticSince);

  OX = baseOX;
}

// ---------- 沙滩背景（天色跟着本地时间走） ----------

enum SkyPhase { SKY_DAWN, SKY_DAY, SKY_DUSK, SKY_NIGHT };

struct Scene { uint8_t sky[3][3], sea[3], foam[3], sand[3], ink[3], sun[3]; };
static const Scene SCENES[] = {
  // 清晨 5–7 点
  {{{120, 150, 200}, {230, 170, 170}, {250, 205, 165}}, {80, 130, 175}, {235, 225, 220}, {225, 195, 150}, {60, 50, 60}, {255, 200, 130}},
  // 白天 7–17 点
  {{{95, 170, 225}, {125, 195, 235}, {165, 215, 240}}, {40, 130, 185}, {235, 245, 250}, {238, 212, 158}, {40, 50, 70}, {255, 232, 120}},
  // 傍晚 17–19 点
  {{{110, 80, 140}, {230, 120, 100}, {250, 170, 100}}, {100, 80, 130}, {245, 200, 170}, {205, 160, 120}, {50, 30, 40}, {255, 140, 70}},
  // 夜里
  {{{10, 14, 32}, {18, 24, 48}, {28, 36, 64}}, {18, 36, 64}, {120, 140, 170}, {78, 70, 62}, {200, 195, 185}, {240, 236, 210}},
};

// 天色关键帧（一天中的分钟）：每个整点分界前后各半小时慢慢混过去
struct SkyKey { int minute; SkyPhase phase; };
static const SkyKey SKY_KEYS[] = {
  {0, SKY_NIGHT}, {270, SKY_NIGHT}, {330, SKY_DAWN}, {390, SKY_DAWN}, {450, SKY_DAY},
  {990, SKY_DAY}, {1050, SKY_DUSK}, {1110, SKY_DUSK}, {1170, SKY_NIGHT}, {1440, SKY_NIGHT},
};

Scene sky;       // 当前天色，由相邻两套混出来
float skyW[4];   // 每套天色占多少
int skyMinute;   // 没对上时间时按中午算

static const int HORIZON = 100, SHORE = 132;  // 海平线、沙滩起点（像素）

void tintWeather(Scene& s);  // 在下面天气那节

void updateSky() {
  int m = localMinute();
  skyMinute = m < 0 ? 12 * 60 : m;
  int i = 0;
  while (SKY_KEYS[i + 1].minute <= skyMinute) i++;
  const SkyKey &a = SKY_KEYS[i], &b = SKY_KEYS[i + 1];
  int k = (skyMinute - a.minute) * 256 / (b.minute - a.minute);
  // Scene 全是 uint8_t，可以逐字节混
  const uint8_t *pa = (const uint8_t*)&SCENES[a.phase], *pb = (const uint8_t*)&SCENES[b.phase];
  uint8_t* out = (uint8_t*)&sky;
  for (unsigned j = 0; j < sizeof(Scene); j++) out[j] = pa[j] + (pb[j] - pa[j]) * k / 256;
  tintWeather(sky);
  for (float& x : skyW) x = 0;
  skyW[a.phase] += (256 - k) / 256.0f;
  skyW[b.phase] += k / 256.0f;
}

// 占比最大的那套，决定沙堡还是篝火
SkyPhase skyPhase() {
  int best = 0;
  for (int i = 1; i < 4; i++) if (skyW[i] > skyW[best]) best = i;
  return (SkyPhase)best;
}

uint16_t rgb(const uint8_t c[3]) { return canvas.color565(c[0], c[1], c[2]); }

// a 和 b 按 k/256 混合
uint16_t mix(const uint8_t a[3], const uint8_t b[3], int k) {
  k = constrain(k, 0, 256);
  return canvas.color565(a[0] + (b[0] - a[0]) * k / 256, a[1] + (b[1] - a[1]) * k / 256, a[2] + (b[2] - a[2]) * k / 256);
}

// ---------- 天气（Open-Meteo，不用 key） ----------

#ifndef TZ_INFO
#define TZ_INFO "UTC0"
#endif
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif

enum Weather { W_CLEAR, W_CLOUDY, W_OVERCAST, W_FOG, W_RAIN, W_SNOW, W_STORM, W_COUNT };
static const char* WEATHER_NAMES[] = {"clear", "cloudy", "overcast", "fog", "rain", "snow", "storm"};
#ifdef WEATHER_LAT
static const char* WEATHER_URL = "http://api.open-meteo.com/v1/forecast?latitude=" WEATHER_LAT "&longitude=" WEATHER_LON
                                 "&current=temperature_2m,weather_code";
#endif
volatile int wxCode = -1;       // 拉到的 WMO 天气代码，-1 = 还没拉到
volatile float wxTemp = 0;
int wxOverride = -1;            // /test?weather= 假装的天气
uint32_t boltAt = 0, boltNext = 0, boltSeed = 0;  // 雷暴的闪电

Weather weatherFromCode(int c) {
  if (c <= 1) return W_CLEAR;
  if (c == 2) return W_CLOUDY;
  if (c == 3) return W_OVERCAST;
  if (c == 45 || c == 48) return W_FOG;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return W_SNOW;
  if (c >= 95) return W_STORM;
  if (c >= 51) return W_RAIN;  // 毛毛雨、冻雨、阵雨都算雨
  return W_CLEAR;
}

Weather weather() {
  if (wxOverride >= 0) return (Weather)wxOverride;
  return wxCode < 0 ? W_CLEAR : weatherFromCode(wxCode);
}

bool raining() { Weather w = weather(); return w == W_RAIN || w == W_STORM; }
bool gloomy() { return weather() >= W_OVERCAST; }  // 看不见太阳月亮星星

// 在 "current":{...} 里找某个数字字段（current_units 里有同名字段，要跳过）
bool jsonNumber(const String& body, const char* key, float& out) {
  int cur = body.indexOf("\"current\":{");
  if (cur < 0) return false;
  int i = body.indexOf(String("\"") + key + "\":", cur);
  if (i < 0) return false;
  out = body.substring(i + strlen(key) + 3).toFloat();
  return true;
}

// 单独一个任务拉天气，HTTP 卡住也不影响动画
#ifdef WEATHER_LAT
void weatherTask(void*) {
  for (;;) {
    bool ok = false;
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.setTimeout(8000);
      if (http.begin(WEATHER_URL) && http.GET() == 200) {
        String body = http.getString();
        float t, c;
        if (jsonNumber(body, "temperature_2m", t) && jsonNumber(body, "weather_code", c)) {
          wxTemp = t;
          wxCode = (int)c;
          ok = true;
        }
      }
      http.end();
    }
    vTaskDelay(pdMS_TO_TICKS(ok ? 15 * 60 * 1000 : 60 * 1000));  // 失败了一分钟后再试
  }
}
#endif

// 阴天下雨把天色往灰里拉：先去掉饱和度再调亮暗
void tintWeather(Scene& s) {
  static const int GREY[W_COUNT] = {0, 0, 150, 170, 170, 140, 200};    // 去色程度 /256
  static const int LIGHT[W_COUNT] = {256, 256, 225, 250, 195, 245, 150}; // 亮度 /256
  Weather w = weather();
  if (!GREY[w]) return;
  auto tint = [&](uint8_t* c) {
    int l = (c[0] * 3 + c[1] * 6 + c[2]) / 10;
    for (int i = 0; i < 3; i++) c[i] = (c[i] + (l - c[i]) * GREY[w] / 256) * LIGHT[w] / 256;
  };
  for (auto& row : s.sky) tint(row);
  tint(s.sea);
  tint(s.foam);
  tint(s.sand);
  if (w == W_SNOW)  // 沙滩积了一层薄雪，颜色跟着浪花走，夜里不会太亮
    for (int i = 0; i < 3; i++) s.sand[i] += (s.foam[i] - s.sand[i]) * 150 / 256;
}

void updateWeather(uint32_t now) {
  if (weather() != W_STORM) { boltAt = 0; return; }
  if (boltAt && now - boltAt > 400) boltAt = 0;
  if (!boltNext) boltNext = now + 3000;
  if (!boltAt && now >= boltNext) {
    boltAt = now;
    boltSeed = esp_random();
    boltNext = now + random(5000, 15000);
  }
}

// 闪电亮的那一下（两闪），返回 0~256
int boltFlash(uint32_t now) {
  if (!boltAt) return 0;
  uint32_t t = now - boltAt;
  return t < 80 ? 200 : t < 160 ? 0 : t < 260 ? 150 : 0;
}

void drawBolt(uint32_t now) {
  if (!boltFlash(now)) return;
  uint16_t c = canvas.color565(255, 250, 210);
  int x = 40 + boltSeed % 240, y = 0;
  for (int i = 0; i < 7 && y < HORIZON - 4; i++) {  // 折线往下劈
    int nx = x + (int)((boltSeed >> (i * 3)) % 13) - 6, ny = y + 10 + (boltSeed >> (i * 2 + 5)) % 6;
    canvas.drawLine(x, y, nx, ny, c);
    canvas.drawLine(x + 1, y, nx + 1, ny, c);
    x = nx;
    y = ny;
  }
}

void drawRain(uint32_t now) {
  static const uint8_t WHITE[3] = {255, 255, 255};
  int w = canvas.width(), h = canvas.height();
  bool storm = weather() == W_STORM;
  uint16_t drop = mix(sky.sky[2], WHITE, 110), splash = mix(sky.sand, WHITE, 90);
  int n = storm ? 60 : 40;
  for (int i = 0; i < n; i++) {
    uint32_t k = (i + 1) * 2654435761u;
    int speed = 260 + (k >> 24) % 80;  // 像素/秒，每滴稍有不同
    int y = (int)((now * speed / 1000 + (k >> 8)) % (h + 20)) - 10;
    int x = (int)((k >> 4) % (w + 40)) - (y * (storm ? 2 : 1)) / 4;  // 斜着往左下落
    if (x < -4) x += w + 40;
    canvas.drawLine(x, y, x - (storm ? 3 : 2), y + 6, drop);
    if (y > SHORE + 10 && (k >> 12) % 3 == 0) {  // 落到沙上溅一下
      int sy = SHORE + 10 + (k >> 16) % (h - SHORE - 14);
      if (abs(y - sy) < 3) { canvas.fillRect(x - 3, sy, 2, 1, splash); canvas.fillRect(x + 2, sy, 2, 1, splash); }
    }
  }
}

void drawSnow(uint32_t now) {
  uint16_t flake = canvas.color565(245, 248, 255);
  int w = canvas.width(), h = canvas.height();
  for (int i = 0; i < 36; i++) {
    uint32_t k = (i + 1) * 2246822519u;
    int speed = 18 + (k >> 24) % 20;
    int y = (int)((now * speed / 1000 + (k >> 8)) % (h + 10)) - 5;
    int x = (int)((k >> 4) % w) + (int)(6 * sinf(now / 900.0f + i));
    int sz = 1 + (k >> 20) % 2;
    canvas.fillRect(x, y, sz + 1, sz + 1, flake);
  }
}

// 雾：几条长长的浅灰带子在海平线附近慢慢飘
void drawFog(uint32_t now) {
  static const uint8_t FOG[3] = {215, 215, 210};
  int w = canvas.width();
  for (int i = 0; i < 4; i++) {
    int y = HORIZON - 18 + i * 14;
    int x = (int)((now / (90 + i * 40) + i * 97) % (w + 160)) - 160;
    uint16_t c = mix(i < 2 ? sky.sky[2] : sky.sea, FOG, 150);
    canvas.fillRoundRect(x, y, 150, 6, 3, c);
    canvas.fillRoundRect(x + 190, y + 3, 110, 5, 2, c);
  }
}

// ---------- 偶尔出现的小东西 ----------

enum Extra { X_BOAT, X_GULL, X_STAR, X_COUNT };
static const char* EXTRA_NAMES[] = {"boat", "gull", "star"};
static const uint32_t EXTRA_MS[] = {50000, 9000, 800};
uint32_t extraAt[X_COUNT], extraNext[X_COUNT];  // extraAt 为 0 表示没在播
uint32_t extraSeed[X_COUNT];

void startExtra(Extra x, uint32_t now) {
  extraAt[x] = now ? now : 1;
  extraSeed[x] = esp_random();
}

// 帆船随时可能有（夜里挂灯），海鸥只在白天，流星只在夜里
bool extraAllowed(Extra x) {
  switch (x) {
    case X_GULL: return skyW[SKY_DAY] + skyW[SKY_DAWN] > 0.6f && weather() <= W_OVERCAST;
    case X_STAR: return skyW[SKY_NIGHT] > 0.6f && !gloomy();
    default: return true;
  }
}

void updateExtras(uint32_t now) {
  static const uint32_t GAP_MIN[] = {180000, 60000, 40000}, GAP_MAX[] = {420000, 180000, 120000};
  for (int i = 0; i < X_COUNT; i++) {
    if (extraAt[i] && now - extraAt[i] > EXTRA_MS[i]) extraAt[i] = 0;
    if (!extraNext[i]) extraNext[i] = now + random(GAP_MIN[i] / 3, GAP_MAX[i] / 2);
    if (extraAt[i] || now < extraNext[i]) continue;
    if (extraAllowed((Extra)i)) startExtra((Extra)i, now);
    extraNext[i] = now + random(GAP_MIN[i], GAP_MAX[i]);
  }
}

// 0~1 的播放进度，没在播返回负数
float extraProgress(Extra x, uint32_t now) {
  return extraAt[x] ? (now - extraAt[x]) / (float)EXTRA_MS[x] : -1;
}

void drawShootingStar(uint32_t now) {
  static const uint8_t WHITE[3] = {255, 255, 240};
  float p = extraProgress(X_STAR, now);
  if (p < 0) return;
  uint32_t k = extraSeed[X_STAR];
  int x0 = 80 + k % 220, y0 = 6 + (k >> 8) % 30;
  float fade = p < 0.7f ? 1 : (1 - p) / 0.3f;
  for (int i = 0; i < 8; i++) {  // 拖着一条越来越淡的尾巴
    float q = p - i * 0.035f;
    if (q < 0) break;
    int x = x0 - (int)(q * 110), y = y0 + (int)(q * 40);
    canvas.fillRect(x, y, 2, 2, mix(sky.sky[0], WHITE, (int)(256 * fade * (8 - i) / 8)));
  }
}

void drawGull(uint32_t now) {
  float p = extraProgress(X_GULL, now);
  if (p < 0) return;
  uint32_t k = extraSeed[X_GULL];
  int w = canvas.width();
  bool ltr = k & 1;
  int x = ltr ? -10 + (int)(p * (w + 20)) : w + 10 - (int)(p * (w + 20));
  int y = 30 + (k >> 4) % 40 + (int)(3 * sinf(now / 400.0f));
  uint16_t c = canvas.color565(70, 72, 84);
  bool up = (now / 220) % 2;  // 翅膀一上一下
  canvas.fillRect(x - 1, y, 3, 2, c);
  canvas.fillRect(x - 4, y - (up ? 2 : 0), 3, 2, c);
  canvas.fillRect(x + 2, y - (up ? 2 : 0), 3, 2, c);
  canvas.fillRect(x - 6, y - (up ? 4 : -1), 2, 2, c);
  canvas.fillRect(x + 5, y - (up ? 4 : -1), 2, 2, c);
}

void drawBoat(uint32_t now) {
  static const uint8_t BLACK[3] = {0, 0, 0};
  float p = extraProgress(X_BOAT, now);
  if (p < 0) return;
  int w = canvas.width();
  bool ltr = extraSeed[X_BOAT] & 1;
  int x = ltr ? -20 + (int)(p * (w + 40)) : w + 20 - (int)(p * (w + 40));
  int y = HORIZON + 6 + (now / 900) % 2;  // 船底那一行
  canvas.fillRect(x - 7, y - 3, 15, 2, mix(sky.sea, BLACK, 150));
  canvas.fillRect(x - 5, y - 1, 11, 1, mix(sky.sea, BLACK, 150));
  canvas.fillRect(x, y - 14, 1, 11, mix(sky.sea, BLACK, 180));
  for (int r = 0; r < 9; r++)  // 三角帆
    canvas.fillRect(x + 1, y - 13 + r, 1 + r * 6 / 9, 1, rgb(sky.foam));
  if (skyW[SKY_NIGHT] > 0.5f && (now / 700) % 4) canvas.fillRect(x, y - 16, 2, 2, C_FLAME);
}

// 傍晚和夜里沙滩上飘着几只萤火虫，一明一暗
void drawFireflies(uint32_t now) {
  float dark = skyW[SKY_DUSK] + skyW[SKY_NIGHT];
  if (dark < 0.5f || raining() || weather() == W_SNOW) return;
  static const uint8_t GLOW[3] = {210, 245, 120};
  int w = canvas.width(), h = canvas.height();
  for (int i = 0; i < 5; i++) {
    uint32_t k = (i + 3) * 2654435761u;
    if ((now / 60 + (k >> 8)) % 50 > 30) continue;
    int x = (k >> 4) % w + (int)(16 * sinf(now / 2300.0f + i * 1.7f));
    int y = SHORE + 14 + (k >> 16) % (h - SHORE - 24) + (int)(8 * sinf(now / 1700.0f + i * 2.3f));
    canvas.fillRect(x - 1, y, 4, 2, mix(sky.sand, GLOW, 110));
    canvas.fillRect(x, y - 1, 2, 4, mix(sky.sand, GLOW, 110));
    canvas.fillRect(x, y, 2, 2, mix(sky.sand, GLOW, 256));
  }
}

void drawBeach(uint32_t now) {
  static const uint8_t BLACK[3] = {0, 0, 0}, WHITE[3] = {255, 255, 255}, STAR[3] = {225, 230, 245};
  int w = canvas.width(), h = canvas.height();
  updateSky();
  const Scene& s = sky;
  uint16_t sea = rgb(s.sea), sea2 = mix(s.sea, s.foam, 90), foam = rgb(s.foam);
  uint16_t sand = rgb(s.sand), wet = mix(s.sand, BLACK, 36);
  C_SHADOW = mix(s.sand, BLACK, 70);
  C_INK = rgb(s.ink);

  int flash = boltFlash(now);
  for (int i = 0; i < 3; i++) canvas.fillRect(0, i * HORIZON / 3, w, HORIZON / 3 + 1, mix(s.sky[i], WHITE, flash));
  drawBolt(now);

  // 星星和月牙随着天黑慢慢显出来；阴天看不见
  float night = gloomy() ? 0 : skyW[SKY_NIGHT];
  if (night > 0) {
    for (int i = 0; i < 16; i++) {
      uint32_t k = (i + 1) * 2654435761u;
      int x = (k >> 4) % w, y = (k >> 14) % (HORIZON - 12);
      if (x > 250 && y < 62) continue;  // 别挡月亮和时钟
      if ((now / 600 + i) % 5) canvas.fillRect(x, y, 2, 2, mix(s.sky[0], STAR, (int)(night * 256)));
    }
    if (night > 0.5f) {
      int k = (int)((night - 0.5f) * 512);
      canvas.fillCircle(284, 46, 11, mix(s.sky[1], SCENES[SKY_NIGHT].sun, k));
      canvas.fillCircle(289, 42, 9, rgb(s.sky[1]));  // 挖掉一块成月牙
    }
    drawShootingStar(now);
  }

  // 太阳 5 点从左边海平线升起，中午到正上方，19 点在右边落下；贴近海平线时大一点，下半截被海挡住
  float f = (skyMinute - 300) / 840.0f;
  if (f > -0.15f && f < 1.15f && !gloomy()) {
    float e = sinf(f * PI);
    int r = 13 + (int)(5 * (1 - max(0.0f, e)));
    canvas.fillCircle(30 + (int)(f * 260), HORIZON - (int)(e * 72), r, rgb(s.sun));
  }

  // 云：晴天两朵白的，多云四朵，阴雨天夜里也有，颜色是更深一点的灰
  Weather wx = weather();
  float cloudy = wx >= W_CLOUDY ? 1 : skyW[SKY_DAY] + skyW[SKY_DAWN];
  if (cloudy > 0.05f && wx != W_FOG) {
    // 晴天、多云是白云；阴雨天白天是比天深的灰云。夜里一律比天稍亮一点，不然看不见
    uint8_t dayC[3], nightC[3], c[3];
    const uint8_t* to = wx >= W_OVERCAST && wx != W_SNOW ? BLACK : WHITE;
    int dk = wx >= W_OVERCAST && wx != W_SNOW ? 60 : (int)(200 * min(1.0f, skyW[SKY_DAY] + skyW[SKY_DAWN] + 0.3f));
    int nk = (int)(skyW[SKY_NIGHT] * 256);
    for (int i = 0; i < 3; i++) {
      dayC[i] = s.sky[1][i] + (to[i] - s.sky[1][i]) * dk / 256;
      nightC[i] = s.sky[1][i] + (s.foam[i] - s.sky[1][i]) * 70 / 256;
      c[i] = dayC[i] + (nightC[i] - dayC[i]) * nk / 256;
    }
    uint16_t cloud = rgb(c);
    int nc = wx == W_CLEAR ? 2 : wx == W_CLOUDY ? 4 : 5;
    for (int i = 0; i < nc; i++) {
      int cx = (int)((now / 150 + i * (w + 80) / nc + (i & 1) * 23) % (w + 80)) - 40, cy = 22 + (i % 3) * 22 - (i & 1) * 8;
      canvas.fillRoundRect(cx, cy, 36, 10, 5, cloud);
      canvas.fillRoundRect(cx + 8, cy - 6, 20, 10, 5, cloud);
    }
  }
  drawGull(now);

  // 海：两排浪花反向慢慢漂
  canvas.fillRect(0, HORIZON, w, SHORE - HORIZON, sea);
  canvas.fillRect(0, HORIZON, w, 1, sea2);
  drawBoat(now);
  for (int row = 0; row < 2; row++) {
    int y = HORIZON + 8 + row * 11;
    int off = (int)((now / (50 + row * 30)) % 60);
    if (row) off = 60 - off;
    for (int x = off - 60; x < w; x += 60) canvas.fillRect(x + row * 30, y, 14, 2, sea2);
  }

  // 沙滩，加点固定的沙粒
  canvas.fillRect(0, SHORE, w, h - SHORE, sand);
  for (int i = 0; i < 24; i++) {
    uint32_t k = (i + 7) * 2246822519u;
    canvas.fillRect((k >> 4) % w, SHORE + 14 + (k >> 16) % (h - SHORE - 14), 2, 2, wet);
  }

  // 浪一涨一退：海水漫上沙滩，前沿是白沫，后面留一道湿沙
  float tide = 4 * sinf(now * 2 * PI / 5000.0f) + 2;
  for (int x = 0; x < w; x += 4) {
    int e = SHORE + (int)(tide + 2 * sinf(x / 18.0f + now / 600.0f));
    if (e > SHORE) canvas.fillRect(x, SHORE, 4, e - SHORE, sea);
    canvas.fillRect(x, e, 4, 2, foam);
    canvas.fillRect(x, e + 2, 4, 3, wet);
  }
  drawFireflies(now);
  if (wx == W_FOG) drawFog(now);
  if (raining()) drawRain(now);
  if (wx == W_SNOW) drawSnow(now);
}

// 多只螃蟹围着的东西：白天堆沙堡，傍晚和夜里点篝火。按 12x10 的格子画，底边在 baseY
void drawCenterpiece(uint32_t now, int cx, int baseY) {
  static const uint8_t BLACK[3] = {0, 0, 0}, BROWN[3] = {150, 100, 50};
  int u = U, ox = OX, oy = OY;
  U = 4;
  OX = cx - 6 * U;
  OY = baseY - 10 * U;
  SkyPhase sp = skyPhase();
  const Scene& s = sky;

  if (sp == SKY_DUSK || sp == SKY_NIGHT) {
    // 地上一圈暖光，两根木头，三簇火苗轮流窜高，火星往上飘
    canvas.fillEllipse(OX + 6 * U, OY + 10 * U, 9 * U, 2 * U, mix(s.sand, (const uint8_t[]){255, 140, 40}, 90));
    px(1, 8, 10, 2, C_LOG);
    px(1, 8, 1, 2, C_LOG_D);
    px(10, 8, 1, 2, C_LOG_D);
    static const int H[3][3] = {{4, 6, 3}, {5, 7, 4}, {3, 6, 5}};
    int f = (now / 120) % 3;
    for (int i = 0; i < 3; i++) {
      int x = 3 + i * 2, hh = H[f][i];
      px(x, 8 - hh, 2, hh, C_FIRE);
      px(x, 10 - hh, 2, hh - 2, C_FLAME);
    }
    for (int i = 0; i < 3; i++) {
      int k = (now / 90 + i * 5) % 12;
      if (k >= 2) hpx(8 + (i * 5 + now / 300) % 8, 14 - 2 * k, 1, 1, C_FLAME);
    }
  } else {
    uint16_t wall = mix(s.sand, BROWN, 70), side = mix(s.sand, BLACK, 60), door = mix(s.sand, BLACK, 120);
    px(-1, 10, 14, 1, C_SHADOW);
    px(0, 6, 12, 4, wall);
    for (int x : {0, 2, 9, 11}) px(x, 5, 1, 1, wall);  // 城垛
    px(4, 2, 4, 4, wall);
    px(4, 1, 1, 1, wall);
    px(7, 1, 1, 1, wall);
    px(11, 6, 1, 4, side);
    px(7, 2, 1, 4, side);
    px(5, 7, 2, 3, door);
    hpx(11, 6, 2, 2, door);
    hpx(11, -4, 1, 6, C_LOG);  // 旗杆，小旗随风摆
    if ((now / 300) % 2) hpx(12, -4, 3, 2, C_ANGRY);
    else { hpx(12, -4, 2, 2, C_ANGRY); hpx(14, -3, 1, 2, C_ANGRY); }
  }
  U = u; OX = ox; OY = oy;
}

// ---------- 布局 ----------

void placeCrab(int x, int y, int w, int h) {
  U = max(1, min(w / 20, h / 15));
  OX = x + (w - 20 * U) / 2;
  OY = y + (h - 14 * U) / 2;
}

// 多只时围着沙堡/篝火坐成半圈：cx 是中线，feetY 是影子那一行的顶，z=0 后排（远、小、高）
struct Seat { int cx, feetY, u, z; };
static const Seat SEATS2[] = {{82, 200, 7, 1}, {238, 200, 7, 1}};
static const Seat SEATS3[] = {{70, 206, 7, 1}, {160, 150, 6, 0}, {250, 206, 7, 1}};

typedef decltype(Crab::to) Place;

Place seatPlace(const Seat& s) { return {(float)s.cx, (float)s.feetY, (float)s.u}; }

Place placeNow(const Crab& c, uint32_t now) {
  if (!c.moveMs || now - c.moveAt >= c.moveMs) return c.to;
  float k = (now - c.moveAt) / (float)c.moveMs;
  return {c.from.cx + (c.to.cx - c.from.cx) * k, c.from.fy + (c.to.fy - c.from.fy) * k, c.from.u + (c.to.u - c.from.u) * k};
}

bool moving(const Crab& c, uint32_t now) { return c.moveMs && now - c.moveAt < c.moveMs; }

void snapTo(Crab& c, Place p) {
  c.from = c.to = p;
  c.moveMs = 0;
}

void moveTo(Crab& c, Place p, uint32_t now) {
  if (p.cx == c.to.cx && p.fy == c.to.fy && p.u == c.to.u) return;
  c.from = placeNow(c, now);
  c.to = p;
  float d = hypotf(p.cx - c.from.cx, p.fy - c.from.fy);
  c.moveAt = now;
  c.moveMs = constrain((int)(d / 0.16f), 400, 2200);  // 每秒走 160 像素左右
}

// 同一排、同样大小，挪到屏幕外最近的那一边
Place offscreen(Place p, int w) {
  p.cx = p.cx < w / 2 ? -11 * p.u : w + 11 * p.u;
  return p;
}

void applyPlace(Place p) {
  U = max(1, (int)lroundf(p.u));
  OX = (int)lroundf(p.cx) - 10 * U;
  OY = (int)lroundf(p.fy) - 13 * U;
}

// 像素字：用 GLCD 5x7 字库的字形，去掉左右空列变成不等宽，外面描一圈深色边
const uint8_t* glyph(char ch) { return lgfx::fonts::Font0.chartbl + (uint8_t)ch * 5; }

// 字形有笔画的列范围，空格算 2 列
void glyphSpan(char ch, int& a, int& b) {
  const uint8_t* g = glyph(ch);
  a = 0;
  b = 4;
  while (a <= 4 && !g[a]) a++;
  if (a > 4) { a = 0; b = 1; return; }
  while (!g[b]) b--;
}

int pixelTextWidth(const String& s, int k) {
  int cols = 0;
  for (unsigned i = 0; i < s.length(); i++) {
    int a, b;
    glyphSpan(s[i], a, b);
    cols += b - a + 2;
  }
  return cols ? (cols - 1) * k : 0;
}

bool isAscii(const String& s) {
  for (unsigned i = 0; i < s.length(); i++) if ((uint8_t)s[i] >= 0x80) return false;
  return true;
}

// 5x7 字库只有 ASCII，带中文的字用 12 像素的 efont 中文点阵，同样描一圈深色边
void drawTextCJK(String s, int x, int y, uint8_t datum, uint16_t color, int maxW) {
  canvas.setFont(&fonts::efontCN_12);
  canvas.setTextSize(1);
  while (s.length() > 1 && canvas.textWidth(s) > maxW) {  // 按 UTF-8 字符往回删
    int i = s.length() - 1;
    while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) i--;
    s.remove(i);
  }
  canvas.setTextDatum(datum);
  canvas.setTextColor(C_BG);
  for (int dx = -1; dx <= 1; dx++)
    for (int dy = -1; dy <= 1; dy++)
      if (dx || dy) canvas.drawString(s, x + dx, y + dy);
  canvas.setTextColor(color);
  canvas.drawString(s, x, y);
  canvas.setFont(&fonts::Font0);
}

// datum 低 2 位是水平对齐（0 左 1 中 2 右），3~4 位是垂直对齐（0 上 4 中 8 下）
void drawText(String s, int x, int y, uint8_t datum, uint16_t color, int k, int maxW) {
  if (!isAscii(s)) return drawTextCJK(s, x, y, datum, color, maxW);
  while (s.length() > 1 && pixelTextWidth(s, k) > maxW) s.remove(s.length() - 1);
  if (s.isEmpty()) return;
  int tw = pixelTextWidth(s, k), th = 8 * k;
  int x0 = x - ((datum & 3) == 1 ? tw / 2 : (datum & 3) == 2 ? tw : 0);
  int y0 = y - ((datum & 12) == 8 ? th : (datum & 12) == 4 ? th / 2 : 0);
  for (int pass = 0; pass < 2; pass++) {  // 先描边再填字
    int cx = x0;
    for (unsigned i = 0; i < s.length(); i++) {
      int a, b;
      glyphSpan(s[i], a, b);
      const uint8_t* g = glyph(s[i]);
      for (int c = a; c <= b; c++)
        for (int r = 0; r < 8; r++)
          if ((g[c] >> r) & 1) {
            int gx = cx + (c - a) * k, gy = y0 + r * k;
            if (pass) canvas.fillRect(gx, gy, k, k, color);
            else canvas.fillRect(gx - k, gy - k, 3 * k, 3 * k, C_BG);
          }
      cx += (b - a + 2) * k;
    }
  }
}

bool visibleState(State s) { return s >= THINKING && s <= DONE; }

int priority(State s) {
  switch (s) {
    case WAITING: return 3;
    case THINKING: case WORKING: return 2;
    case DONE: return 1;
    default: return 0;
  }
}

// 要显示的会话：最多 3 只，挤不下时先让 done 的让位。按上屏先后排座位，
// 3 只时最新来的坐后排中间，已经坐着的两只基本不用挪
int visibleSessions(Crab* out[]) {
  int n = 0;
  for (int pr = 3; pr >= 1; pr--)
    for (auto& c : sessions)
      if (c.used && priority(c.state) == pr && n < MAX_VISIBLE) out[n++] = &c;
  auto key = [](Crab* c) { return c->onScreen && !c->leaving ? c->shownAt : UINT32_MAX; };
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && (key(out[j]) < key(out[j - 1]) || (key(out[j]) == key(out[j - 1]) && out[j] < out[j - 1])); j--)
      std::swap(out[j], out[j - 1]);
  if (n == 3) std::swap(out[1], out[2]);
  return n;
}

bool anyVisible() {
  for (auto& c : sessions) if (c.used && visibleState(c.state)) return true;
  return false;
}

void drawClock() {
  int m = localMinute();
  if (m < 0) return;
  char buf[6];
  snprintf(buf, sizeof buf, "%02d:%02d", m / 60, m % 60);
  int x = canvas.width() - 4;
  drawText(buf, x, 4, top_right, C_DIM, 1, 60);
  if (wxCode < 0) return;
  // 左边是当地气温，度数符号字库里没有，画个小方圈
  String t = String((int)lroundf(wxTemp));
  x -= pixelTextWidth(buf, 1) + 10 + 4;
  drawText(t, x, 4, top_right, C_DIM, 1, 30);
  canvas.fillRect(x + 1, 3, 5, 5, C_BG);
  canvas.drawRect(x + 2, 4, 3, 3, C_DIM);
}

void render(uint32_t now) {
  static int prevN = 0;
  drawBeach(now);
  int w = canvas.width(), h = canvas.height();
  Crab* vis[MAX_VISIBLE];
  int n = visibleSessions(vis);
  const Seat* seats = n == 2 ? SEATS2 : SEATS3;

  canvas.setTextSize(2);
  placeCrab(0, 0, w, h - canvas.fontHeight() - 4);
  Place single = {(float)(OX + 10 * U), (float)(OY + 13 * U), (float)U};
  Place target[MAX_VISIBLE];
  for (int i = 0; i < n; i++) target[i] = n <= 1 ? single : seatPlace(seats[i]);

  // 新来的从离座位近的那边走进来，走了的挥挥手走出去，其他的走到新座位。
  // 只剩一只或一只都没了时不走动，直接换成大的那只，和没有会话时一样
  for (auto& c : sessions) {
    int idx = -1;
    for (int i = 0; i < n; i++) if (vis[i] == &c) idx = i;
    if (idx >= 0) {
      if (!c.onScreen || c.leaving) c.shownAt = now;
      if (!c.onScreen) {
        c.onScreen = true;
        if (prevN >= 1 && n >= 2) {
          snapTo(c, offscreen(target[idx], w));
          moveTo(c, target[idx], now);
        } else {
          snapTo(c, target[idx]);
        }
      } else if (prevN == 0) {
        snapTo(c, target[idx]);
      } else {
        moveTo(c, target[idx], now);
      }
      c.leaving = false;
    } else if (c.onScreen) {
      if (n == 0 && !c.leaving) c.onScreen = false;
      else if (!c.leaving) { c.leaving = true; moveTo(c, offscreen(placeNow(c, now), w), now); }
      else if (!moving(c, now)) { c.leaving = false; c.onScreen = false; }
    }
  }

  // 从远到近画（按脚的位置），沙堡/篝火夹在后排和前排之间
  struct Item { float key; Crab* crab; };
  Item items[MAX_SESSIONS + 2];
  int m = 0;
  if (n >= 2) items[m++] = {180, nullptr};
  if (n == 0) { snapTo(pet, single); items[m++] = {single.fy, &pet}; }
  for (auto& c : sessions) if (c.used && c.onScreen) items[m++] = {placeNow(c, now).fy, &c};
  for (int i = 1; i < m; i++)
    for (int j = i; j > 0 && items[j].key < items[j - 1].key; j--) std::swap(items[j], items[j - 1]);
  for (int i = 0; i < m; i++) {
    if (!items[i].crab) { drawCenterpiece(now, w / 2, n == 2 ? 214 : 218); continue; }
    Crab& c = *items[i].crab;
    applyPlace(c.to.u ? placeNow(c, now) : single);
    drawCrab(c, &c == &pet, moving(c, now), now);
  }

  // 摸的时候按当前位置判断；字最后画，免得被别的螃蟹挡住，走动中的不写字
  slotCount = n ? n : 1;
  if (n == 0) {
    applyPlace(single);
    slots[0] = {&pet, 0, U, OX, OY};
    drawText(pet.label, w / 2, h - 2, bottom_center, greetUntil ? C_TEXT : C_DIM, 2, w - 8);
  }
  for (int i = 0; i < n; i++) {
    Crab& c = *vis[i];
    Place cur = placeNow(c, now);
    applyPlace(cur);
    int z = n >= 2 ? seats[i].z : 0;
    slots[i] = {&c, z, U, OX, OY};
    if (moving(c, now)) continue;
    uint16_t color;
    String l = stateLabel(c, color);
    if (n == 1) {
      drawText(l, w / 2, h - 2, bottom_center, color, 2, w - 8);
      drawText(c.name, 4, 4, top_left, C_DIM, 1, w / 2);
    } else {
      int cx = (int)cur.cx;
      if (z == 0) drawText(l, cx, (int)cur.fy + U + 4, top_center, color, 2, 20 * U);
      else drawText(l, cx, h - 3, bottom_center, color, 2, 20 * U);
      drawText(c.name, cx, OY - 2, bottom_center, C_DIM, 1, 14 * U);
    }
  }
  drawClock();
  prevN = n;
  canvas.pushSprite(0, 0);
}

// ---------- 状态 ----------

void setCrabState(Crab& c, State s, const String& l, uint32_t now) {
  if (s != c.state) {
    c.prev = c.state;
    c.since = now;
    if (s == WAITING) beepPending = true;
  }
  if (s == THINKING || s == WORKING) {
    if (!c.busySince) c.busySince = now;
  } else if (s != WAITING) {
    c.busySince = 0;
  }
  c.state = s;
  c.label = l;
}

void startReaction(Crab& c, Reaction r, uint32_t now) {
  c.react = r;
  c.reactSince = now;
  c.reactUntil = now + REACTION_MS[r];
  if (&c == &pet) antic = A_NONE;
}

void wakePet(uint32_t now) {
  if (pet.state != SLEEP) return;
  setCrabState(pet, IDLE, "", now);
  antic = A_NONE;
  nextAnticAt = now + 4000;
  int h = localHour(), day = localYDay();
  if (h >= 5 && h < 11 && day != greetDay) {  // 每天早上第一次醒来打招呼
    greetDay = day;
    pet.label = "good morning!";
    greetUntil = now + 8000;
  }
}

Crab* findSession(const String& sid) {
  for (auto& c : sessions) if (c.used && c.sid == sid) return &c;
  return nullptr;
}

Crab* newSession(const String& sid, uint32_t now) {
  Crab* pick = nullptr;
  for (auto& c : sessions) if (!c.used) { pick = &c; break; }
  if (!pick) {  // 满了：挤掉没在显示、最久没消息的那个
    for (auto& c : sessions) {
      if (!pick) { pick = &c; continue; }
      bool vc = visibleState(c.state), vp = visibleState(pick->state);
      if (vc != vp ? !vc : c.lastMsg < pick->lastMsg) pick = &c;
    }
  }
  *pick = Crab();
  pick->used = true;
  pick->sid = sid;
  pick->since = now;
  return pick;
}

// 录像模式：时钟冻住，每次 GET /frame 往前走 40ms 画一帧并返回，录宣传视频不受截屏速度限制
bool recMode = false;
uint32_t recClock = 0;
uint32_t clk() { return recMode ? recClock : millis(); }

void handleState() {
  uint32_t now = clk();
  String s = server.arg("s");
  int st = -1;
  for (int i = 0; i <= SLEEP; i++) if (s == STATE_NAMES[i]) st = i;
  if (st < 0) {
    server.send(400, "text/plain", "unknown state");
    return;
  }
  String sid = server.arg("sid");
  if (sid.isEmpty()) sid = "default";
  uint64_t stamp = server.hasArg("t") ? strtoull(server.arg("t").c_str(), nullptr, 10) : 0;

  Crab* c = findSession(sid);
  if (c && stamp && stamp < c->lastStamp) {  // 异步 hook 可能乱序到达，丢掉旧的
    server.send(200, "text/plain", "stale");
    return;
  }
  lastActivity = now;
  if (!greetUntil) pet.label = "";  // 开机显示的地址，收到第一条消息就撤掉

  if (st != SLEEP) wakePet(now);
  if (!c) c = newSession(sid, now);
  if (stamp) c->lastStamp = stamp;
  c->lastMsg = now;
  if (server.hasArg("name")) c->name = server.arg("name");
  // 会话结束也留着槽位（不显示），挡住结束后才到的旧消息，过一阵自动忘掉
  setCrabState(*c, (State)st, server.arg("label"), now);

  if (st == SLEEP) {
    bool alive = false;
    for (auto& o : sessions) if (o.used && o.state != SLEEP) alive = true;
    if (!alive) setCrabState(pet, SLEEP, "", now);
  }
  server.send(200, "text/plain", "ok");
}

String jsonEsc(const String& s) {
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    char ch = s[i];
    if ((uint8_t)ch < 0x20) continue;
    if (ch == '"' || ch == '\\') o += '\\';
    o += ch;
  }
  return o;
}

void handleRoot() {
  Crab* vis[MAX_VISIBLE];
  int n = visibleSessions(vis);
  Crab* top = &pet;
  for (int i = 0; i < n; i++) if (priority(vis[i]->state) > priority(top->state)) top = vis[i];
  String j = String("{\"state\":\"") + STATE_NAMES[top->state] + "\",\"label\":\"" + jsonEsc(top->label) + "\"";
  struct tm tm;
  if (getLocalTime(&tm, 0)) {
    char buf[8];
    strftime(buf, sizeof buf, "%H:%M", &tm);
    j += String(",\"time\":\"") + buf + "\"";
  }
  if (wxCode >= 0)
    j += String(",\"weather\":{\"code\":") + wxCode + ",\"temp\":" + String(wxTemp, 1) + ",\"shown\":\"" +
         WEATHER_NAMES[weather()] + "\"}";
  j += ",\"sessions\":[";
  bool first = true;
  for (auto& c : sessions) {
    if (!c.used) continue;
    if (!first) j += ",";
    first = false;
    j += "{\"sid\":\"" + jsonEsc(c.sid) + "\",\"name\":\"" + jsonEsc(c.name) + "\",\"state\":\"" +
         STATE_NAMES[c.state] + "\",\"label\":\"" + jsonEsc(c.label) + "\"}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleTest() {
  uint32_t now = clk();
  if (server.hasArg("hour")) {
    float hr = server.arg("hour").toFloat();
    minuteOverride = hr < 0 ? -1 : (int)(hr * 60) % 1440;
  }
  if (server.hasArg("weather")) {
    wxOverride = -1;
    for (int i = 0; i < W_COUNT; i++) if (server.arg("weather") == WEATHER_NAMES[i]) wxOverride = i;
  }
  if (server.arg("event") == "bolt") boltNext = now;  // 下一帧就劈（只在雷暴时）
  if (server.hasArg("event")) {
    for (int i = 0; i < X_COUNT; i++)
      if (server.arg("event") == EXTRA_NAMES[i]) startExtra((Extra)i, now);
  }
  if (server.hasArg("antic")) {
    for (int i = 1; i <= A_YAWN; i++) {
      if (server.arg("antic") == ANTIC_NAMES[i]) {
        wakePet(now);
        lastActivity = now;
        antic = (Antic)i;
        anticSince = now;
      }
    }
  }
  if (server.hasArg("react")) {
    int i = server.arg("i").toInt();
    Crab* c = i < slotCount ? slots[i].crab : &pet;
    for (int r = 1; r <= R_WAVE_R; r++)
      if (server.arg("react") == REACTION_NAMES[r]) startReaction(*c, (Reaction)r, now);
  }
  if (server.hasArg("busy")) {
    uint32_t ms = server.arg("busy").toInt() * 60000u;
    for (auto& c : sessions) if (c.used && c.busySince) c.busySince = now - ms;
  }
  server.send(200, "text/plain", "ok");
}

// 截屏：把画布转成 24 位 BMP，调试动画用
void handleShot() {
  int w = canvas.width(), h = canvas.height();
  uint32_t rowSize = (w * 3 + 3) & ~3u;
  uint32_t size = 54 + rowSize * h;
  uint8_t hdr[54] = {'B', 'M'};
  auto le32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; i++) hdr[off + i] = v >> (8 * i); };
  le32(2, size); le32(10, 54); le32(14, 40); le32(18, w); le32(22, h);
  hdr[26] = 1; hdr[28] = 24;
  le32(34, rowSize * h);

  server.setContentLength(size);
  server.send(200, "image/bmp", "");
  server.sendContent((const char*)hdr, 54);

  static uint8_t buf[8 * 1024];
  const uint16_t* pix = (const uint16_t*)canvas.getBuffer();
  uint32_t used = 0;
  for (int y = h - 1; y >= 0; y--) {  // BMP 从下往上存
    uint8_t* row = buf + used;
    memset(row, 0, rowSize);
    for (int x = 0; x < w; x++) {
      uint16_t v = pix[y * w + x];
      v = (v >> 8) | (v << 8);  // 画布里是大端 RGB565
      uint8_t r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;
      row[x * 3] = (b << 3) | (b >> 2);
      row[x * 3 + 1] = (g << 2) | (g >> 4);
      row[x * 3 + 2] = (r << 3) | (r >> 2);
    }
    used += rowSize;
    if (used + rowSize > sizeof buf || y == 0) {
      server.sendContent((const char*)buf, used);
      used = 0;
    }
  }
}

void step(uint32_t now);

// GET /rec?on=1|0 进出录像模式；GET /frame?n=<帧数> 走 n 帧后返回画布，格式是一串 [次数][RGB565 大端两字节] 的游程
void handleRec() {
  recMode = server.arg("on") == "1";
  recClock = millis();
  server.send(200, "text/plain", "ok");
}

void handleFrame() {
  if (!recMode) {
    server.send(409, "text/plain", "not recording");
    return;
  }
  int n = max(1, (int)server.arg("n").toInt());
  for (int i = 0; i < n; i++) {
    recClock += 40;
    step(recClock);
  }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/octet-stream", "");
  static uint8_t buf[6 * 1024];
  const uint16_t* pix = (const uint16_t*)canvas.getBuffer();
  int total = canvas.width() * canvas.height(), used = 0;
  for (int i = 0; i < total;) {
    uint16_t v = pix[i];
    int run = 1;
    while (i + run < total && run < 255 && pix[i + run] == v) run++;
    buf[used++] = run;
    memcpy(buf + used, &v, 2);
    used += 2;
    i += run;
    if (used > (int)sizeof buf - 3 || i == total) {
      server.sendContent((const char*)buf, used);
      used = 0;
    }
  }
  server.sendContent("");
}

// ---------- 输入和定时 ----------

Crab* touchTarget = nullptr;
bool touchConsumed = false;

// 点在谁身上就是谁（重叠时前排优先），都没点中就算离得最近的那只
Slot* slotAt(int x, int y) {
  Slot* best = nullptr;
  long bestD = LONG_MAX;
  for (int i = 0; i < slotCount; i++) {
    Slot& s = slots[i];
    bool inside = x >= s.ox && x < s.ox + 20 * s.u && y >= s.oy && y < s.oy + 14 * s.u;
    long d = abs(x - (s.ox + 10 * s.u)) - (inside ? 100000L * (s.z + 1) : 0);
    if (d < bestD) { bestD = d; best = &s; }
  }
  return best;
}

Reaction hitTest(const Slot& s, int x, int y) {
  if (x < s.ox || y < s.oy) return R_NONE;
  int gx = (x - s.ox) / s.u, gy = (y - s.oy) / s.u;
  if (gy >= 4 && gy <= 7 && ((gx >= 6 && gx <= 8) || (gx >= 11 && gx <= 13))) return R_BLINK;
  if (gy >= 4 && gy <= 8 && gx >= 1 && gx <= 3) return R_WAVE_L;
  if (gy >= 4 && gy <= 8 && gx >= 16 && gx <= 18) return R_WAVE_R;
  if (gx >= 4 && gx <= 15 && gy >= 2 && gy <= 12) return R_SHY;
  return R_NONE;
}

void handleTouch(uint32_t now) {
  if (!M5.Touch.isEnabled()) return;
  auto d = M5.Touch.getDetail();
  if (d.wasPressed()) {
    lastActivity = now;
    Slot* s = slotAt(d.x, d.y);
    touchTarget = s ? s->crab : &pet;
    touchConsumed = false;
    if (touchTarget == &pet && pet.state == SLEEP) {
      wakePet(now);
      touchConsumed = true;
    } else if (touchTarget->state == WAITING || touchTarget->state == DONE) {  // 确认提醒
      setCrabState(*touchTarget, IDLE, "", now);
      touchConsumed = true;
    }
  }
  if (touchConsumed || !touchTarget) return;

  if (d.wasHold()) startReaction(*touchTarget, R_TICKLE, now);  // 长按挠痒痒
  if (d.isHolding() && touchTarget->react == R_TICKLE)
    touchTarget->reactUntil = now + REACTION_MS[R_TICKLE];

  if (d.wasClicked()) {
    Crab& c = *touchTarget;
    if (now - c.tapWindow > 2000) { c.tapWindow = now; c.taps = 0; }
    if (++c.taps >= 4) {           // 2 秒内连点 4 下就生气
      c.taps = 0;
      startReaction(c, R_ANGRY, now);
    } else if (c.react != R_ANGRY) {
      Slot* s = slotAt(d.x, d.y);
      Reaction r = s ? hitTest(*s, d.x, d.y) : R_NONE;
      if (r != R_NONE) startReaction(c, r, now);
    }
  }
}

void updateTimers(uint32_t now) {
  for (auto& c : sessions) {
    if (!c.used) continue;
    uint32_t quiet = now - c.lastMsg;
    if (c.state == DONE && now - c.since > DONE_TIMEOUT_MS) {
      setCrabState(c, IDLE, "", now);
      lastActivity = now;
    }
    if ((c.state == THINKING || c.state == WORKING) && quiet > STALE_TIMEOUT_MS) setCrabState(c, IDLE, "", now);
    if (!visibleState(c.state) && quiet > FORGET_AFTER_MS) c.used = false;
  }
  if (greetUntil && now >= greetUntil) {
    greetUntil = 0;
    pet.label = "";
  }
  bool shown = !anyVisible();
  if (shown && pet.state == IDLE && now - lastActivity > SLEEP_AFTER_MS) setCrabState(pet, SLEEP, "", now);

  uint8_t b = shown && pet.state == SLEEP ? DIM : BRIGHT;
  if (b != brightness) {
    brightness = b;
    M5.Display.setBrightness(b);
  }
}

// 闲着时随机眨眼、隔一阵做个小动作；深夜多打哈欠
void updateIdleLife(uint32_t now) {
  if (now >= nextBlinkAt) {
    blinkUntil = now + 140;
    nextBlinkAt = now + random(2000, 6500);
  }
  bool petShown = !anyVisible() && pet.state == IDLE;
  if (antic != A_NONE && (!petShown || now - anticSince > ANTIC_MS[antic])) {
    antic = A_NONE;
    nextAnticAt = now + random(5000, 20000);
  }
  if (antic == A_NONE && petShown && now >= nextAnticAt && pet.react == R_NONE && now - pet.since > 1500) {
    static const Antic day[] = {A_LOOK, A_WAVE, A_BUBBLES, A_WALK};
    static const Antic night[] = {A_YAWN, A_YAWN, A_LOOK, A_BUBBLES};
    antic = isNight() ? night[random(4)] : day[random(4)];
    anticSince = now;
  }
}

// 走 Wi-Fi 更新固件时屏幕上显示进度
void drawOtaProgress(int pct) {
  static int last = -1;
  if (pct == last) return;
  last = pct;
  int w = canvas.width(), h = canvas.height();
  M5.Display.setBrightness(BRIGHT);
  canvas.fillScreen(C_BG);
  placeCrab(0, 0, w, h - 40);
  Pose p;
  p.eyes = EYES_UP;
  p.armL = p.armR = -2;
  drawClawd(p);
  canvas.fillRect(40, h - 30, w - 80, 8, C_SHADOW);
  canvas.fillRect(40, h - 30, (w - 80) * pct / 100, 8, C_OK);
  drawText("updating " + String(pct) + "%", w / 2, h - 4, bottom_center, C_TEXT, 2, w);
  canvas.pushSprite(0, 0);
}

void setupOTA() {
  ArduinoOTA.setHostname(HOSTNAME);
#ifdef OTA_PASS
  ArduinoOTA.setPassword(OTA_PASS);
#endif
  ArduinoOTA.onStart([]() { drawOtaProgress(0); });
  ArduinoOTA.onProgress([](unsigned done, unsigned total) { drawOtaProgress(done * 100 / total); });
  ArduinoOTA.begin();
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  canvas.setTextSize(2);
  placeCrab(0, 0, canvas.width(), canvas.height() - canvas.fontHeight() - 4);
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    canvas.fillScreen(C_BG);
    Pose p;
    p.eyes = (millis() / 300) % 2 ? EYES_UP : EYES_OPEN;
    drawClawd(p);
    drawText("connecting Wi-Fi", canvas.width() / 2, canvas.height() - 2, bottom_center, C_DIM, 2, canvas.width());
    canvas.pushSprite(0, 0);
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    configTzTime(TZ_INFO, NTP_SERVER, "pool.ntp.org", "time.cloudflare.com");
    setupOTA();
#ifdef WEATHER_LAT
    xTaskCreate(weatherTask, "weather", 8192, nullptr, 1, nullptr);
#endif
    pet.label = String(HOSTNAME) + ".local";
  } else {
    pet.label = "Wi-Fi failed";
  }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  if (M5.Display.width() < M5.Display.height()) M5.Display.setRotation(1);  // 竖屏设备转成横屏
  M5.Display.setBrightness(BRIGHT);
  brightness = BRIGHT;

  canvas.setColorDepth(16);
  canvas.setPsram(true);
  canvas.createSprite(M5.Display.width(), M5.Display.height());

  C_BG      = canvas.color565(24, 22, 20);
  C_BODY    = canvas.color565(217, 119, 87);   // Claude 橙
  C_EYE     = canvas.color565(20, 20, 20);
  C_SHADOW  = canvas.color565(48, 44, 40);
  C_TEXT    = canvas.color565(236, 230, 220);
  C_DIM     = canvas.color565(130, 122, 112);
  C_ALERT   = canvas.color565(255, 196, 64);
  C_OK      = canvas.color565(120, 200, 120);
  C_BLUSH   = canvas.color565(255, 150, 165);
  C_ANGRY   = canvas.color565(220, 64, 48);
  C_CAP     = canvas.color565(90, 110, 210);
  C_KEY     = canvas.color565(70, 66, 62);
  C_KEYCAP  = canvas.color565(160, 152, 142);
  C_PENCIL  = canvas.color565(240, 200, 80);
  C_BRASS   = canvas.color565(196, 154, 72);
  C_BRASS_D = canvas.color565(140, 104, 44);
  C_GLASS   = canvas.color565(140, 200, 255);
  C_STEAM   = canvas.color565(180, 174, 168);
  C_INK     = C_TEXT;
  C_FIRE    = canvas.color565(255, 130, 40);
  C_FLAME   = canvas.color565(255, 220, 90);
  C_LOG     = canvas.color565(110, 70, 40);
  C_LOG_D   = canvas.color565(70, 45, 25);

  randomSeed(esp_random());
  connectWiFi();

  server.on("/", handleRoot);
  server.on("/state", handleState);
  server.on("/test", handleTest);
  server.on("/shot", handleShot);
  server.on("/rec", handleRec);
  server.on("/frame", handleFrame);
  server.begin();

  uint32_t now = millis();
  pet.since = lastActivity = now;
  nextAnticAt = now + 5000;
}

void step(uint32_t now) {
  updateTimers(now);
  updateIdleLife(now);
  updateExtras(now);
  updateWeather(now);
  render(now);
}

void loop() {
  M5.update();
  ArduinoOTA.handle();
  server.handleClient();
  if (recMode) {  // 录像时只由 /frame 推进
    delay(2);
    return;
  }
  uint32_t now = millis();

  if (beepPending) {
    beepPending = false;
    if (M5.Speaker.isEnabled()) M5.Speaker.tone(1800, 120);
  }

  // 实体按键：确认所有提醒并叫醒
  if (M5.BtnA.wasPressed()) {
    for (auto& c : sessions)
      if (c.used && (c.state == WAITING || c.state == DONE)) setCrabState(c, IDLE, "", now);
    wakePet(now);
    lastActivity = now;
  }

  handleTouch(now);
  step(now);
  delay(40);
}
