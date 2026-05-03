"""
he thong nhung - AI Camera Server (v5)
====================================================
Thay đổi so với v4:
- analyze_thread: lock camera TOÀN BỘ quá trình scan (không lock/release từng frame)
  → main loop dùng frozen frame trong suốt thời gian scan, không xen vào giữa
- Thêm flush buffer trước khi bắt đầu đọc N frame
  → tránh nhận frame cũ còn trong buffer
- Không đổi logic CAPTURE — đúng với kiến trúc ESP32 mới
"""

import cv2
import serial
import time
import threading
from ultralytics import YOLO
from collections import Counter

# ─── CẤU HÌNH ────────────────────────────────────────────────
SERIAL_PORT = 'COM3'
BAUD_RATE   = 115200
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

# ─── Kết nối Serial ──────────────────────────────────────────
def connect_serial(port: str, baud: int, retries: int = 5):
    for attempt in range(1, retries + 1):
        try:
            s = serial.Serial(port, baud, timeout=0.1)
            s.setDTR(False)
            s.setRTS(False)
            time.sleep(2)
            print(f"[SERIAL] Ket noi {port} thanh cong (lan {attempt})")
            return s
        except serial.SerialException as e:
            print(f"[SERIAL] LOI lan {attempt}: {e}")
            time.sleep(2)
    return None

# ─── Thread quét sản phẩm ────────────────────────────────────
def analyze_thread(cap: cv2.VideoCapture, model: YOLO, esp32: serial.Serial):
    global _scanning, _last_result

    with _lock:
        _scanning = True

    predictions = []
    print(f"[AI] Bat dau quet {NUM_FRAMES} frames...")

    # Lock camera TOÀN BỘ trong suốt quá trình scan
    # Main loop sẽ dùng _frozen_frame, không tranh lock
    with _cam_lock:
        # Flush buffer: đọc bỏ vài frame cũ trước khi lấy frame thật
        # CAP_PROP_BUFFERSIZE=1 không đảm bảo buffer rỗng hoàn toàn
        for _ in range(3):
            cap.grab()  # grab() không decode, nhanh hơn read()

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

    # Camera đã unlock — main loop tiếp tục đọc frame live

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
        print(f"[SERIAL] Gui: '{result}' ({CLASS_NAME[result]})")
    except serial.SerialException as e:
        print(f"[SERIAL] LOI khi gui: {e}")

    set_status(f"SENT: {CLASS_NAME[result]}")
    with _lock:
        _last_result = result
        _scanning = False

# ─── Vẽ overlay ──────────────────────────────────────────────
def draw_overlay(frame, fps: float, is_frozen: bool = False):
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
    if is_frozen:
        cv2.putText(frame, "[ SCANNING - CAM LOCKED ]", (80, 240),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 165, 255), 2)
    if scanning:
        cv2.rectangle(frame, (0, 0),
                      (frame.shape[1]-1, frame.shape[0]-1), (0, 0, 255), 4)

# ─── Main ────────────────────────────────────────────────────
def main():
    global _frozen_frame

    esp32 = connect_serial(SERIAL_PORT, BAUD_RATE)
    if esp32 is None:
        print("[FATAL] Khong the ket noi Serial. Thoat.")
        return

    print("[AI] Dang tai model YOLO...")
    try:
        model = YOLO(MODEL_PATH)
        print("[AI] Model san sang.")
    except Exception as e:
        print(f"[FATAL] Khong tai duoc model: {e}")
        esp32.close()
        return

    cap = cv2.VideoCapture(URL_CAMERA)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

    if not cap.isOpened():
        print("[FATAL] Khong mo duoc camera!")
        esp32.close()
        return

    print("=== HE THONG SAN SANG === (Nhan 'q' de thoat)")
    print(f"[INFO] Scan {NUM_FRAMES} frames/lan. Chi scan khi IR Entry trigger.")
    set_status("READY - CHO ESP32...")

    prev_time = time.time()
    scan_thread = None

    try:
        while True:
            # Thử lấy frame live — nếu AI thread đang giữ cam_lock thì dùng frozen
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
                # AI thread đang lock camera — hiển thị frame đóng băng
                if _frozen_frame is not None:
                    frame = _frozen_frame.copy()
                    is_frozen = True
                else:
                    time.sleep(0.01)
                    continue

            now = time.time()
            fps = 1.0 / max(now - prev_time, 1e-6)
            prev_time = now

            # Live YOLO chỉ chạy khi không scan
            if not is_scanning():
                try:
                    results_live = model(frame, conf=CONF_THRESH, verbose=False)
                    display_frame = results_live[0].plot()
                except Exception:
                    display_frame = frame.copy()
            else:
                display_frame = frame.copy()

            draw_overlay(display_frame, fps, is_frozen)
            cv2.imshow("hethognhung AI Camera", display_frame)

            # Đọc lệnh từ ESP32
            try:
                if esp32.in_waiting > 0:
                    line = esp32.readline().decode('utf-8', errors='ignore').strip()
                    if line == "CAPTURE":
                        print(f"\n[SERIAL] Nhan CAPTURE! (IR Entry da trigger)")
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
            except serial.SerialException as e:
                print(f"[SERIAL] Mat ket noi: {e}. Thu ket noi lai...")
                esp32.close()
                time.sleep(2)
                esp32 = connect_serial(SERIAL_PORT, BAUD_RATE)
                if esp32 is None:
                    print("[FATAL] Khong the ket noi lai. Thoat.")
                    break

            if cv2.waitKey(1) & 0xFF == ord('q'):
                break

    except KeyboardInterrupt:
        print("\n[INFO] Nhan Ctrl+C, dang tat...")

    finally:
        if scan_thread and scan_thread.is_alive():
            scan_thread.join(timeout=3)
        cap.release()
        cv2.destroyAllWindows()
        if esp32 and esp32.is_open:
            esp32.setDTR(False)
            esp32.setRTS(False)
            esp32.close()
        print("[INFO] He thong da tat.")

if __name__ == "__main__":
    main()