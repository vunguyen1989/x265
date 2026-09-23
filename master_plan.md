# Master Plan — Khám phá x265 encoder

> Giai đoạn đầu chỉ gồm Step 01, tương ứng với Step 01 trong
> `../kvazaar/master_plan.md`: chạy và debug main flow trước khi đọc sâu từng
> thuật toán. Các step sau chỉ được thêm khi Step 01 đã có ghi chép và bằng
> chứng chạy được.

## Step 01 — Chạy và debug main flow của encoder

**Trạng thái:** IN PROGRESS — build Debug và cấu hình CodeLLDB đã hoàn tất;
tiếp theo là Step 1.4, debug flow một frame.

### 1.1. Chuẩn bị input — TODO

Dùng lại fixture của Kvazaar để hai encoder nhận cùng một nguồn:

```text
../kvazaar/testdata/step1_input_320x240_20f.yuv
format: yuv420p, resolution: 320x240, frame rate: 10 fps, frames: 20
```

Không đưa file YUV vào source tree x265 lần thứ hai. Trước khi debug, xác nhận
kích thước mong đợi là `320 * 240 * 3 / 2 * 20 = 2,304,000` byte:

```shell
wc -c ../kvazaar/testdata/step1_input_320x240_20f.yuv
```

### 1.2. Build với debug symbol — DONE

Bằng chứng build: [`step1_1.2.log`](./step1_1.2.log). Binary đã build thành
công với cấu hình `Debug`, `CHECKED_BUILD=ON`, `noasm` trên macOS ARM64.

Từ thư mục `x265`, dùng build directory riêng để không trộn với build release:

```shell
cmake -S source -B build/debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCHECKED_BUILD=ON \
  -DENABLE_SHARED=OFF \
  -DENABLE_ASSEMBLY=OFF \
  -DCMAKE_C_FLAGS_DEBUG="-O0 -g3 -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS_DEBUG="-O0 -g3 -fno-omit-frame-pointer"

cmake --build build/debug -j
```

Binary đầu ra là `build/debug/x265`. Tắt assembly ở build đầu tiên để debugger
ở trong C/C++; sau khi hiểu scalar flow mới bật lại SIMD. Xác nhận build:

```shell
./build/debug/x265 --version
file ./build/debug/x265
readelf -S ./build/debug/x265 | rg 'debug_info|debug_line'
```

### 1.3. Tạo cấu hình debug trong VS Code — DONE

Cấu hình: [`.vscode/launch.json`](./.vscode/launch.json). Profile một frame đặt
breakpoint tại `main` để bắt đầu từ source C++; output của encoder hiển thị trong
integrated terminal.

Thêm [`.vscode/launch.json`](./.vscode/launch.json) dùng CodeLLDB với hai
profile. Cả hai profile dùng:

```text
program: ${workspaceFolder}/build/debug/x265
cwd:     ${workspaceFolder}
input:   ${workspaceFolder}/../kvazaar/testdata/step1_input_320x240_20f.yuv
```

Profile **`x265: 1 frame (simple flow)`** dùng các argument tương đương:

```shell
./build/debug/x265 \
  --input-res 320x240 --fps 10 --input-depth 8 --input-csp i420 \
  --frames 1 --preset ultrafast --tune zerolatency \
  --keyint 1 --min-keyint 1 --no-scenecut \
  --frame-threads 1 --pools none --no-wpp \
  --no-pmode --no-pme --no-threaded-me --no-asm --qp 28 \
  ../kvazaar/testdata/step1_input_320x240_20f.yuv \
  -o step1_output_1f.h265
```

Profile **`x265: 20 frames (default GOP)`** giữ lookahead/B-frame/GOP mặc định,
chỉ giảm parallelism để call stack dễ đọc:

```shell
./build/debug/x265 \
  --input-res 320x240 --fps 10 --input-depth 8 --input-csp i420 \
  --frames 20 --frame-threads 1 --pools none --no-wpp --no-asm \
  ../kvazaar/testdata/step1_input_320x240_20f.yuv \
  -o step1_output_20f.h265
```

### 1.4. Debug flow một frame — TODO

Chọn profile `x265: 1 frame (simple flow)` và theo breakpoint theo từng lớp,
không cố step qua mọi hàm ngay lần đầu:

1. CLI/config: `main`, `CLIOptions::parse`, `AbrEncoder::AbrEncoder`,
   `PassEncoder::init`.
2. Input/orchestration: `YUVInput::readPicture`, `Reader::threadMain`,
   `PassEncoder::threadMain`.
3. Public API: `x265_encoder_open`, `x265_encoder_headers`,
   `x265_encoder_encode`, `x265_encoder_close`.
4. Encoder scheduling: `Encoder::encode`, `FrameEncoder::startCompressFrame`,
   `FrameEncoder::threadMain`, `FrameEncoder::compressFrame`.
5. CTU/syntax: `FrameEncoder::processRowEncoder`, `Analysis::compressCTU`,
   `Entropy::encodeCTU`.
6. NAL/output: `NALList::serialize`, `FrameEncoder::getEncodedPicture`,
   `RAWOutput::writeHeaders`, `RAWOutput::writeFrame`.

Flow cần hiểu ở mức object:

```text
CLI arguments
  -> x265_param + InputFile/OutputFile
  -> YUV bytes -> x265_picture
  -> x265_encoder_encode -> Encoder::encode -> Frame
  -> lookahead/DPB -> FrameEncoder -> CTU analysis + entropy
  -> Bitstream -> NALList -> x265_nal[]
  -> RAWOutput -> Annex-B .h265
```

#### Thread model cần biết trước khi debug

Ngay cả profile đơn giản vẫn có nhiều thread:

- `main`: parse CLI, tạo `AbrEncoder`, chờ các encode hoàn tất và cleanup.
- `YUVRead`: prefetch raw YUV vào ring buffer của `YUVInput`.
- `Reader`: chuyển picture từ input sang queue dùng bởi `PassEncoder`.
- `PassEncoder`: sở hữu vòng lặp gọi public API và ghi NAL ra output.
- `Frame`: chạy `FrameEncoder::compressFrame`; với `--pools none`, đây cũng là
  nơi đi qua CTU analysis thay vì giao CTU cho worker pool.

Vì vậy breakpoint trong `x265_encoder_encode` hoặc `Analysis::compressCTU` không
chạy trên `main`. Trong CodeLLDB cần bật hiển thị tất cả thread và kiểm tra tên
thread trước khi kết luận flow bị bỏ qua.

#### Ba lượt debug đề xuất

1. **Lượt API:** breakpoint từ `PassEncoder::threadMain` tới
   `x265_encoder_encode`; ghi lại `picInput`, `numEncoded`, `nal`.
2. **Lượt frame/CTU:** breakpoint `Encoder::encode`,
   `FrameEncoder::compressFrame`, `Analysis::compressCTU`; theo dõi `poc`,
   slice type, CTU address và mode thắng.
3. **Lượt bitstream:** breakpoint `Entropy::encodeCTU`, `NALList::serialize`,
   `RAWOutput::writeFrame`; ghi NAL type, `sizeBytes` và start code Annex-B.

Không thêm trace logging vào source ở Step 01. Chỉ khi breakpoint/thread view
không đủ mới tạo một sub-step instrumentation riêng, có format log và tiêu chí
gỡ bỏ rõ ràng.

### 1.5. Debug GOP, delayed output và flush — TODO

Chọn profile `x265: 20 frames (default GOP)`. Đặt breakpoint ở hai lệnh gọi
`api->encoder_encode` trong `PassEncoder::threadMain`: vòng input và vòng flush.
Quan sát:

- Khi `picInput != NULL`, một lần gọi có thể trả `numEncoded == 0` vì lookahead
  và frame reordering.
- Khi hết input, CLI gọi lại với `picInput == NULL` cho đến khi
  `numEncoded == 0`.
- `pic_out.poc`, `pts`, `dts`, `sliceType` và thứ tự output có thể khác thứ tự
  input do B-frame.
- `Encoder::encode` đưa input vào lookahead/DPB, chọn `FrameEncoder`, rồi lấy
  output bằng `FrameEncoder::getEncodedPicture`.
- `NALList::takeContents` chuyển quyền sở hữu access-unit buffer từ
  `FrameEncoder` sang danh sách NAL công khai của `Encoder`.

Lập bảng tối thiểu cho 20 lần input và các lần flush:

```text
call | picInput POC/null | numEncoded | output POC | PTS | DTS | sliceType | NAL types
```

### 1.6. Xác minh output — TODO

```shell
ffprobe -v error -count_frames -select_streams v:0 \
  -show_entries stream=codec_name,profile,width,height,pix_fmt,nb_read_frames \
  -of default=noprint_wrappers=1 step1_output_20f.h265

ffmpeg -v error -i step1_output_20f.h265 -f null -
```

Kết quả mong đợi: HEVC Main, 320x240, YUV420p, đủ 20 frame và decode không lỗi.
Nếu `ffprobe` không báo `nb_read_frames=20`, kiểm tra trước vòng flush và số lần
`RAWOutput::writeFrame` được gọi, chưa đi sâu vào thuật toán CTU.

### 1.7. Ghi lại kết quả — TODO

Tạo `notes/step1_encoder.md` gồm:

- Phiên bản compiler/CMake/x265 và command build/run chính xác.
- Sequence diagram có thread: `main`, `YUVRead`, `Reader`, `PassEncoder`,
  `Frame`.
- Call stack thực tế từ đọc YUV tới `RAWOutput::writeFrame`.
- Vai trò của `x265_param`, `x265_picture`, `Frame`, `FrameEncoder`, `Bitstream`,
  `NALList`, `x265_nal`.
- Bảng input/output/flush của profile 20 frame và danh sách NAL type quan sát
  được.
- Kết quả `ffprobe` và FFmpeg decode.
- Danh sách câu hỏi cho Step 02; chưa biến các câu hỏi đó thành kết luận.

Step 01 hoàn thành khi có thể:

1. Debug từ `PassEncoder::threadMain` qua public API tới CTU analysis và lúc
   `RAWOutput` ghi Annex-B.
2. Giải thích vì sao profile một frame vẫn có nhiều thread dù worker pool bị
   tắt.
3. Giải thích delayed output, frame reordering và vòng flush bằng dữ liệu quan
   sát được.
4. Decode đủ 20 frame bằng FFmpeg mà không có lỗi.

## Sau Step 01

Chỉ chọn **một** lát cắt cho Step 02 dựa trên ghi chép thực tế, ưu tiên theo
thứ tự phụ thuộc:

```text
parameter sets/NAL framing
  -> lookahead + slice type + DPB
  -> CTU/CU mode decision
  -> prediction + transform + quantization
  -> CABAC
  -> deblock + SAO
  -> rate control
  -> WPP/frame parallelism/SIMD
```

Mỗi step mới phải có input cố định, profile CLI nhỏ nhất, breakpoint/call stack
cụ thể, output kiểm chứng được và một file note riêng. Không mở đồng thời nhiều
nhánh thuật toán khi nhánh trước chưa có definition of done.
