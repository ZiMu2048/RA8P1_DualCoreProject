#!/usr/bin/env python3
"""DA16200 TCP receiver and local monitoring dashboard."""

from __future__ import annotations

import json
import os
import socket
import struct
import threading
from collections import deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


TCP_HOST = os.getenv("DA16200_TCP_HOST", "0.0.0.0")
TCP_PORT = int(os.getenv("DA16200_TCP_PORT", "5000"))
WEB_HOST = os.getenv("DA16200_WEB_HOST", "0.0.0.0")
WEB_PORT = int(os.getenv("DA16200_WEB_PORT", "8000"))
MAX_EVENTS = 100
RECV_SIZE = 4096
IMAGE_MAGIC = b"RJPG"
IMAGE_PROTOCOL_VERSION = 1
IMAGE_HEADER = struct.Struct("!4sBBHIHHIHH")
MAX_JPEG_SIZE = 64 * 1024
RECEIVE_DIR = Path(__file__).resolve().parent
ERROR_DIR = RECEIVE_DIR / "ERROR"
MAX_IMAGE_HISTORY = 24


def now_text() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


class ReceiverState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.connected = False
        self.peer = "--"
        self.connected_at = "--"
        self.total_bytes = 0
        self.packet_count = 0
        self.latest_text = "等待 RA8P1 发送数据..."
        self.latest_hex = "--"
        self.latest_at = "--"
        self.latest_image_name = ""
        self.latest_width = 0
        self.latest_height = 0
        self.latest_frame_id = 0
        self.latest_confidence_milli = 0
        self.images: deque[dict[str, Any]] = deque(maxlen=MAX_IMAGE_HISTORY)
        self.events: deque[dict[str, Any]] = deque(maxlen=MAX_EVENTS)

    def add_event(self, kind: str, message: str) -> None:
        self.events.appendleft({"time": now_text(), "kind": kind, "message": message})

    def set_connected(self, address: tuple[str, int]) -> None:
        with self.lock:
            self.connected = True
            self.peer = f"{address[0]}:{address[1]}"
            self.connected_at = now_text()
            self.add_event("connected", f"DA16200 已连接：{self.peer}")

    def set_disconnected(self, address: tuple[str, int]) -> None:
        with self.lock:
            self.connected = False
            self.add_event("disconnected", f"连接已断开：{address[0]}:{address[1]}")

    def add_image(
        self,
        frame_id: int,
        width: int,
        height: int,
        jpeg: bytes,
        filename: str,
        confidence_milli: int,
    ) -> None:
        marker_text = (
            f"{jpeg[0]:02X} {jpeg[1]:02X} ... "
            f"{jpeg[-2]:02X} {jpeg[-1]:02X}"
        )
        with self.lock:
            self.total_bytes += IMAGE_HEADER.size + len(jpeg)
            self.packet_count += 1
            self.latest_text = (
                f"JPEG frame={frame_id}, {width}x{height}, "
                f"{len(jpeg)} bytes"
            )
            self.latest_hex = marker_text
            self.latest_at = now_text()
            self.latest_image_name = filename
            self.latest_width = width
            self.latest_height = height
            self.latest_frame_id = frame_id
            self.latest_confidence_milli = confidence_milli
            self.images.appendleft(
                {
                    "frame_id": frame_id,
                    "filename": filename,
                    "width": width,
                    "height": height,
                    "confidence_milli": confidence_milli,
                    "received_at": self.latest_at,
                    "url": f"/error/{filename}?frame={frame_id}",
                }
            )
            self.add_event(
                "image",
                f"已保存 {filename}，{width}x{height}，{len(jpeg)} 字节",
            )

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            return {
                "connected": self.connected,
                "peer": self.peer,
                "connected_at": self.connected_at,
                "total_bytes": self.total_bytes,
                "packet_count": self.packet_count,
                "latest_text": self.latest_text,
                "latest_hex": self.latest_hex,
                "latest_at": self.latest_at,
                "latest_image_name": self.latest_image_name,
                "latest_image_url": (
                    f"/latest.jpg?frame={self.latest_frame_id}"
                    if self.latest_image_name
                    else ""
                ),
                "latest_width": self.latest_width,
                "latest_height": self.latest_height,
                "latest_confidence_milli": self.latest_confidence_milli,
                "images": list(self.images),
                "events": list(self.events),
                "tcp_port": TCP_PORT,
                "web_port": WEB_PORT,
            }

    def latest_image_path(self) -> Path | None:
        with self.lock:
            if not self.latest_image_name:
                return None
            return ERROR_DIR / self.latest_image_name


STATE = ReceiverState()


def recv_exact(client: socket.socket, size: int) -> bytes:
    """Receive exactly size bytes or raise when the peer disconnects."""
    data = bytearray()
    while len(data) < size:
        chunk = client.recv(min(RECV_SIZE, size - len(data)))
        if not chunk:
            raise ConnectionError(
                f"peer disconnected after {len(data)}/{size} bytes"
            )
        data.extend(chunk)
    return bytes(data)


def timestamp_filename() -> str:
    """Build a millisecond-resolution JPEG filename."""
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
    return f"ra8p1_{stamp}.jpg"


def handle_client(client: socket.socket, address: tuple[str, int]) -> None:
    STATE.set_connected(address)
    print(f"[{now_text()}] DA16200 connected: {address[0]}:{address[1]}", flush=True)
    try:
        with client:
            while True:
                header_data = recv_exact(client, IMAGE_HEADER.size)
                (
                    magic,
                    version,
                    header_size,
                    flags,
                    frame_id,
                    width,
                    height,
                    jpeg_length,
                    confidence_milli,
                    reserved,
                ) = IMAGE_HEADER.unpack(header_data)

                if magic != IMAGE_MAGIC:
                    raise ValueError(f"invalid image magic: {magic!r}")
                if version != IMAGE_PROTOCOL_VERSION:
                    raise ValueError(f"unsupported protocol version: {version}")
                if header_size != IMAGE_HEADER.size:
                    raise ValueError(f"invalid header size: {header_size}")
                if not (4 <= jpeg_length <= MAX_JPEG_SIZE):
                    raise ValueError(f"invalid JPEG length: {jpeg_length}")

                jpeg = recv_exact(client, jpeg_length)
                if jpeg[:2] != b"\xFF\xD8" or jpeg[-2:] != b"\xFF\xD9":
                    raise ValueError("invalid JPEG SOI/EOI markers")

                filename = timestamp_filename()
                destination = ERROR_DIR / filename
                destination.write_bytes(jpeg)
                STATE.add_image(
                    frame_id,
                    width,
                    height,
                    jpeg,
                    filename,
                    confidence_milli,
                )

                print(
                    f"[{now_text()}] JPEG frame={frame_id} "
                    f"{width}x{height} {jpeg_length} bytes -> {destination}",
                    flush=True,
                )
                if flags or confidence_milli or reserved:
                    print(
                        f"[{now_text()}] metadata flags={flags} "
                        f"confidence_milli={confidence_milli} reserved={reserved}",
                        flush=True,
                    )
    except (ConnectionError, OSError, ValueError) as exc:
        print(f"[{now_text()}] Client error: {exc}", flush=True)
    finally:
        STATE.set_disconnected(address)
        print(f"[{now_text()}] DA16200 disconnected: {address[0]}:{address[1]}", flush=True)


def run_tcp_server() -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((TCP_HOST, TCP_PORT))
        server.listen(8)
        print(f"[{now_text()}] TCP receiver listening on {TCP_HOST}:{TCP_PORT}", flush=True)
        while True:
            client, address = server.accept()
            thread = threading.Thread(
                target=handle_client,
                args=(client, address),
                daemon=True,
                name=f"tcp-client-{address[0]}-{address[1]}",
            )
            thread.start()


LEGACY_HTML = r"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>RA8P1 图像传输监视器</title>
  <style>
    :root{color-scheme:dark;--bg:#08111f;--panel:#101d30;--line:#243752;--text:#e8f0fb;--muted:#89a0bd;--cyan:#3bd8ff;--green:#42e6a4;--red:#ff7185}
    *{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 15% -10%,#15365d 0,transparent 38%),var(--bg);color:var(--text);font-family:Inter,"Segoe UI","Microsoft YaHei",sans-serif;min-height:100vh}
    header{max-width:1180px;margin:auto;padding:38px 24px 20px;display:flex;align-items:center;justify-content:space-between;gap:20px}.eyebrow{color:var(--cyan);font-size:12px;letter-spacing:.18em;font-weight:800}.title{font-size:clamp(28px,4vw,48px);margin:7px 0 5px;line-height:1.05}.subtitle{color:var(--muted);margin:0}.badge{display:flex;align-items:center;gap:9px;padding:10px 14px;border:1px solid var(--line);border-radius:999px;background:#0b1728;font-weight:700}.dot{width:10px;height:10px;border-radius:50%;background:var(--red);box-shadow:0 0 14px var(--red)}.badge.online .dot{background:var(--green);box-shadow:0 0 14px var(--green)}
    main{max-width:1180px;margin:auto;padding:8px 24px 50px}.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:16px}.card{background:linear-gradient(145deg,rgba(18,34,55,.94),rgba(10,24,41,.94));border:1px solid var(--line);border-radius:18px;padding:20px;box-shadow:0 18px 45px rgba(0,0,0,.2)}.stat{grid-column:span 3}.label{font-size:12px;color:var(--muted);letter-spacing:.08em;text-transform:uppercase}.value{font-size:25px;font-weight:800;margin-top:10px;word-break:break-all}.payload{grid-column:span 7;min-height:300px}.preview{grid-column:span 5;min-height:300px}.events{grid-column:span 12}.section-title{font-size:16px;font-weight:800;margin-bottom:14px}.mono{font-family:"Cascadia Mono",Consolas,monospace;background:#07111e;border:1px solid #1b304a;border-radius:12px;padding:16px;min-height:82px;white-space:pre-wrap;word-break:break-all;color:#bfeeff}.hex{color:#92aaca;font-size:13px;max-height:120px;overflow:auto}.image-placeholder{height:212px;border:1px dashed #315172;border-radius:14px;display:grid;place-items:center;text-align:center;color:var(--muted);background:repeating-linear-gradient(45deg,#0a1829,#0a1829 12px,#0c1b2e 12px,#0c1b2e 24px)}.event{display:grid;grid-template-columns:155px 95px 1fr;gap:12px;border-top:1px solid #1b3048;padding:12px 2px;font-size:14px}.event:first-child{border-top:0}.event .time{color:var(--muted)}.event .kind{color:var(--cyan);font-weight:700}.empty{color:var(--muted);padding:14px 0}
    .image-preview{width:100%;height:212px;object-fit:contain;border:1px solid #315172;border-radius:14px;background:#07111e}.image-preview[hidden]{display:none}
    @media(max-width:820px){.stat{grid-column:span 6}.payload,.preview{grid-column:span 12}.event{grid-template-columns:1fr}.event .time{font-size:12px}}@media(max-width:520px){header{align-items:flex-start;flex-direction:column}.stat{grid-column:span 12}}
  </style>
</head>
<body>
  <header><div><div class="eyebrow">RENESAS RA8P1 · DA16200</div><h1 class="title">边缘 AI 图像传输监视器</h1><p class="subtitle">TCP 数据接收、连接诊断与图像上传预览</p></div><div class="badge" id="badge"><span class="dot"></span><span id="statusText">等待设备</span></div></header>
  <main><div class="grid">
    <section class="card stat"><div class="label">TCP 对端</div><div class="value" id="peer">--</div></section>
    <section class="card stat"><div class="label">累计字节</div><div class="value" id="bytes">0</div></section>
    <section class="card stat"><div class="label">接收次数</div><div class="value" id="packets">0</div></section>
    <section class="card stat"><div class="label">最近接收</div><div class="value" id="latestAt">--</div></section>
    <section class="card payload"><div class="section-title">最近一次 TCP 数据</div><div class="label">UTF-8 / ASCII</div><div class="mono" id="text">等待 RA8P1 发送数据...</div><div class="label" style="margin-top:14px">HEX</div><div class="mono hex" id="hex">--</div></section>
    <section class="card preview"><div class="section-title">图像预览</div><div class="image-placeholder" id="imagePlaceholder">等待 RA8P1 上传 JPEG</div><img class="image-preview" id="previewImage" alt="最新 RA8P1 JPEG" hidden><p class="subtitle" id="imageInfo" style="margin-top:14px">尚未收到图像</p></section>
    <section class="card events"><div class="section-title">连接与接收日志</div><div id="events"><div class="empty">暂无事件</div></div></section>
  </div></main>
  <script>
    const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    let consecutiveFailures=0;
    function updatePreview(d){const img=document.getElementById('previewImage');const placeholder=document.getElementById('imagePlaceholder');const info=document.getElementById('imageInfo');if(d.latest_image_url){if(img.dataset.src!==d.latest_image_url){img.src=d.latest_image_url;img.dataset.src=d.latest_image_url;}img.hidden=false;placeholder.hidden=true;info.textContent=`${d.latest_image_name} · ${d.latest_width}×${d.latest_height}`;}else{img.hidden=true;placeholder.hidden=false;info.textContent='尚未收到图像';}}
    async function refresh(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);const d=await r.json();consecutiveFailures=0;const badge=document.getElementById('badge');badge.classList.toggle('online',d.connected);document.getElementById('statusText').textContent=d.connected?'设备在线':'等待设备';document.getElementById('peer').textContent=d.peer;document.getElementById('bytes').textContent=d.total_bytes.toLocaleString();document.getElementById('packets').textContent=d.packet_count;document.getElementById('latestAt').textContent=d.latest_at;document.getElementById('text').textContent=d.latest_text;document.getElementById('hex').textContent=d.latest_hex;document.getElementById('events').innerHTML=d.events.length?d.events.map(e=>`<div class="event"><span class="time">${esc(e.time)}</span><span class="kind">${esc(e.kind)}</span><span>${esc(e.message)}</span></div>`).join(''):'<div class="empty">暂无事件</div>';updatePreview(d);}catch(e){consecutiveFailures++;if(consecutiveFailures>=3){document.getElementById('badge').classList.remove('online');document.getElementById('statusText').textContent='网页服务异常';}}finally{setTimeout(refresh,1000);}}
    refresh();
  </script>
</body></html>"""


HTML = r"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>RA8P1 异常图像监控</title>
  <style>
    :root{color-scheme:dark;--bg:#07101d;--panel:#101d30;--line:#263b58;--text:#e8f0fb;--muted:#8da5c2;--cyan:#3bd8ff;--green:#42e6a4;--red:#ff7185}
    *{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 15% -10%,#15365d 0,transparent 38%),var(--bg);color:var(--text);font-family:"Segoe UI","Microsoft YaHei",sans-serif;min-height:100vh}
    header,main{max-width:1180px;margin:auto;padding:28px 24px}header{display:flex;align-items:center;justify-content:space-between;gap:20px}.eyebrow{color:var(--cyan);font-size:12px;letter-spacing:.18em;font-weight:800}h1{margin:6px 0}.muted{color:var(--muted)}
    .badge{padding:10px 14px;border:1px solid var(--line);border-radius:999px;background:#0b1728;font-weight:700}.badge::before{content:"";display:inline-block;width:10px;height:10px;margin-right:9px;border-radius:50%;background:var(--red)}.badge.online::before{background:var(--green)}
    .grid{display:grid;grid-template-columns:repeat(12,1fr);gap:16px}.card{background:linear-gradient(145deg,#122237,#0a1829);border:1px solid var(--line);border-radius:18px;padding:18px}.stat{grid-column:span 3}.stat b{display:block;font-size:24px;margin-top:8px}.preview{grid-column:span 5}.detail{grid-column:span 7}.history{grid-column:span 12}.preview img{width:100%;height:300px;object-fit:contain;background:#050b12;border-radius:12px}.gallery{display:grid;grid-template-columns:repeat(auto-fill,minmax(170px,1fr));gap:14px}.item{border:1px solid var(--line);border-radius:12px;overflow:hidden;background:#081423}.item img{width:100%;aspect-ratio:1;object-fit:cover;display:block}.meta{padding:10px;font-size:13px}.event{padding:9px 0;border-bottom:1px solid var(--line)}
    @media(max-width:820px){.stat{grid-column:span 6}.preview,.detail{grid-column:span 12}}@media(max-width:520px){.stat{grid-column:span 12}header{align-items:flex-start;flex-direction:column}}
  </style>
</head>
<body>
  <header><div><div class="eyebrow">RENESAS RA8P1 · DA16200</div><h1>异常图像实时监控</h1><div class="muted">置信度超过阈值后自动接收并归档</div></div><div class="badge" id="badge">等待设备</div></header>
  <main><div class="grid">
    <section class="card stat"><span class="muted">TCP 对端</span><b id="peer">--</b></section>
    <section class="card stat"><span class="muted">累计字节</span><b id="bytes">0</b></section>
    <section class="card stat"><span class="muted">异常图像</span><b id="packets">0</b></section>
    <section class="card stat"><span class="muted">最新置信度</span><b id="confidence">--</b></section>
    <section class="card preview"><h2>最新异常图像</h2><img id="preview" alt="等待异常图像"><p class="muted" id="imageInfo">尚未收到图像</p></section>
    <section class="card detail"><h2>接收状态</h2><div id="latest">等待 RA8P1 发送异常图像...</div><h2>最近事件</h2><div id="events"></div></section>
    <section class="card history"><h2>ERROR 图像历史</h2><div class="gallery" id="gallery"><span class="muted">暂无异常图像</span></div></section>
  </div></main>
  <script>
    const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    let shownFrame=-1;
    async function refresh(){try{const r=await fetch('/api/status',{cache:'no-store'});const d=await r.json();const badge=document.getElementById('badge');badge.classList.toggle('online',d.connected);badge.textContent=d.connected?'设备在线':'等待设备';document.getElementById('peer').textContent=d.peer;document.getElementById('bytes').textContent=d.total_bytes.toLocaleString();document.getElementById('packets').textContent=d.packet_count;document.getElementById('confidence').textContent=d.latest_image_name?(d.latest_confidence_milli/1000).toFixed(3):'--';document.getElementById('latest').textContent=d.latest_text;if(d.latest_image_url&&shownFrame!==d.latest_image_url){document.getElementById('preview').src=d.latest_image_url;shownFrame=d.latest_image_url;}document.getElementById('imageInfo').textContent=d.latest_image_name?`${d.latest_image_name} · ${d.latest_width}×${d.latest_height}`:'尚未收到图像';document.getElementById('events').innerHTML=d.events.slice(0,8).map(e=>`<div class="event"><span class="muted">${esc(e.time)}</span> ${esc(e.message)}</div>`).join('');document.getElementById('gallery').innerHTML=d.images.length?d.images.map(i=>`<article class="item"><img src="${i.url}" alt="异常帧 ${i.frame_id}"><div class="meta"><b>置信度 ${(i.confidence_milli/1000).toFixed(3)}</b><br><span class="muted">${esc(i.received_at)}</span></div></article>`).join(''):'<span class="muted">暂无异常图像</span>';}catch(e){document.getElementById('badge').classList.remove('online');document.getElementById('badge').textContent='网页服务异常';}finally{setTimeout(refresh,500)}}refresh();
  </script>
</body></html>"""


class WebHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        if self.path == "/" or self.path.startswith("/?"):
            self.send_bytes(200, "text/html; charset=utf-8", HTML.encode("utf-8"))
            return
        if self.path == "/api/status":
            payload = json.dumps(STATE.snapshot(), ensure_ascii=False).encode("utf-8")
            self.send_bytes(200, "application/json; charset=utf-8", payload)
            return
        if self.path.startswith("/latest.jpg"):
            image_path = STATE.latest_image_path()
            if image_path is None or not image_path.is_file():
                self.send_bytes(404, "text/plain; charset=utf-8", b"No image")
                return
            self.send_bytes(200, "image/jpeg", image_path.read_bytes())
            return
        if self.path.startswith("/error/"):
            filename = self.path.split("?", 1)[0].removeprefix("/error/")
            image_path = ERROR_DIR / filename
            if filename != Path(filename).name or not image_path.is_file():
                self.send_bytes(404, "text/plain; charset=utf-8", b"No image")
                return
            self.send_bytes(200, "image/jpeg", image_path.read_bytes())
            return
        self.send_bytes(404, "text/plain; charset=utf-8", "Not found".encode("utf-8"))

    def send_bytes(self, status: int, content_type: str, payload: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format: str, *args: object) -> None:
        return


def main() -> None:
    ERROR_DIR.mkdir(parents=True, exist_ok=True)
    tcp_thread = threading.Thread(target=run_tcp_server, daemon=True, name="tcp-server")
    tcp_thread.start()
    web_server = ThreadingHTTPServer((WEB_HOST, WEB_PORT), WebHandler)
    print(f"[{now_text()}] Dashboard: http://127.0.0.1:{WEB_PORT}", flush=True)
    print("Press Ctrl+C to stop.", flush=True)
    try:
        web_server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping...", flush=True)
    finally:
        web_server.server_close()


if __name__ == "__main__":
    main()
