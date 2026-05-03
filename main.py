"""
he thong nhung - AI Camera Server (v6 - WiFi TCP)
====================================================
Thay đổi so với v5:
- Thay Serial USB → TCP Socket WiFi (port 8888)
  ESP32 kết nối WiFi, giao tiếp với PC qua TCP
  → ESP32 dùng nguồn cục sạc dự phòng, không cần USB
"""

import cv2
import socket
import time
import threading
import queue as _queue
from ultralytics import YOLO
from collections import Counter

# ─── CẤU HÌNH ────────────────────────────────────────────────
TCP_HOST    = '0.0.0.0'   # Lắng nghe tất cả interface
TCP_PORT    = 8888
URL_CAMERA  = 1
MODEL_PATH  = 'best.pt'
NUM_FRAMES  = 8
MIN_VALID   = 3
CONF_THRESH = 0.75

CLASS_MAP  = {0: 'A', 1: 'B', 2: 'O', 3: 'M'}
CLASS_NAME = {'A': 'Apple', 'B': 'Banana', 'O': 'Orange', 'M': 'Milk', 'U': 'Unknown'}

# ─── Trạng thái chia sẻ ──────────────────────────────────────
_lock         = threading.Lock()
_cam_lock     = threading.Lock()
_scanning     = False
_last_result  = 'U'
_status_msg   = "READY"
_frozen_frame = None

def set_status(msg: str):
    global _status_msg
    with _lock:
        _status_msg = msg

def get_status() -> str:
    with _lock:
        return _status_msg

def is_scanning() -> bool:
    with _lock:
        return _scanning

# ─── TCPSerial Wrapper ───────────────────────────────────────
class TCPSerial:
    """Bắt chước giao diện serial.Serial nhưng dùng TCP socket."""

    def __init__(self, client_sock: socket.socket):
        self._sock   = client_sock
        self._closed = False
        self._rx_q   = _queue.Queue()
        self._wlock  = threading.Lock()
        threading.Thread(target=self._recv_loop, daemon=True).start()

    def _recv_loop(self):
        partial = b""
        self._sock.settimeout(0.2)
        while not self._closed:
            try:
                chunk = self._sock.recv(256)
                if not chunk:
                    break
                partial += chunk
                while b'\n' in partial:
                    line, partial = partial.split(b'\n', 1)
                    self._rx_q.put(line.strip().decode('utf-8', errors='ignore'))
            except socket.timeout:
                continue
            except Exception:
                break
        self._closed = True

    @property
    def in_waiting(self) -> int:
        return self._rx_q.qsize()

    def readline(self) -> bytes:
        try:
            line = self._rx_q.get(timeout=0.1)
            return (line + '\n').encode()
        except _queue.Empty:
            return b""

    def write(self, data: bytes):
        with self._wlock:
            if self._closed:
                raise ConnectionError("TCP connection closed")
            try:
                self._sock.sendall(data)
            except Exception as e:
                self._closed = True
                raise ConnectionError(f"TCP write error: {e}")

    def close(self):
        self._closed = True
        try:
            self._sock.close()
        except Exception:
            pass

    @property
    def is_open(self) -> bool:
        return not self._closed

# ─── TCP Server ──────────────────────────────────────────────
def init_tcp_server(host: str, port: int) -> socket.socket:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    print(f"[TCP] Server lang nghe tai port {port}")
    print(f"[TCP] IP may tinh: 192.168.1.188")
    return srv

def wait_for_esp32(srv: socket.socket) -> 'TCPSerial | None':
    print("[TCP] Dang cho ESP32 ket noi WiFi...")
    srv.settimeout(None)  # Block cho đến khi có kết nối
    try:
        cli, addr = srv.accept()
        cli.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f"[TCP] ESP32 da ket noi tu {addr}")
        return TCPSerial(cli)
    except Exception as e:
        print(f"[TCP] Loi accept: {e}")
        return None

# ─── Thread quét sản phẩm ────────────────────────────────────
def analyze_thread(cap: cv2.VideoCapture, model: YOLO, esp32: TCPSerial):
    global _scanning, _last_result

    with _lock:
        _scanning = True

    predictions = []
    print(f"[AI] Bat dau quet {NUM_FRAMES} frames...")

    with _cam_lock:
        for _ in range(3):
            cap.grab()

        for i in range(NUM_FRAMES):
            ret, frame = cap.read()
            if not ret:
                print(f"[AI] Canh bao: Khong doc duoc frame {i+1}")
                continue

            frame = cv2.resize(frame, (640, 480))
            try:
                results = model(frame, conf=CONF_THRESH, verbose=False)
            except Exception as e:
                print(f"[AI] LOI model frame {i+1}: {e}")
                continue

            set_status(f"SCANNING {i+1}/{NUM_FRAMES}")

            if len(results[0].boxes) > 0:
                try:
                    class_id = int(results[0].boxes.cls[0])
                    predictions.append(class_id)
                except (IndexError, ValueError):
                    predictions.append(-1)
            else:
                predictions.append(-1)

    valid = [p for p in predictions if p != -1]

    if len(valid) < MIN_VALID:
        result = 'U'
        print(f"[AI] Chi co {len(valid)}/{NUM_FRAMES} frame hop le -> Unknown")
    else:
        most_common_id = Counter(valid).most_common(1)[0][0]
        result = CLASS_MAP.get(most_common_id, 'U')
        confidence = valid.count(most_common_id) / len(valid) * 100
        print(f"[AI] Ket qua: {CLASS_NAME[result]} ({confidence:.0f}% trong {len(valid)} frame)")

    try:
        esp32.write(result.encode())
        print(f"[TCP] Gui ket qua: '{result}' ({CLASS_NAME[result]})")
    except ConnectionError as e:
        print(f"[TCP] LOI khi gui: {e}")

    set_status(f"SENT: {CLASS_NAME[result]}")
    with _lock:
        _last_result = result
        _scanning = False

# ─── Vẽ overlay ──────────────────────────────────────────────
def draw_overlay(frame, fps: float, is_frozen: bool = False, connected: bool = True):
    status = get_status()
    with _lock:
        scanning = _scanning
        last_res = _last_result

    color = (0, 255, 255) if scanning else (0, 255, 0)
    cv2.putText(frame, status, (10, 35),
                cv2.FONT_HERSHEY_SIMPLEX, 0.9, color, 2)
    cv2.putText(frame, f"FPS: {fps:.1f}", (10, 70),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (200, 200, 200), 1)
    cv2.putText(frame, f"Last: {CLASS_NAME.get(last_res, '?')}", (10, 100),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (200, 200, 200), 1)

    conn_color = (0, 255, 0) if connected else (0, 0, 255)
    conn_text  = "WiFi: CONNECTED" if connected else "WiFi: WAITING..."
    cv2.putText(frame, conn_text, (10, 130),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, conn_color, 2)

    if is_frozen:
        cv2.putText(frame, "[ SCANNING - CAM LOCKED ]", (80, 240),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 165, 255), 2)
    if scanning:
        cv2.rectangle(frame, (0, 0),
                      (frame.shape[1]-1, frame.shape[0]-1), (0, 0, 255), 4)
    if not connected:
        cv2.rectangle(frame, (0, 0),
                      (frame.shape[1]-1, frame.shape[0]-1), (255, 0, 0), 4)

# ─── Main ────────────────────────────────────────────────────
def main():
    global _frozen_frame

    tcp_server = init_tcp_server(TCP_HOST, TCP_PORT)

    print("[AI] Dang tai model YOLO...")
    try:
        model = YOLO(MODEL_PATH)
        print("[AI] Model san sang.")
    except Exception as e:
        print(f"[FATAL] Khong tai duoc model: {e}")
        tcp_server.close()
        return

    cap = cv2.VideoCapture(URL_CAMERA)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
    if not cap.isOpened():
        print("[FATAL] Khong mo duoc camera!")
        tcp_server.close()
        return

    esp32 = wait_for_esp32(tcp_server)
    if esp32 is None:
        print("[FATAL] Khong ket noi duoc ESP32. Thoat.")
        cap.release()
        tcp_server.close()
        return

    print("=== HE THONG SAN SANG === (Nhan 'q' de thoat)")
    set_status("READY - CHO ESP32...")

    prev_time  = time.time()
    scan_thread = None

    try:
        while True:
            # Lấy frame live hoặc frozen khi AI đang scan
            if _cam_lock.acquire(blocking=False):
                ret, frame = cap.read()
                _cam_lock.release()
                if ret:
                    frame = cv2.resize(frame, (640, 480))
                    _frozen_frame = frame.copy()
                    is_frozen = False
                else:
                    print("[CAMERA] Mat ket noi! Thu lai...")
                    time.sleep(0.5)
                    continue
            else:
                if _frozen_frame is not None:
                    frame = _frozen_frame.copy()
                    is_frozen = True
                else:
                    time.sleep(0.01)
                    continue

            now = time.time()
            fps = 1.0 / max(now - prev_time, 1e-6)
            prev_time = now

            if not is_scanning():
                try:
                    results_live = model(frame, conf=CONF_THRESH, verbose=False)
                    display_frame = results_live[0].plot()
                except Exception:
                    display_frame = frame.copy()
            else:
                display_frame = frame.copy()

            draw_overlay(display_frame, fps, is_frozen, connected=esp32.is_open)
            cv2.imshow("PBL4 AI Camera", display_frame)

            # Xử lý mất kết nối ESP32
            if not esp32.is_open:
                print("[TCP] ESP32 mat ket noi! Dang cho ket noi lai...")
                set_status("WAITING ESP32 WiFi...")
                esp32.close()
                esp32 = wait_for_esp32(tcp_server)
                if esp32 is None:
                    print("[FATAL] Khong ket noi lai duoc. Thoat.")
                    break
                set_status("READY - CHO ESP32...")
                continue

            # Đọc lệnh CAPTURE từ ESP32 qua TCP
            try:
                if esp32.in_waiting > 0:
                    line = esp32.readline().decode('utf-8', errors='ignore').strip()
                    if line == "CAPTURE":
                        print(f"\n[TCP] Nhan CAPTURE!")
                        if not is_scanning():
                            set_status(f"SCANNING 0/{NUM_FRAMES}")
                            scan_thread = threading.Thread(
                                target=analyze_thread,
                                args=(cap, model, esp32),
                                daemon=True
                            )
                            scan_thread.start()
                        else:
                            print("[AI] Dang scan, bo qua CAPTURE nay.")
            except Exception as e:
                print(f"[TCP] Loi nhan du lieu: {e}")

            if cv2.waitKey(1) & 0xFF == ord('q'):
                break

    except KeyboardInterrupt:
        print("\n[INFO] Nhan Ctrl+C, dang tat...")

    finally:
        if scan_thread and scan_thread.is_alive():
            scan_thread.join(timeout=3)
        cap.release()
        cv2.destroyAllWindows()
        if esp32:
            esp32.close()
        tcp_server.close()
        print("[INFO] He thong da tat.")

if __name__ == "__main__":
    main()