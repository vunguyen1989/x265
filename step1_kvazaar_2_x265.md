# Step 01 — Từ Kvazaar sang x265

Tài liệu này ghi lại các khác biệt quan trọng khi áp dụng cách khám phá Step 01
của Kvazaar cho x265. Phạm vi chỉ là main flow:

```text
raw YUV -> encoder API -> frame/CTU encode -> NAL -> Annex-B output
```

Chi tiết thực hành và definition of done nằm trong
[`master_plan.md`](./master_plan.md).

## Kết luận nhanh

Step 01 của x265 phức tạp hơn Kvazaar khoảng **2–3 lần về mặt điều phối**,
không nhất thiết vì thuật toán HEVC cơ bản khác hẳn, mà vì x265 đặt nhiều lớp
CLI, queue, thread, lookahead, DPB và ownership giữa input với output.

Trong Kvazaar, main flow tương đối tuyến tính. Trong x265, trước khi tới phần
CTU analysis cần đi qua cả bộ máy điều phối của CLI và frame encoder. Vì vậy,
với x265 cần hiểu **thread nào đang chạy** và **object nào đang sở hữu dữ liệu**
trước khi đọc sâu thuật toán.

## So sánh main flow

### Kvazaar

Với profile một frame, một worker và tắt WPP, flow quan sát được gần như:

```text
main
  -> input-reader đọc YUV
  -> kvazaar_encode
  -> encode CTU trên main
  -> worker tạo bitstream
  -> kvz_data_chunk
  -> fwrite Annex-B
```

### x265

Với `--frame-threads 1 --pools none --no-wpp`, flow vẫn đi qua nhiều thread và
lớp trung gian:

```text
main
  -> AbrEncoder
      -> YUVRead
      -> Reader
      -> PassEncoder
          -> x265_encoder_encode
              -> Encoder::encode
                  -> lookahead + DPB
                  -> FrameEncoder thread
                      -> CTU analysis
                      -> entropy coding
                      -> NALList
          -> RAWOutput::writeFrame
```

`--pools none` loại bỏ worker pool dùng cho WPP và các công việc song song,
nhưng không biến toàn bộ chương trình thành single-thread.

## Bảng đối chiếu

| Khía cạnh | Kvazaar | x265 |
|---|---|---|
| CLI | `main` trực tiếp điều phối encode | `main` tạo `AbrEncoder`; encode thật nằm trong `PassEncoder` |
| Đọc input | Một `input-reader` | `YUVRead` prefetch và `Reader` chuyển picture qua queue |
| Public API | `kvazaar_encode` | `x265_encoder_encode` gọi `Encoder::encode` |
| Encode một frame | CTU chủ yếu chạy trên `main` khi tắt WPP | CTU vẫn chạy trên thread `FrameEncoder` khi tắt worker pool |
| Frame pipeline | Tương đối tuyến tính | Có lookahead, DPB và frame scheduling |
| Bitstream | Trả về chuỗi `kvz_data_chunk` | `Bitstream` → `NALList` → `x265_nal[]` |
| Output | Chunk được ghi trực tiếp | Header NAL và frame NAL được ghi qua hai đường riêng |
| Flush | Có delayed output/GOP flush | Có delayed output, reordering và vòng flush qua API |
| Quan sát | Checkout Kvazaar đã có trace object/job chi tiết | Step 01 của x265 ban đầu dựa vào breakpoint và thread view |

## Finding 1 — `main` không phải thread encode

Trong Kvazaar, khi tắt WPP, phần lớn flow CTU đơn giản có thể được quan sát trên
`main`. Điều này không đúng với x265.

`main` của x265 chủ yếu:

1. Parse command line.
2. Tạo `AbrEncoder`.
3. Chờ các encode thread hoàn tất.
4. Cleanup và trả exit code.

Vòng lặp gọi public encoder API nằm trong `PassEncoder::threadMain`. Phần nén
frame nằm trong `FrameEncoder::threadMain` và `FrameEncoder::compressFrame`.

Hệ quả khi debug:

- Breakpoint `x265_encoder_encode` thường dừng trên thread `PassEncoder`.
- Breakpoint `Analysis::compressCTU` thường dừng trên thread `Frame`.
- Nếu debugger chỉ theo dõi `main`, có thể hiểu nhầm rằng breakpoint bị bỏ qua.
- Cần luôn kiểm tra thread name và call stack trước khi step tiếp.

## Finding 2 — Profile đơn giản vẫn có nhiều thread

Các option sau làm flow dễ đọc hơn:

```text
--frame-threads 1
--pools none
--no-wpp
--no-pmode
--no-pme
--no-threaded-me
--no-asm
```

Nhưng profile vẫn có các thread chính:

- `main`: khởi tạo và chờ hoàn tất.
- `YUVRead`: prefetch YUV vào ring buffer của input.
- `Reader`: copy picture từ input sang queue của `AbrEncoder`.
- `PassEncoder`: gọi API, nhận NAL và ghi output.
- `Frame`: chạy `FrameEncoder::compressFrame` và CTU analysis.

Do đó, “simple flow” trong x265 có nghĩa là **giảm parallelism của encode**, chứ
không có nghĩa là toàn bộ pipeline chạy tuần tự trên một thread.

## Finding 3 — CLI có thêm lớp `AbrEncoder`

CLI x265 hiện tại hỗ trợ cả ABR ladder. Ngay cả một encode bình thường cũng đi
qua các lớp:

```text
main
  -> AbrEncoder
  -> PassEncoder::init
  -> x265_encoder_open
  -> PassEncoder::threadMain
```

Lớp này tạo thêm queue và thread quanh core encoder. Khi khám phá core codec,
cần phân biệt:

- **CLI orchestration:** `AbrEncoder`, `PassEncoder`, `Reader`.
- **Public API boundary:** `x265_encoder_open`, `x265_encoder_encode`,
  `x265_encoder_headers`, `x265_encoder_close`.
- **Codec core:** `Encoder`, `FrameEncoder`, `Analysis`, `Entropy`, `NALList`.

Không nên đọc logic ABR sâu trong Step 01. Chỉ cần hiểu nó là lớp đưa picture
vào và lấy NAL ra khỏi public API.

## Finding 4 — Một input call không tương ứng một output frame

Một lần gọi:

```text
x265_encoder_encode(..., picInput, ...)
```

có thể trả `numEncoded == 0`, dù `picInput != NULL`. Picture có thể đang nằm
trong lookahead, chờ quyết định slice type, reference hoặc frame encoder.

Khi hết input, CLI tiếp tục gọi:

```text
x265_encoder_encode(..., NULL, ...)
```

cho tới khi `numEncoded == 0`. Đây là vòng flush.

Với GOP mặc định, cần ghi riêng:

```text
call | input POC/null | numEncoded | output POC | PTS | DTS | sliceType | NAL types
```

Bảng này là bằng chứng tốt hơn việc suy luận thứ tự từ source. Nó cho thấy:

- Độ trễ của lookahead.
- Thứ tự encode/output.
- B-frame reordering.
- Những frame chỉ xuất hiện trong lúc flush.

## Finding 5 — Lookahead và DPB luôn nằm trên đường đi

Ngay cả khi dùng profile một frame/zerolatency, cấu trúc `Encoder::encode` vẫn
được tổ chức quanh các thành phần như lookahead, DPB và `FrameEncoder`. Một số
độ trễ và quyết định phức tạp được tắt hoặc rút gọn, nhưng các abstraction vẫn
tồn tại.

Vì vậy, Step 01 chỉ cần trả lời:

- Picture được đưa vào lookahead ở đâu?
- Frame được lấy ra để encode ở đâu?
- DPB chuẩn bị reference/RPS ở đâu?
- `FrameEncoder` bắt đầu và hoàn tất ở đâu?

Chưa cần đọc sâu thuật toán quyết định GOP, RPS hoặc reference list.

## Finding 6 — Ownership của bitstream phức tạp hơn

Trong Kvazaar, output được quan sát dưới dạng chuỗi `kvz_data_chunk`. Trong
x265, đường đi chính là:

```text
Entropy/Bitstream
  -> FrameEncoder::m_nalList
  -> NALList::takeContents
  -> Encoder::m_nalList
  -> x265_nal[] qua public API
  -> RAWOutput::writeFrame
```

`NALList::takeContents` chuyển buffer chứa access unit từ `FrameEncoder` sang
`Encoder`. Vì vậy cần phân biệt:

- Nơi tạo RBSP/slice data.
- Nơi thêm NAL header, start code và emulation-prevention byte.
- Nơi chuyển ownership của NAL buffer.
- Nơi public API trả `x265_nal[]`.
- Nơi CLI thực sự gọi `fwrite`.

Ngoài ra, stream headers đi qua:

```text
x265_encoder_headers
  -> RAWOutput::writeHeaders
```

trong khi access unit của frame đi qua:

```text
x265_encoder_encode
  -> RAWOutput::writeFrame
```

## Finding 7 — x265 chưa có trace tương đương Kvazaar trong Step 01

Checkout Kvazaar hiện có log cho queue, job, dependency, waiter, CTU, bitstream
và output. Điều đó cho phép dựng lại timeline sau khi chương trình chạy.

x265 chưa có instrumentation tương đương trong phạm vi Step 01. Cách quan sát
ban đầu nên là:

1. Breakpoint theo từng lớp.
2. Thread view của CodeLLDB.
3. Ghi các biến `picInput`, `numEncoded`, `nal`, POC, PTS, DTS và slice type.
4. Dùng `ffprobe`/FFmpeg kiểm chứng output cuối.

Không nên thêm log dày đặc ngay từ đầu. Nếu breakpoint không đủ, instrumentation
cần trở thành một sub-step riêng với schema log và tiêu chí gỡ bỏ rõ ràng.

## Chiến lược debug x265 Step 01

### Lượt 1 — Public API

```text
PassEncoder::threadMain
  -> x265_encoder_encode
  -> Encoder::encode
```

Theo dõi:

- `picInput` có null hay không.
- Input POC.
- `numEncoded`.
- Số NAL trả về.
- Output POC/PTS/DTS nếu có.

Mục tiêu là hiểu input loop và flush loop, chưa step vào CTU.

### Lượt 2 — Frame và CTU

```text
Encoder::encode
  -> FrameEncoder::startCompressFrame
  -> FrameEncoder::threadMain
  -> FrameEncoder::compressFrame
  -> FrameEncoder::processRowEncoder
  -> Analysis::compressCTU
```

Theo dõi:

- Thread hiện tại.
- Frame POC và slice type.
- CTU address/row.
- Mode cuối cùng được chọn.

Mục tiêu là xác nhận frame chuyển từ orchestration sang codec core như thế nào.

### Lượt 3 — Entropy, NAL và output

```text
Entropy::encodeCTU
  -> NALList::serialize
  -> FrameEncoder::getEncodedPicture
  -> NALList::takeContents
  -> RAWOutput::writeFrame
```

Theo dõi:

- NAL type.
- Số byte của từng NAL.
- Start code Annex-B.
- Thời điểm ownership của buffer thay đổi.

Mục tiêu là nối được CTU đã encode với byte thực sự ghi ra file.

## Phạm vi không làm trong Step 01

Các nội dung sau chỉ cần ghi thành câu hỏi cho step sau:

- Chi tiết thuật toán lookahead và scenecut.
- RPS, reference list và toàn bộ DPB policy.
- So sánh mode intra/inter và rate-distortion search.
- Motion estimation và fractional-pel interpolation.
- Transform, quantization và coefficient coding chi tiết.
- CABAC context derivation.
- Deblock và SAO internals.
- Rate control, VBV và CU-tree.
- WPP, frame parallelism và SIMD optimization.

Điều kiện để rời Step 01 là hiểu đường đi end-to-end và có output được decoder
độc lập xác nhận, không phải hiểu toàn bộ thuật toán x265.

## Tóm tắt

Khó khăn chính khi chuyển từ Kvazaar sang x265 là:

```text
Kvazaar Step 01:
  chủ yếu lần theo pipeline encode

x265 Step 01:
  hiểu CLI/thread/ownership
  rồi mới lần theo pipeline encode
```

Sau khi vượt qua `AbrEncoder`, `PassEncoder` và `FrameEncoder`, phần lõi vẫn có
cấu trúc quen thuộc:

```text
picture -> CTU analysis -> entropy -> NAL -> Annex-B
```

Vì vậy cách khám phá hiệu quả nhất là giữ profile nhỏ, debug theo ba lượt độc
lập và không mở sâu nhiều nhánh thuật toán cùng lúc.
