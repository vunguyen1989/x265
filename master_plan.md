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

### 1.4. Debug flow một frame — DONE

Dùng profile `x265: 1 frame (simple flow)` để tạo một timeline end-to-end.
Bốn tiểu mục 1.4.1–1.4.4 là bốn góc đọc trên cùng capture, không phải bốn lần
encode và bốn bản log trùng nhau:

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

#### 1.4.1. Input boundary — DONE

Mục tiêu: theo một frame từ lần `fread` raw YUV, qua ring buffer của
`YUVInput`, bản copy trong queue của `Reader`, đến `x265_picture picInput` trên
thread `PassEncoder`.

Breakpoint:

1. `YUVInput::populateFrameQueue` trên thread `YUVRead`.
2. `YUVInput::readPicture`; step tới sau khi các trường của `pic` được gán.
3. `Reader::threadMain` tại `m_input[view]->readPicture(*src)` và ngay sau
   `memcpy(dest->planes[0], ...)`.
4. `PassEncoder::readPicture` tại lúc lấy `srcPic` từ input queue.
5. `PassEncoder::threadMain` ngay sau `readPicture(pic_in[view], view)`.

Ghi `thread`, object, POC, kích thước, bit-depth, `framesize`, stride, địa chỉ
plane và queue index. Không dump toàn bộ plane; chỉ so sánh địa chỉ để xác định
nơi copy dữ liệu và nơi chỉ copy metadata/con trỏ.

Log runtime: `x265.log`, do chính encoder sinh khi chạy profile
`x265: 1 frame (simple flow)`. Profile đặt `X265_FLOW_LOG` tới file này;
logger chỉ được biên dịch trong checked/debug build.

Schema chính:

- Mọi dòng có `event=NNNNNN` tăng toàn cục để đọc total order giữa thread.
- `PROCESS`, `COMPONENT`, `THREAD-CONTROL`: lifecycle từ `main/API-0`,
  CLI parse, encoder creation, worker start request/ready, wait và cleanup.
- `THREAD`: lifecycle của `YUVRead`, `Reader`, `PassEncoder`,
  `FrameEncoder`.
- `CHANNEL`: `WAIT_BEGIN/WAIT_END/PUBLISH/RECEIVE` và queue/slot tương ứng.
- `owner_before/owner_after`: owner của bước xử lý (execution responsibility),
  không mặc định là owner cấp phát/lifetime của vùng nhớ. Riêng
  `NALList::takeContents` là chuyển ownership buffer thật.
- `CTU`, `BITSTREAM`, `OUTPUT`: các stage sau input để cùng một log có thể
  nối xuyên suốt toàn bộ frame.

`x265.log` được mở với mode `w`, nên mỗi lần chạy sẽ thay log cũ. Capture
đã chốt và phần giải thích:

- Raw trace: [`step1_1.4.log`](./step1_1.4.log).
- Flow, ownership và sequence diagrams:
  [`step1_1.4_flow.md`](./step1_1.4_flow.md).

Hoàn thành khi xác nhận được chuỗi `YUVRead -> Reader -> PassEncoder`, nơi
ownership đổi, và frame có `320x240`, 8-bit, I420, `framesize == 115200`.

#### 1.4.2. Public API và zero-latency — DONE

Mục tiêu: quan sát ranh giới CLI/core và xác định input call hay flush call trả
frame duy nhất của profile.

Breakpoint: input call `api->encoder_encode` trong `PassEncoder::threadMain`,
`x265_encoder_encode`, `Encoder::encode`, dòng sau input call, và flush call
với `picInput == NULL`.

Lập bảng:

```text
call | thread | picInput/null | input POC | numEncoded
     | output POC | PTS | DTS | sliceType | nal count
```

Log dự kiến: `step1_1.4.2.log`.

Hoàn thành khi phân biệt được input/flush loop, chứng minh hành vi zero-latency
bằng dữ liệu thực tế và xác định call nào trả NAL cho CLI.

#### 1.4.3. Frame scheduling và CTU đầu tiên — DONE

Mục tiêu: theo frame từ lookahead/DPB sang thread `Frame`, rồi quan sát CTU đầu
tiên được analysis và entropy coding.

Breakpoint: `m_lookahead->addPicture`, `FrameEncoder::startCompressFrame`,
`FrameEncoder::threadMain`, `FrameEncoder::compressFrame`,
`FrameEncoder::processRowEncoder` với `intRow == 0`,
`Analysis::compressCTU` với `ctu.m_cuAddr == 0`, dòng sau `compressCTU` trả về
và `Entropy::encodeCTU` cho CTU 0.

Ghi thread, frame POC, slice type, row, CTU address, QP và top-level mode thắng.
Không đi sâu thuật toán rate-distortion ở lượt này.

Log dự kiến: `step1_1.4.3.log`.

Hoàn thành khi xác nhận `Encoder::encode` giao frame cho `FrameEncoder`, CTU
analysis chạy trên thread `Frame`, và ghi được kết quả của CTU address 0.

#### 1.4.4. Bitstream, NAL ownership và output — DONE

Mục tiêu: nối CTU đã entropy-code với NAL Annex-B được trả qua API và ghi ra
`step1_output_1f.h265`.

Breakpoint: `Entropy::encodeCTU`, `NALList::serialize`,
`FrameEncoder::getEncodedPicture`, `NALList::takeContents`,
`RAWOutput::writeHeaders` và `RAWOutput::writeFrame`.

Lập bảng:

```text
stage | thread | NAL type | sizeBytes | payload address
      | owner before | owner after | output function
```

Phân biệt `x265_encoder_headers -> RAWOutput::writeHeaders` cho VPS/SPS/PPS và
`x265_encoder_encode -> RAWOutput::writeFrame` cho access unit của frame.

Log dự kiến: `step1_1.4.4.log`.

Hoàn thành khi xác định nơi RBSP được đóng gói thành NAL Annex-B, giải thích
được `NALList::takeContents`, và đối chiếu tổng `sizeBytes` với số byte được ghi.

Instrumentation tạm thời đã được bật theo yêu cầu vì breakpoint/thread view
không thể tạo một timeline hợp nhất như log Kvazaar. Nó chỉ hoạt động trong
checked/debug build khi có `X265_FLOW_LOG`; bỏ biến môi trường thì logger
không mở file. Gỡ `flowlog.*` và các call `X265_FLOW_LOG` sau khi bốn artifact
`step1_1.4.x.log` đã được chốt.

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
