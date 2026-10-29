import dev, os, sys, json
from dev import state, test, frame, get
OUT = os.path.dirname(os.path.abspath(__file__)) + "/frames"
DEMO = ["d0a1", "d0a2", "d0a3"]

def clear():
    for s in json.loads(get("/"))["sessions"]:
        state(s["sid"], "sleep")
    for s in DEMO: state(s, "sleep")
    test(antic="look"); frame(5)

def scene(name, n, events, setup=None):
    os.makedirs(f"{OUT}/{name}", exist_ok=True)
    clear()
    if setup: setup()
    for i in range(n):
        for f in events.get(i, []): f()
        frame().save(f"{OUT}/{name}/{i:04d}.png")
    print(name, n, flush=True)

E = lambda **kw: (lambda: test(**kw))
St = lambda *a: (lambda: state(*a))

get("/rec?on=1")
get("/rec?on=1")
SCENES = {
 # 宠物：夜里，打哈欠、吐泡泡
 "pet": (110, {0: [E(antic="yawn")], 60: [E(antic="bubbles")]},
         lambda: (test(hour=22.6, weather="clear"), [frame(5) for _ in range(3)])),
 # 一个会话的一生：thinking → working → waiting → done，白天
 "flow": (300, {
     0:   [St("d0a1", "thinking", "my-app")],
     55:  [St("d0a1", "working", "my-app", "Bash")],
     115: [St("d0a1", "working", "my-app", "Edit")],
     170: [St("d0a1", "waiting", "my-app", "npm run migrate")],
     240: [St("d0a1", "done", "my-app")],
  }, lambda: (test(hour=10.5, weather="clear"), frame(20))),
 # 三个会话围着篝火，一只只走进来
 "multi": (190, {
     0:  [St("d0a1", "working", "my-app", "Bash")],
     40: [St("d0a2", "thinking", "api")],
     85: [St("d0a3", "waiting", "docs", "plan ready")],
  }, lambda: (test(hour=18.2, weather="clear"), frame(10))),
 # 一天：清晨 → 白天 → 傍晚 → 夜里（宠物）
 "day": (200, {0: [E(hour=5.6)], 50: [E(hour=12)], 100: [E(hour=18)], 150: [E(hour=21.5)]},
         lambda: (test(weather="clear"), frame(5))),
 # 天气：雨 → 雪 → 雷暴
 "weather": (225, {0: [E(weather="rain", hour=14)], 75: [E(weather="snow")], 150: [E(weather="storm", hour=20)],
                   165: [E(event="bolt")], 205: [E(event="bolt")]}, None),
 # 摸一摸
 "touch": (170, {10: [E(react="shy", i=0)], 60: [E(react="tickle", i=0)], 115: [E(react="angry", i=0)]},
           lambda: (test(hour=16, weather="clear"), frame(30))),
 # 小东西：帆船、海鸥
 "extras": (150, {0: [E(event="boat")], 20: [E(event="gull")]},
            lambda: (test(hour=8, weather="clear"), frame(5))),
}
only = sys.argv[1:]
for name, (n, ev, setup) in SCENES.items():
    if only and name not in only: continue
    scene(name, n, ev, setup)
