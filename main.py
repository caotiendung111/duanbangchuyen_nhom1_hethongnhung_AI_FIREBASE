"""
AI CAMERA IMAGE PROCESSING SERVER - CONVEYOR SORTING SYSTEM
============================================================
"""

import cv2
import socket
import time
import threading
import queue as _queue
import firebase_admin
from firebase_admin import credentials, db as firebase_db
from ultralytics import YOLO
from collections import Counter

# ─── Configuration ──────────────────────────────────────────
TCP_HOST    = '0.0.0.0'   
TCP_PORT    = 8888
URL_CAMERA  = 1
MODEL_PATH  = 'best.pt'
NUM_FRAMES  = 4
MIN_VALID   = 2
CONF_THRESH = 0.65

# Firebase Database Settings
FIREBASE_DB_URL = "https://bangchuyen-a2516-default-rtdb.asia-southeast1.firebasedatabase.app"
FB_AUTH_TOKEN = "IbXPvjLfRZCGljcwvJo1Cgtsqyq9rhded4JpaxvU"

CLASS_MAP  = {0: 'A', 1: 'B', 2: 'O', 3: 'M'}
CLASS_NAME = {'A': 'Apple', 'B': 'Banana', 'O': 'Orange', 'M': 'Milk', 'U': 'Unknown'}

# ─── Shared Thread States ────────────────────────────────────
_lock         = threading.Lock()
_cam_lock     = threading.Lock()
_scanning     = False
_last_result  = 'U'
_status_msg   = "READY"
_frozen_frame = None

# Initialize Firebase SDK
try:
    options = {'databaseURL': FIREBASE_DB_URL, 'auth': {'uid': 'admin'}}
    firebase_admin.initialize_app(options=options)
    print("[FIREBASE] Connection established successfully!")
except Exception as e:
    print(f"[FIREBASE] SDK initialization error: {e}")

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

# ─── TCP Socket Communication Wrapper ────────────────────────
class TCPSerial:
    """Mock serial.Serial interface using TCP raw sockets."""

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

# ─── TCP Socket Server Setup ─────────────────────────────────
def init_tcp_server(host: str, port: int) -> socket.socket:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    print(f"[TCP] Server listening on port {port}")
    print(f"[TCP] Ensure client connects to this system's Wi-Fi IP.")
    return srv

def wait_for_esp32(srv: socket.socket) -> 'TCPSerial | None':
    print("[TCP] Awaiting ESP32 Wi-Fi connection...")
    srv.settimeout(None)
    try:
        cli, addr = srv.accept()
        cli.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f"[TCP] ESP32 client connected from address {addr}")
        return TCPSerial(cli)
    except Exception as e:
        print(f"[TCP] Connection accept error: {e}")
        return None

# ─── Object Recognition Thread ──────────────────────────────
def analyze_thread(cap: cv2.VideoCapture, model: YOLO, esp32: TCPSerial):
    global _scanning, _last_result

    with _lock:
        _scanning = True

    predictions = []
    print(f"[AI] Beginning acquisition of {NUM_FRAMES} frames...")

    with _cam_lock:
        # Clear camera hardware buffer
        for _ in range(3):
            cap.grab()

        for i in range(NUM_FRAMES):
            ret, frame = cap.read()
            if not ret:
                print(f"[AI] Warning: Failed to read frame {i+1}")
                continue

            frame = cv2.resize(frame, (640, 480))
            try:
                results = model(frame, conf=CONF_THRESH, verbose=False)
            except Exception as e:
                print(f"[AI] Model inference error on frame {i+1}: {e}")
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
        print(f"[AI] Only {len(valid)}/{NUM_FRAMES} valid frames -> Unknown")
    else:
        most_common_id = Counter(valid).most_common(1)[0][0]
        result = CLASS_MAP.get(most_common_id, 'U')
        confidence = valid.count(most_common_id) / len(valid) * 100
        print(f"[AI] Classification result: {CLASS_NAME[result]} ({confidence:.0f}% over {len(valid)} frames)")

    try:
        esp32.write(result.encode())
        print(f"[TCP] Sending result: '{result}' ({CLASS_NAME[result]})")
    except ConnectionError as e:
        print(f"[TCP] TCP transmit error: {e}")

    set_status(f"SENT: {CLASS_NAME[result]}")
    with _lock:
        _last_result = result
        _scanning = False

# ─── OpenCV Canvas Draw Overlay ──────────────────────────────
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
    conn_text  = "Wi-Fi: CONNECTED" if connected else "Wi-Fi: AWAITING CLIENT..."
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

# ─── Server Loop ─────────────────────────────────────────────
def main():
    global _frozen_frame

    tcp_server = init_tcp_server(TCP_HOST, TCP_PORT)

    print("[AI] Loading YOLO model...")
    try:
        model = YOLO(MODEL_PATH)
        print("[AI] YOLO model loaded successfully.")
    except Exception as e:
        print(f"[FATAL] Failed to load model: {e}")
        tcp_server.close()
        return

    cap = cv2.VideoCapture(URL_CAMERA)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
    if not cap.isOpened():
        print("[FATAL] Failed to open camera device!")
        tcp_server.close()
        return

    esp32 = wait_for_esp32(tcp_server)
    if esp32 is None:
        print("[FATAL] Failed to connect to ESP32. Exiting.")
        cap.release()
        tcp_server.close()
        return

    print("=== SYSTEM ACTIVE === (Press 'q' to quit)")
    set_status("READY - AWAITING CLIENT...")

    prev_time  = time.time()
    scan_thread = None

    try:
        while True:
            # Capture live frames if camera lock is available
            if _cam_lock.acquire(blocking=False):
                ret, frame = cap.read()
                _cam_lock.release()
                if ret:
                    frame = cv2.resize(frame, (640, 480))
                    _frozen_frame = frame.copy()
                    is_frozen = False
                else:
                    print("[CAMERA] Stream link lost! Retrying...")
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
            cv2.imshow("Smart Conveyor AI Camera", display_frame)

            # Reconnection logic on client socket disconnection
            if not esp32.is_open:
                print("[TCP] ESP32 disconnected! Re-establishing connection...")
                set_status("AWAITING CLIENT Wi-Fi...")
                esp32.close()
                esp32 = wait_for_esp32(tcp_server)
                if esp32 is None:
                    print("[FATAL] Failed to reconnect. Exiting.")
                    break
                set_status("READY - AWAITING CLIENT...")
                continue

            # Read capture triggers from ESP32 TCP socket
            try:
                if esp32.in_waiting > 0:
                    line = esp32.readline().decode('utf-8', errors='ignore').strip()
                    if line == "CAPTURE":
                        print(f"\n[TCP] CAPTURE command received!")
                        if not is_scanning():
                            set_status(f"SCANNING 0/{NUM_FRAMES}")
                            scan_thread = threading.Thread(
                                target=analyze_thread,
                                args=(cap, model, esp32),
                                daemon=True
                            )
                            scan_thread.start()
                        else:
                            print("[AI] Scanning in progress, ignoring duplicate CAPTURE request.")
            except Exception as e:
                print(f"[TCP] TCP receive error: {e}")

            if cv2.waitKey(1) & 0xFF == ord('q'):
                break

    except KeyboardInterrupt:
        print("\n[INFO] Ctrl+C interrupt, shutting down...")

    finally:
        if scan_thread and scan_thread.is_alive():
            scan_thread.join(timeout=3)
        cap.release()
        cv2.destroyAllWindows()
        if esp32:
            esp32.close()
        tcp_server.close()
        print("[INFO] Shutdown complete.")

if __name__ == "__main__":
    main()