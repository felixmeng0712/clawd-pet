import os, urllib.request, numpy as np, time
from PIL import Image
HOST = "http://" + os.environ.get("CLAWD_HOST", "clawd.local")
def get(path):
    return urllib.request.urlopen(HOST + path, timeout=10).read()
def state(sid, s, name="", label=""):
    from urllib.parse import urlencode
    get("/state?" + urlencode(dict(s=s, sid=sid, name=name, label=label, t=int(time.time()*1000))))
def test(**kw):
    from urllib.parse import urlencode
    get("/test?" + urlencode(kw))
def frame(n=1):
    d = np.frombuffer(get(f"/frame?n={n}"), dtype=np.uint8).reshape(-1, 3)
    cnt = d[:, 0].astype(np.int64)
    v = (d[:, 1].astype(np.uint16) << 8) | d[:, 2]   # stored big-endian in buffer
    px = np.repeat(v, cnt)
    assert px.size == 320*240, px.size
    r = ((px >> 11) & 31) * 255 // 31; g = ((px >> 5) & 63) * 255 // 63; b = (px & 31) * 255 // 31
    return Image.fromarray(np.stack([r, g, b], -1).reshape(240, 320, 3).astype(np.uint8))
