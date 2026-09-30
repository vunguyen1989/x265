# Debug FPA — Issue 970

Bắt đầu đọc **[flow một frame và sequence diagram](../970_worklog/step2/2.1.1_flow.md)**,
đối chiếu **[log capture](../970_worklog/step2/2.1.1_x264.log)**.

## Build và launch

Chạy từ root repo:

```bash
bash issues/970_debug/build.sh
```

Trong VS Code Run and Debug, chọn **x264: Issue 970 - 1 frame FPA flow** rồi F5.
Dừng tại main; đặt breakpoint theo bảng trong tài liệu flow để step, hoặc F5
để chạy hết. Encoder tạo `x264.log` ở root nhờ biến `X264_FLOW_LOG` của launch.
Các lần chạy thay log/output cũ. Capture đã chốt nằm trong worklog.

Profile dùng C/C++ extension (`cppdbg`) và GDB Linux, với `-nx` để tránh cấu hình
GDB toàn cục can thiệp. Không nạp script Python vào debugger. Logger được biên dịch
với `-DX264_FLOW_TRACE`; bỏ biến môi trường thì không mở log. Đây là instrumentation
tạm cho Linux/POSIX, chưa phải patch tính năng FPA cho x265.

`.vscode` bị repo cha ignore; [launch.json tại đây](./launch.json) lưu bản profile
để review hoặc thêm vào `configurations` của `.vscode/launch.json` ở checkout mới.
Không ghi đè các profile x265 có sẵn.

## Tái lập bằng debug adapter và kiểm tra

```bash
python3 issues/970_debug/capture_launch.py
python3 issues/970_debug/validate.py
```

Runner đọc profile trong `.vscode/launch.json`, launch qua debug adapter của
C/C++ extension đã cài ở `~/.vscode-server/extensions`, continue từ entry,
lưu DAP transcript trong worklog. Cần quyền ptrace.

Validation đọc `x264.log`, parse/decode output, so 18 field FPA và byte NAL,
chạy cùng binary với logger tắt rồi so bitstream. Cuối cùng lưu bản log chốt.
So sánh trước instrumentation dùng artifact `2.1.1_before_instrumentation.h264`
đã có trong workspace; vì worklog bị ignore, checkout mới cần lấy lại bộ artifact
Step 01/02 trước khi chạy nguyên script validation.

## Source instrumentation

Source local trong `x264/` đã được sửa; repo cha ignore thư mục này.
[x264-flow.patch](./x264-flow.patch) lưu toàn bộ diff, gồm `common/flowlog.[ch]`.
Base là x264 commit `c24e06c2e184345ceb33eb20a15d1024d9fd3497`.
Chỉ ở checkout x264 sạch cùng commit, kiểm tra và áp dụng bằng:

```bash
git -C x264 apply --check ../issues/970_debug/x264-flow.patch
git -C x264 apply ../issues/970_debug/x264-flow.patch
```

Source hiện tại đã có patch nên không cần áp dụng lại. Các capture GDB cũ vẫn
được giữ trong worklog; số dòng source cũ có thể dịch sau instrumentation.
