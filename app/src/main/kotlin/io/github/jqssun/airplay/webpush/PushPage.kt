package io.github.jqssun.airplay.webpush

/**
 * The page the phone opens after scanning the QR code on the TV.
 *
 * Kept as one inline string on purpose: no web framework, no asset pipeline, no extra download
 * step at runtime. Everything it needs is a form field, three buttons and a 1 s state poll.
 */
internal fun pushPage(): String = """
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>推送视频到电视</title>
<style>
:root{color-scheme:dark}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;min-height:100dvh;background:#111318;color:#e8eaed;
font:15px/1.55 -apple-system,system-ui,"PingFang SC","Microsoft YaHei",sans-serif;
display:flex;justify-content:center;padding:24px 18px calc(24px + env(safe-area-inset-bottom))}
main{width:100%;max-width:460px}
h1{font-size:19px;margin:0 0 6px;font-weight:600}
.sub{color:#9aa0a6;font-size:13px;margin:0 0 22px}
.card{background:#1a1d24;border:1px solid #2a2f3a;border-radius:14px;padding:16px;margin-bottom:14px}
input{width:100%;background:#0f1218;border:1px solid #2f3540;color:#e8eaed;border-radius:10px;
padding:12px 13px;font-size:15px;outline:none}
input:focus{border-color:#4c8dff}
button{border:0;border-radius:10px;padding:12px 16px;font-size:15px;font-weight:600;color:#fff;
background:#4c8dff;cursor:pointer}
button:active{opacity:.8}
.ghost{background:#262b35;color:#cfd4dc}
.row{display:flex;gap:10px;margin-top:12px}
.grow{flex:1}
.bar{height:6px;background:#2a2f3a;border-radius:3px;overflow:hidden;margin:12px 0 8px}
.bar i{display:block;height:100%;width:0;background:#4c8dff;transition:width .3s linear}
.meta{display:flex;justify-content:space-between;color:#9aa0a6;font-size:12px}
.title{font-size:14px;font-weight:600;margin:0 0 6px;word-break:break-all}
.hint{color:#7c828c;font-size:12px;margin:10px 2px 0;line-height:1.5}
#msg{color:#ffb4a2;font-size:13px;margin-top:10px;min-height:18px}
</style>
</head>
<body>
<main>
<h1>推送视频到电视</h1>
<p class="sub">粘贴视频直链或直播流地址，电视立即播放</p>
<div class="card">
<input id="url" type="url" inputmode="url" autocomplete="off" autocapitalize="off"
spellcheck="false" placeholder="https://…/video.mp4、rtsp://…、rtmp://…">
<div class="row"><button id="go" class="grow">播放</button></div>
<div id="msg"></div>
</div>
<div class="card" id="now" style="display:none">
<p class="title" id="t"></p>
<div class="bar"><i id="p"></i></div>
<div class="meta"><span id="pos">0:00</span><span id="dur">0:00</span></div>
<div class="row">
<button id="toggle" class="grow">暂停</button>
<button id="stop" class="ghost">停止</button>
</div>
</div>
<p class="hint">支持 mp4 / m3u8 等媒体直链，以及 rtsp（监控摄像头，可写成 rtsp://账号:密码@192.168.0.9:554/live）、rtmp（OBS / nginx-rtmp，形如 rtmp://192.168.0.9:1935/live/串流名）、rtp 与 udp（MPEG-TS 组播，形如 udp://@239.1.1.1:1234，必须带端口）。rtsps 暂不支持。网页链接（含视频网站分享页）无法直接播放。</p>
</main>
<script>
var K = '';
try { K = new URLSearchParams(location.search).get('k') || ''; } catch (e) { K = ''; }
var playing = false;

function q(path) { return path + '?k=' + encodeURIComponent(K); }
function post(path, body) {
  return fetch(q(path), {
    method: 'POST',
    body: body || '',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' }
  });
}
function fmt(s) {
  s = Math.max(0, Math.floor(s || 0));
  var m = Math.floor(s / 60), r = s % 60;
  return m + ':' + (r < 10 ? '0' : '') + r;
}
function msg(t) { document.getElementById('msg').textContent = t; }

document.getElementById('go').onclick = function () {
  var v = document.getElementById('url').value.trim();
  if (!v) { msg('请先粘贴视频链接'); return; }
  msg('发送中…');
  post('/api/play', 'url=' + encodeURIComponent(v)).then(function (r) {
    if (r.ok) {
      msg('');
      document.getElementById('url').value = '';
      refresh();
    } else {
      r.text().then(function (t) { msg(t || '播放失败'); });
    }
  }).catch(function () { msg('连接不上电视，请检查是否同一 Wi-Fi'); });
};

function ctl(a, p) {
  return post('/api/ctl', 'a=' + a + (p == null ? '' : ('&p=' + p)));
}
document.getElementById('toggle').onclick = function () {
  ctl(playing ? 'pause' : 'resume').then(refresh);
};
document.getElementById('stop').onclick = function () { ctl('stop').then(refresh); };

function refresh() {
  fetch(q('/api/state')).then(function (r) { return r.json(); }).then(function (s) {
    var now = document.getElementById('now');
    if (!s || !s.active) { now.style.display = 'none'; return; }
    playing = !!s.playing;
    now.style.display = 'block';
    document.getElementById('t').textContent = s.title || s.url;
    document.getElementById('pos').textContent = fmt(s.pos);
    // an RTSP/HLS live stream reports no duration at all; "LIVE" beats a stuck 0:00
    document.getElementById('dur').textContent = s.dur > 0 ? fmt(s.dur) : (s.pos > 0 ? 'LIVE' : '0:00');
    document.getElementById('p').style.width =
      (s.dur > 0 ? Math.min(100, s.pos / s.dur * 100) : 0) + '%';
    document.getElementById('toggle').textContent = playing ? '暂停' : '继续';
  }).catch(function () {});
}
refresh();
setInterval(refresh, 1000);
</script>
</body>
</html>
""".trimIndent()
