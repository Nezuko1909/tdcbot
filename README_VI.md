# dcbot

Một Discord bot lightweight, hiện đại và dạng modular được viết bằng C++17, sử dụng thư viện [D++ (DPP)](https://dpp.dev/) và `libcurl`.

[![C++ Standard](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.16%2B-green.svg)](https://cmake.org/)
[![D++](https://img.shields.io/badge/D%2B%2B-Discord%20Bot%20Library-5865F2.svg)](https://dpp.dev/)

## Tính năng nổi bật

- **Hỗ trợ Slash Commands**: Các lệnh slash command được cấu hình sẵn (`/ping`, `/help`, `/sauce`).
- **Tích hợp Context Menu**: Hỗ trợ menu ngữ cảnh cho tin nhắn (Chuột phải vào tin nhắn -> `Apps -> Sauce`) giúp tìm kiếm nguồn ảnh nhanh chóng.
- **Bộ nạp cấu hình linh hoạt (Configuration Loader)**:
  - Tự động nạp cấu hình từ biến môi trường hệ thống (Environment Variables), file `.env` (ở thư mục gốc hoặc `config/.env`), hoặc file `config/config.json`.
  - Hỗ trợ đăng ký lệnh theo Guild cụ thể (`DISCORD_GUILD_ID`) giúp việc debug nhanh hơn.
- **Hệ thống ghi Log tùy chỉnh**: Ghi log ra console với định dạng thời gian (`YYYY-MM-DD HH:MM:SS`), phân loại mức độ (`INFO`, `WARN`, `ERROR`), và tag module.
- **Kiến trúc mã nguồn dạng Modular**: Phân chia rõ ràng giữa xử lý sự kiện (events), định nghĩa lệnh (commands), bộ điều hướng (handlers), nạp cấu hình và tiện ích.
- **Hỗ trợ Unit Test**: Cấu hình test tùy chọn tích hợp [Catch2 v3](https://github.com/catchorg/Catch2).

## Yêu cầu hệ thống & Thư viện

Trước khi biên dịch dự án, hãy đảm bảo hệ thống của bạn đã cài đặt:

- **Trình biên dịch C++17**: GCC 9+, Clang 10+, hoặc MSVC 2019+.
- **CMake**: Phiên bản 3.16 trở lên.
- **D++ (libdpp)**: Đã cài đặt trên hệ thống (tìm thấy qua `find_package(DPP)`).
- **libcurl**: Thư viện HTTP dùng cho các truy vấn API bên ngoài.
- **Catch2 v3** *(Tùy chọn)*: Chỉ cần thiết nếu bạn muốn biên dịch unit tests (`BUILD_TESTING=ON`).

### Cài đặt D++ & Thư viện phụ thuộc

- **Ubuntu / Debian**:
  ```bash
  sudo apt update
  sudo apt install build-essential cmake libcurl4-openssl-dev
  # Tải và cài đặt gói deb D++ tại https://dl.dpp.dev/
  ```
- **Arch Linux**:
  ```bash
  sudo pacman -S cmake curl libdpp
  ```

## Cấu hình (Configuration)

1. Sao chép một trong các file mẫu cấu hình:
   ```bash
   cp config/.env.example .env
   # HOẶC
   cp config/config.json.example config/config.json
   ```
2. Điền thông tin cấu hình của bạn:
   - `DISCORD_TOKEN`: *(Bắt buộc)* Discord Bot Token của bạn.
   - `DISCORD_GUILD_ID`: *(Tùy chọn)* ID Server Discord để kiểm thử lệnh nhanh hơn trên server đó.

> **Lưu ý bảo mật**: Không bao giờ commit file `.env` hoặc `config.json` chứa Token bot của bạn lên Git.

## Biên dịch dự án (Build)

### Biên dịch tiêu chuẩn với CMake

```bash
# Tạo và chuyển vào thư mục build
mkdir -p build && cd build

# Khởi tạo build system
cmake ..

# Biên dịch chương trình
cmake --build . --config Release
```

### Sử dụng Script biên dịch nhanh

Bạn cũng có thể chạy script shell có sẵn tại thư mục gốc:

```bash
chmod +x build.sh
./build.sh
```

File thực thi sau khi biên dịch sẽ nằm tại `build/bin/dcbot`.

### Biên dịch Unit Tests

Để biên dịch các bài test, truyền thêm tham số `-DBUILD_TESTING=ON` cho CMake:

```bash
cd build
cmake -DBUILD_TESTING=ON ..
cmake --build .
ctest --output-on-failure
```

## Chạy Bot

Sau khi biên dịch thành công, chạy file thực thi từ thư mục gốc của dự án:

```bash
./build/bin/dcbot
```

## Danh sách lệnh (Commands)

| Lệnh | Loại | Mô tả |
| :--- | :--- | :--- |
| `/ping` | Slash Command | Phản hồi "Pong!" để kiểm tra độ trễ và trạng thái hoạt động của bot. |
| `/help` | Slash Command | Hiển thị danh sách các lệnh khả dụng và hướng dẫn sử dụng. |
| `/sauce` | Slash Command | Nhận một liên kết tin nhắn Discord (`link`), lấy ảnh đính kèm và tìm kiếm nguồn ảnh. |
| `Sauce` | Message Context Menu | Chuột phải vào bất kỳ tin nhắn nào -> **Apps** -> **Sauce** để tìm nguồn ảnh trực tiếp. |

## Cấu trúc dự án (Project Structure)

```text
dcbot/
├── CMakeLists.txt          # Cấu hình biên dịch CMake chính
├── build.sh                # Script hỗ trợ biên dịch nhanh
├── config/
│   ├── .env.example        # Mẫu file cấu hình biến môi trường
│   └── config.json.example # Mẫu file cấu hình dạng JSON
├── include/                # Thư mục chứa các file header (.h)
│   ├── commands/           # Khai báo các lệnh Slash & Context (ping, help, sauce)
│   ├── config/             # Cấu trúc cấu hình & giao diện bộ đọc file config
│   ├── events/             # Khai báo bộ lắng nghe sự kiện của D++
│   ├── handlers/           # Logic điều hướng lệnh (Dispatcher)
│   └── utils/              # Các tiện ích bổ trợ (Logger, ...)
├── src/                    # Thư mục chứa mã nguồn triển khai (.cpp)
│   ├── main.cpp            # Điểm khởi chạy ứng dụng (Entry point)
│   ├── commands/           # Triển khai chi tiết các lệnh
│   ├── config/             # Trình đọc cấu hình (Đọc .env & JSON)
│   ├── events/             # Gắn các hàm lắng nghe sự kiện D++
│   ├── handlers/           # Xử lý điều hướng lệnh
│   └── utils/              # Triển khai hệ thống ghi log
└── tests/                  # Mã nguồn Unit Test (Catch2)
    └── test_main.cpp       # File khởi chạy test
```

## Phát triển & Mở rộng (Extending)

Để thêm một Slash Command mới:
1. Tạo header trong `include/commands/my_command.h` và file triển khai `src/commands/my_command.cpp`.
2. Định nghĩa `create_my_command()` và `handle_my_command()`.
3. Đăng ký lệnh trong `src/events/handler.cpp` tại hàm `bot.on_ready()`.
4. Điều hướng lệnh trong `src/handlers/command_dispatcher.cpp`.

## Giấy phép (License)

Dự án được phát triển dựa trên các tiêu chuẩn mã nguồn mở. Bạn có thể tự do chỉnh sửa và tái sử dụng cho các dự án Discord bot của riêng mình.
