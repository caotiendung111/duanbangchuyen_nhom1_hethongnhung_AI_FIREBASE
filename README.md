# Hệ Thống Băng Chuyền Phân Loại Sản Phẩm Thông Minh (AI + IoT)

Dự án thiết kế và xây dựng hệ thống băng chuyền phân loại sản phẩm tự động tích hợp nhận diện hình ảnh bằng trí tuệ nhân tạo (YOLOv8), đồng bộ dữ liệu via Cloud (Firebase) và giám sát đa nền tảng.

---

## 🌟 Tính Năng Nổi Bật

- **Nhận Diện AI (Vision AI)**: Thay thế cảm biến màu sắc truyền thống bằng thuật toán **YOLOv8**, cho phép phân loại đa dạng sản phẩm (Trái cây, sữa, linh kiện...).
- **Kết Nối Cloud (Firebase)**: Đồng bộ dữ liệu thời gian thực giữa phần cứng và ứng dụng di động. Thống kê số lượng sản phẩm tức thì.
- **Giám Sát Đa Nền Tảng**: Theo dõi trạng thái hệ thống qua LCD tại chỗ, giao diện Python trên PC và **Mobile App (React Native)**.
- **Hàng Đợi Thông Minh (Queue Management)**: Xử lý chính xác khi có nhiều vật phẩm cùng lúc trên băng chuyền nhờ cơ chế hàng đợi kỹ thuật số.
- **Đa Chế Độ Hoạt Động**: Chuyển đổi linh hoạt giữa chế độ Tự động (Auto) và Thủ công (Manual) qua nút bấm vật lý hoặc App.
- **Hệ Điều Hành Thời Gian Thực (FreeRTOS)**: Đảm bảo tính ổn định và đa nhiệm trên chip ESP32.

---

## 🏗️ Kiến Trúc Hệ Thống

Hệ thống là sự kết hợp giữa phần cứng nhúng (ESP32), máy chủ xử lý AI (PC), nền tảng Cloud (Firebase) và thiết bị người dùng (Mobile App).

![Kiến trúc hệ thống](docs/images/system_architecture.png)

---

## 📋 Danh Mục Linh Kiện

| STT | Tên Linh Kiện | Chức năng | Giao thức |
|:---:|:---|:---|:---:|
| 1 | **ESP32 DevKit V1** | Vi điều khiển trung tâm, xử lý logic và kết nối WiFi/Cloud. | WiFi, UART, GPIO, I2C |
| 2 | **Động cơ DC 12V** | Truyền động cho băng chuyền. | PWM, GPIO |
| 3 | **Module L298N** | Điều khiển tốc độ và chiều quay động cơ DC. | Digital IO |
| 4 | **Servo MG90S (x2)** | Cơ cấu gạt sản phẩm vào khay tương ứng. | PWM |
| 5 | **Cảm bi���n IR (x2)** | Phát hiện vật cản tại cổng vào và cổng gạt. | Digital Input |
| 6 | **LCD 16x2 I2C** | Hiển thị trạng thái hệ thống và số lượng sản phẩm tại chỗ. | I2C |
| 7 | **Camera (iVCam)** | Thu thập hình ảnh sản phẩm truyền về PC để xử lý AI. | WiFi / USB |

---

## 🔌 Sơ Đồ Đấu Nối (Wiring Diagram)

![Sơ đồ mạch](docs/images/wiring_diagram.png)

---

## 🛠️ Lưu Đồ Hoạt Động

### 1. Lưu đồ thuật toán tổng quát
Mô tả luồng xử lý đa nhiệm của hệ thống dưới sự quản lý của FreeRTOS.

![Lưu đồ thuật toán](docs/images/flowchart.png)

### 2. Sơ đồ tuần tự (Sequence Diagram)
Mô tả quá trình tương tác giữa Cảm biến -> ESP32 -> PC (AI Server) -> Firebase -> App.

![Sơ đồ tuần tự](docs/images/sequence_diagram.png)

---

## 📱 Giao Diện Điều Khiển Mobile

Ứng dụng di động cho phép người dùng giám sát số lượng sản phẩm, bật/tắt hệ thống, động cơ và chuyển đổi chế độ hoạt động từ xa.

![Giao diện App](docs/images/app_ui.png)

---

## 📸 Hình Ảnh Thực Tế

| Mô hình tổng thể | Chi tiết cơ cấu gạt |
|:---:|:---:|
| ![Mô hình 1](docs/images/actual_model_1.png) | ![Mô hình 2](docs/images/actual_model_2.png) |

---

## 🚀 Cài Đặt Model và Dataset

### Cài đặt Roboflow Python SDK

Trước tiên, cài đặt thư viện Roboflow:

```bash
pip install roboflow
```

### Tải Dataset và Model YOLOv11

Sử dụng script Python sau để tải dataset và model đã được huấn luyện:

```python
from roboflow import Roboflow

rf = Roboflow(api_key="6f3bvsp9lKc6iyjJQyBU")
project = rf.workspace("dung-tien-pyfr2").project("trainai-paupe")
version = project.version(1)
dataset = version.download("yolov11")
```

Script này sẽ tải dataset YOLOv11 về máy của bạn để huấn luyện hoặc sử dụng cho suy luận.

### Xem Model và Metrics

Bạn có thể xem chi tiết về model, performance metrics, và các phiên bản khác tại đây:

🔗 **[Roboflow Project - trainai-paupe Models](https://app.roboflow.com/dung-tien-pyfr2/trainai-paupe/models)**

---

## 📂 Cấu Trúc Mã Nguồn

- `src/main.cpp`: Mã nguồn chính cho ESP32 sử dụng FreeRTOS.
- `main.py`: Script Python xử lý nhận diện YOLOv8 và TCP Server.
- `bangchuyen-app/`: Mã nguồn ứng dụng di động (React Native).

---

## 👨‍💻 Thành Viên Thực Hiện

1. **Cao Tiến Dũng** - Lập trình ESP32 (Auto), Python (YOLOv8), Mobile App, Firebase.
2. **Đỗ Thế Hùng** - Lập trình ESP32 (Manual), Thi công lắp ráp, Linh kiện.
3. **Tô Văn Mạnh** - Biên soạn báo cáo Chương 1 & 2.
4. **Trần Anh Khoa** - Biên soạn báo cáo Chương 3.
5. **Nguyễn Phước Duy** - Biên soạn báo cáo Chương 4, Tổng hợp báo cáo.

---

*Dự án thuộc học phần Hệ Thống Nhúng - Đại học Bách Khoa - Đại học Đà Nẵng.*
