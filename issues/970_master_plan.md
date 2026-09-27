<h1 align="center">Master Plan — Issue #970: Frame packing arrangement SEI</h1>

<p align="center">Tiến độ: 11/43</p>

## Yêu cầu

- Bộ tài liệu task: [yêu cầu #970](./970.md) → [master plan](./970_master_plan.md) → [worklog](./970_worklog/). Mục tiêu: bổ sung báo hiệu 3D trong HEVC bằng SEI `frame_packing_arrangement` để thiết bị hỗ trợ nhận diện cách sắp xếp view từ elementary stream.
- Thêm CLI `--frame-packing` và param/API tương ứng như x264: type 3 = side-by-side, type 4 = top-bottom, type 5 = frame alternation.
- Ghi SEI trước mỗi keyframe theo yêu cầu gốc; đối chiếu syntax và semantics HEVC trước khi chốt triển khai.
- Nếu chưa triển khai đầy đủ param, phương án tối thiểu được issue đề xuất là cho phép ghi qua generic user-SEI payload API, kèm tài liệu để front-end tích hợp.
- Kiểm chứng bằng bitstream/parser và nguồn stereo thật trên thiết bị hỗ trợ HEVC/FPA; option chỉ báo hiệu cách bố trí view, không chuyển video 2D thành stereo thật.

## Step 01 — Thí nghiệm black-box SBS với x264 và đối chiếu x265

### 1.1. Chốt input, phạm vi và artifact — DONE

- [x] **1.1.1.** Chốt thư mục chạy và input. Mọi command bên dưới chạy từ thư mục gốc
  `x265`, không phải `issues/`. File đã có:
  [`big-buck-bunny-480p-30sec.mp4`](../big-buck-bunny-480p-30sec.mp4).

  Metadata đã đọc bằng `ffprobe` khi cập nhật kế hoạch:

  ```text
  video: H.264, yuv420p, 854x480
  frame rate: 24 fps
  frames: 720
  duration: 30 seconds
  ```

  **Validate output — 1.1.1**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Input MP4 và thư mục chạy. |
  | Cách kiểm tra | Đối chiếu file và metadata ffprobe. |
  | Tiêu chí đạt | Input đúng file, H.264/yuv420p, 854×480, 24 fps, 720 frame, 30 giây. |
  | Kết quả ✅ | Đạt; [metadata](./970_worklog/step1/1.1.2_input.json) đã xác nhận. |

- [x] **1.1.2.** Ghi lại input hash và phiên bản công cụ để tái lập:

  ```shell
  mkdir -p issues/970_worklog/step1
  sha256sum big-buck-bunny-480p-30sec.mp4 > issues/970_worklog/step1/1.1.2_input.sha256
  ffprobe -v error -select_streams v:0 \
    -show_entries stream=codec_name,width,height,pix_fmt,r_frame_rate,nb_frames:format=duration \
    -of json big-buck-bunny-480p-30sec.mp4 > issues/970_worklog/step1/1.1.2_input.json
  ffmpeg -version > issues/970_worklog/step1/1.1.2_ffmpeg_version.txt
  git -C x264 rev-parse HEAD > issues/970_worklog/step1/1.1.2_x264_commit.txt
  git -C x264 status --short > issues/970_worklog/step1/1.1.2_x264_status.txt
  ```

  **Validate output — 1.1.2**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Hash, metadata, phiên bản FFmpeg, commit/status x264. |
  | Cách kiểm tra | Kiểm tra đủ artifact và exit code từng command. |
  | Tiêu chí đạt | Các command thành công, dữ liệu đủ để nhận diện input và công cụ. |
  | Kết quả ✅ | Đạt; [command/exit code](./970_worklog/step1/1.1.2_input_commands.json), [hash](./970_worklog/step1/1.1.2_input.sha256), [FFmpeg](./970_worklog/step1/1.1.2_ffmpeg_version.txt), [commit](./970_worklog/step1/1.1.2_x264_commit.txt), [status](./970_worklog/step1/1.1.2_x264_status.txt). |

- [x] **1.1.3.** Giới hạn phạm vi đầu tiên ở SBS/type 3 và control không truyền option.
  Cần trả lời bằng output: feature thêm metadata gì, ảnh decode có thay đổi
  không, và x265 hiện thiếu phần báo hiệu nào trên cùng input?

  Kết quả: [phạm vi đã chốt](./970_worklog/step1/1.1.3_scope.md),
  [metadata input](./970_worklog/step1/1.1.2_input.json),
  [command và exit code](./970_worklog/step1/1.1.2_input_commands.json).
  Artifact dùng tiền tố checkbox tạo file (`1.1.2_`, `1.2.1_`, `1.3.1_`, …). File dùng chung giữ tiền tố checkbox tạo ban đầu.
  Log/command lịch sử giữ nguyên nội dung; tên cũ được đối chiếu qua
  [bảng đổi tên](./970_worklog/step1/1.1.2_artifact_paths.json).

  **Validate output — 1.1.3**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Phạm vi thí nghiệm. |
  | Cách kiểm tra | Đọc scope và đối chiếu mục tiêu black-box. |
  | Tiêu chí đạt | Chỉ SBS/type 3 và control; nêu câu hỏi metadata, pixel decode và signalling x265. |
  | Kết quả ✅ | Đạt; [scope](./970_worklog/step1/1.1.3_scope.md) đã ghi phạm vi, chưa khẳng định kết quả encode. |

### 1.2. Chuẩn bị binary x264 — DONE

- [x] **1.2.1.** Build local để biết chính xác encoder nào tạo output. Chưa đặt breakpoint
  hoặc đọc sâu writer trong bước này; debug symbol chuẩn bị cho bước sau.
  Dùng build directory riêng, không ghi build vào source x264:

  ```shell
  mkdir -p build/x264-debug
  (
    cd build/x264-debug
    ../../x264/configure \
      --enable-debug \
      --disable-asm \
      --disable-opencl \
      --disable-lavf \
      --disable-ffms \
      --disable-avs \
      --disable-swscale \
      --bit-depth=8 \
      --chroma-format=420 \
      --extra-cflags="-O0 -g3 -fno-omit-frame-pointer"
    make -j2
  ) > issues/970_worklog/step1/1.2.1_build.log 2>&1

  ./build/x264-debug/x264 --version
  file ./build/x264-debug/x264
  readelf -S ./build/x264-debug/x264 | rg 'debug_info|debug_line'
  ```

  **Validate output — 1.2.1**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Binary x264 và build log trong build directory riêng. |
  | Cách kiểm tra | Kiểm tra configure/make exit code và vị trí binary. |
  | Tiêu chí đạt | Build thành công tại `build/x264-debug`, source x264 không đổi. |
  | Kết quả ✅ | Đạt; [build log](./970_worklog/step1/1.2.1_build.log), [exit code](./970_worklog/step1/1.2.1_x264_build_commands.json); configure/make đều trả 0. |

- [x] **1.2.2.** Kiểm tra exit code và `1.2.1_build.log`; xác nhận cờ thực tế trong
  `build/x264-debug/config.mak`. `--enable-debug` của source này thêm `-O1 -g`,
  nên cần xác nhận `-O0` bổ sung có hiệu lực. Các lệnh `readelf` dùng cho Linux.

  **Validate output — 1.2.2**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Cờ compile thực tế và kết quả rà build log. |
  | Cách kiểm tra | Đọc config.mak, thứ tự cờ tối ưu và log compiler. |
  | Tiêu chí đạt | Cờ tối ưu cuối là `-O0`; có `-g3`, giữ frame pointer; không có lỗi build. |
  | Kết quả ✅ | Đạt; [config](./970_worklog/step1/1.2.2_x264_config.mak): `-O0` sau `-O1`; không có compiler warning. |

- [x] **1.2.3.** Xác nhận hoàn thành: có CLI x264 chạy được, debug symbol và log build thành công.

  Kết quả: x264 `0.164.3192 c24e06c` chạy được; configure/build exit code 0,
  không có compiler warning; binary có `.debug_info` và `.debug_line`.
  `-O0` đứng sau `-O1` trong cờ compile nên có hiệu lực; source x264 không đổi.
  Artifact: [build log](./970_worklog/step1/1.2.1_build.log),
  [command/exit code](./970_worklog/step1/1.2.1_x264_build_commands.json),
  [config](./970_worklog/step1/1.2.2_x264_config.mak),
  [kiểm tra binary](./970_worklog/step1/1.2.3_x264_checks.json).

  **Validate output — 1.2.3**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | CLI chạy được và debug symbol. |
  | Cách kiểm tra | Đọc kết quả `--version`, `file`, `readelf -S`. |
  | Tiêu chí đạt | CLI exit code 0; có `.debug_info` và `.debug_line`. |
  | Kết quả ✅ | Đạt; x264 `0.164.3192 c24e06c`; [checks](./970_worklog/step1/1.2.3_x264_checks.json), [sections](./970_worklog/step1/1.2.3_x264_sections.txt). |

### 1.3. Tạo SBS giả từ Big Buck Bunny — DONE

- [x] **1.3.1.** Tạo một frame 640x360 gồm hai nửa 320x360 giống nhau:

  ```text
  2D input -> scale 320x360 -> duplicate -> hstack

  +----------------+----------------+
  | cùng một view  | cùng một view  |
  | nửa trái       | nửa phải       |
  +----------------+----------------+
               640x360
  ```

  ```shell
  ffmpeg -nostdin -i big-buck-bunny-480p-30sec.mp4 \
    -filter_complex "[0:v:0]fps=24,scale=320:360,setsar=1,format=yuv420p,split=2[left][right];[left][right]hstack=inputs=2[sbs]" \
    -map "[sbs]" -an -frames:v 720 -pix_fmt yuv420p \
    issues/970_worklog/step1/1.3.1_sbs-test.y4m \
    > issues/970_worklog/step1/1.3.1_prepare_sbs.log 2>&1

  ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=width,height,pix_fmt,r_frame_rate,nb_read_frames \
    -of default=noprint_wrappers=1 issues/970_worklog/step1/1.3.1_sbs-test.y4m
  ```

  **Validate output — 1.3.1**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | `1.3.1_sbs-test.y4m` và log tạo SBS. |
  | Cách kiểm tra | Kiểm tra exit code; đối chiếu hai nửa pixel Y/U/V. |
  | Tiêu chí đạt | Tạo thành công frame 640×360 gồm hai nửa 320×360 cùng view. |
  | Kết quả ✅ | Đạt; [command](./970_worklog/step1/1.3.1_sbs_commands.json) thành công; [kiểm tra pixel](./970_worklog/step1/1.3.2_sbs_validation.json) xác nhận hai nửa giống nhau trên 720 frame. |

- [x] **1.3.2.** Kiểm tra kỳ vọng: 640x360, yuv420p, 24 fps, 720 frame. Y4M chứa metadata
  đầu vào nên không cần truyền lại resolution/fps cho CLI encoder. File raw
  này khoảng 249 MB; giữ một bản dùng chung cho các ca.

  **Validate output — 1.3.2**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Metadata và kích thước fixture dùng chung. |
  | Cách kiểm tra | Đọc ffprobe có `-count_frames` và kích thước file. |
  | Tiêu chí đạt | 640×360, yuv420p, 24 fps, đủ 720 frame; giữ một fixture dùng chung. |
  | Kết quả ✅ | Đạt; [metadata](./970_worklog/step1/1.3.2_sbs_metadata.json); file 248,836,378 byte. |

- [x] **1.3.3.** Trích một ảnh để kiểm tra cách xếp hai nửa:

  ```shell
  ffmpeg -nostdin -i issues/970_worklog/step1/1.3.1_sbs-test.y4m \
    -frames:v 1 issues/970_worklog/step1/1.3.3_sbs-preview.png
  ```

  **Validate output — 1.3.3**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Ảnh preview SBS. |
  | Cách kiểm tra | Mở ảnh và quan sát bố trí hai nửa. |
  | Tiêu chí đạt | Có ảnh nhìn rõ hai view giống nhau cạnh nhau. |
  | Kết quả ✅ | Đạt; frame đầu đen nên đã trích và xem thêm [preview giây 5](./970_worklog/step1/1.3.3_sbs-preview-5s.png). |

- [x] **1.3.4.** Ghi rõ đây là SBS giả: hai view giống nhau, không có disparity tạo chiều sâu
  stereo. Nó dùng để kiểm tra metadata/bitstream, chưa chứng minh trải nghiệm 3D trên TV.

  **Validate output — 1.3.4**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Ghi chú giới hạn SBS giả. |
  | Cách kiểm tra | Đối chiếu mô tả fixture với cách nhân đôi view. |
  | Tiêu chí đạt | Nêu rõ không có disparity/chiều sâu stereo; chưa chứng minh trải nghiệm 3D trên TV. |
  | Kết quả ✅ | Đạt; ghi chú trong checklist này và [validation fixture](./970_worklog/step1/1.3.2_sbs_validation.json). |

- [x] **1.3.5.** Khi chạy lại lệnh FFmpeg, chọn tên output mới hoặc chủ động dùng `-y` nếu
  muốn thay artifact cũ.

  **Validate output — 1.3.5**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Quy tắc xử lý output khi chạy lại. |
  | Cách kiểm tra | Đọc command đã chạy và chính sách ghi đè. |
  | Tiêu chí đạt | Không ghi đè ngoài ý muốn; lần chạy lại dùng tên mới hoặc chủ động `-y`. |
  | Kết quả ✅ | Đạt; [command](./970_worklog/step1/1.3.1_sbs_commands.json) dùng `-n`; [chính sách](./970_worklog/step1/1.3.2_sbs_validation.json) đã ghi rõ. |

### 1.4. Experiment 1 — x264 control và frame packing 3 — TODO

- [ ] **1.4.1.** Giữ cùng input và toàn bộ tham số encode; chỉ ca feature thêm option.
  Dùng một thread, không B-frame, GOP 24 frame để dễ đối chiếu vị trí SEI.

  **Validate output — 1.4.1**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Cặp command encode thực tế. |
  | Cách kiểm tra | So sánh input và tham số hai ca. |
  | Tiêu chí đạt | Cùng fixture, một thread, không B-frame, GOP 24; chỉ khác frame packing và đường dẫn output/log. |
  | Kết quả | Chưa validate. |

- [ ] **1.4.2.** Encode control và ca feature:

  ```shell
  ./build/x264-debug/x264 \
    --demuxer y4m --frames 720 --preset ultrafast --qp 28 \
    --threads 1 --lookahead-threads 1 --bframes 0 \
    --keyint 24 --min-keyint 24 --scenecut 0 \
    -o issues/970_worklog/step1/1.4.2_no-frame-packing.h264 \
    issues/970_worklog/step1/1.3.1_sbs-test.y4m \
    > issues/970_worklog/step1/1.4.2_no-frame-packing.log 2>&1

  ./build/x264-debug/x264 \
    --demuxer y4m --frames 720 --preset ultrafast --qp 28 \
    --threads 1 --lookahead-threads 1 --bframes 0 \
    --keyint 24 --min-keyint 24 --scenecut 0 \
    --frame-packing 3 \
    -o issues/970_worklog/step1/1.4.2_frame-packing-3.h264 \
    issues/970_worklog/step1/1.3.1_sbs-test.y4m \
    > issues/970_worklog/step1/1.4.2_frame-packing-3.log 2>&1
  ```

  ```text
                           +-> x264 control -> 1.4.2_no-frame-packing.h264
  + SBS input (cùng file) --|
                           +-> x264 type 3  -> 1.4.2_frame-packing-3.h264
  ```

  **Validate output — 1.4.2**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Hai bitstream H.264 và log encode. |
  | Cách kiểm tra | Kiểm tra output không rỗng, exit code và số frame trong log. |
  | Tiêu chí đạt | Cả hai ca encode thành công đủ 720 frame; kiểm tra SEI/pixel riêng ở 1.5. |
  | Kết quả | Chưa validate. |

- [ ] **1.4.3.** Ghi exit code và kiểm tra log từng command trước khi phân tích output.

  **Validate output — 1.4.3**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Bản ghi command, exit code và rà log từng ca. |
  | Cách kiểm tra | Đối chiếu bản ghi với hai lần encode. |
  | Tiêu chí đạt | Đủ exit code/log cho cả hai ca; lỗi nếu có phải xử lý trước khi phân tích. |
  | Kết quả | Chưa validate. |

### 1.5. Chứng minh khác biệt metadata và kiểm tra ảnh decode — TODO

#### 1.5.1. Kiểm tra metadata

- [ ] **1.5.1.1.** Parse cả hai elementary stream:

  ```shell
  ffmpeg -hide_banner -bsfs | rg trace_headers

  for name in no-frame-packing frame-packing-3; do
    ffmpeg -nostdin -hide_banner -i "issues/970_worklog/step1/1.4.2_$name.h264" \
      -map 0:v:0 -c:v copy -bsf:v trace_headers -f null - \
      > "issues/970_worklog/step1/1.5.1.1_${name}_headers.log" 2>&1 || exit 1
  done

  rg -n -i 'frame.packing|arrangement|content_interpretation' \
    issues/970_worklog/step1/1.5.1.1_*_headers.log
  ```

  **Validate output — 1.5.1.1**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Hai log parser đầy đủ. |
  | Cách kiểm tra | Chạy parser cho cả hai stream và lưu exit code. |
  | Tiêu chí đạt | Parser xử lý thành công cả hai stream; giữ log đầy đủ. |
  | Kết quả | Chưa validate. |

- [ ] **1.5.1.2.** Lập bảng bằng kết quả parser, chưa đi vào source encoder:

  | Quan sát | Control | Có `--frame-packing 3` |
  | --- | --- | --- |
  | FPA SEI payload type 45 | Kỳ vọng không có | Kỳ vọng có |
  | Arrangement type | Không áp dụng | Kỳ vọng 3 |
  | Các field còn lại | Không áp dụng | Ghi giá trị parser đọc được |
  | Số message, frame liên quan | Ghi thực tế | Ghi thực tế |
  | Vị trí SEI so với slice NAL | Không áp dụng | Ghi thực tế |

  **Validate output — 1.5.1.2**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Bảng đối chiếu FPA của control và feature. |
  | Cách kiểm tra | Đối chiếu từng ô với log parser. |
  | Tiêu chí đạt | Control không FPA, feature payload type 45/type 3; các field, số message và vị trí ghi từ quan sát thực tế. |
  | Kết quả | Chưa validate. |

- [ ] **1.5.1.3.** Phân biệt payload type 45 với NAL type chứa SEI. Chỉ grep hex `2d` không đủ
  để chứng minh có FPA. Nếu `trace_headers` không đọc được FPA, dùng parser
  hỗ trợ trước khi kết luận vắng mặt; lưu cả log đầy đủ và exit code.

  **Validate output — 1.5.1.3**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Bằng chứng parser hỗ trợ FPA và cách nhận diện payload. |
  | Cách kiểm tra | Kiểm tra parser đọc được field FPA ở ca feature; phân biệt payload type/NAL type. |
  | Tiêu chí đạt | Không dùng grep hex làm bằng chứng; chỉ kết luận vắng mặt khi parser hỗ trợ FPA và parse thành công. |
  | Kết quả | Chưa validate. |

- [ ] **1.5.1.4.** Quan sát lịch lặp so với keyframe, chưa lấy điều kiện trong code làm kết quả.

  **Validate output — 1.5.1.4**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Bảng lịch phát FPA theo frame/keyframe. |
  | Cách kiểm tra | Đối chiếu message với frame và slice NAL trong output. |
  | Tiêu chí đạt | Có lịch lặp quan sát thực tế; không suy ra từ code encoder. |
  | Kết quả | Chưa validate. |

#### 1.5.2. Kiểm tra ảnh decode

- [ ] **1.5.2.1.** Kiểm tra decode và số frame cho từng file:

  ```shell
  for name in no-frame-packing frame-packing-3; do
    ffmpeg -nostdin -v error -i "issues/970_worklog/step1/1.4.2_$name.h264" \
      -map 0:v:0 -an -f framemd5 "issues/970_worklog/step1/1.5.2.1_$name.framemd5" || exit 1
    ffprobe -v error -count_frames -select_streams v:0 \
      -show_entries stream=codec_name,width,height,pix_fmt,nb_read_frames \
      -of json "issues/970_worklog/step1/1.4.2_$name.h264" \
      > "issues/970_worklog/step1/1.5.2.1_${name}_decoded.json" || exit 1
  done

  diff -u issues/970_worklog/step1/1.5.2.1_no-frame-packing.framemd5 \
    issues/970_worklog/step1/1.5.2.1_frame-packing-3.framemd5
  ```

  **Validate output — 1.5.2.1**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Hai file framemd5, metadata decoded và exit code. |
  | Cách kiểm tra | Chạy decode và ffprobe đếm frame cho từng ca. |
  | Tiêu chí đạt | Decode/ffprobe thành công, artifact đủ cho cả hai stream. |
  | Kết quả | Chưa validate. |

- [ ] **1.5.2.2.** Xác nhận kỳ vọng hai ca decode đủ 720 frame 640x360 và checksum pixel từng
  frame trùng nhau. Nếu khác, ghi nhận và điều tra thay vì khẳng định chỉ đổi metadata.

  **Validate output — 1.5.2.2**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Kết quả so checksum và số frame. |
  | Cách kiểm tra | Đối chiếu pixel checksum tương ứng và metadata decoded. |
  | Tiêu chí đạt | Mỗi ca 720 frame 640×360, checksum pixel trùng; chênh lệch phải được điều tra. |
  | Kết quả | Chưa validate. |

- [ ] **1.5.2.3.** Phân biệt phạm vi so sánh: không yêu cầu toàn file H.264 chỉ khác một SEI;
  metadata mô tả encoder/options cũng có thể khác. Không so checksum ảnh lossy
  output với input gốc.

  **Validate output — 1.5.2.3**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Ghi chú phạm vi kết luận pixel/bitstream. |
  | Cách kiểm tra | Rà cách so sánh và diễn giải kết quả. |
  | Tiêu chí đạt | Chỉ so pixel hai ca x264; không so lossy output với input, không yêu cầu bitstream chỉ khác một SEI. |
  | Kết quả | Chưa validate. |

### 1.6. Experiment 2 — Cùng SBS input, so sánh với x265 — TODO

- [ ] **1.6.1.** Chỉ thực hiện sau khi experiment 1 đã có bằng chứng SEI rõ ràng.
  Dùng CLI x265 local để tránh nhầm với thư viện của FFmpeg hệ thống:

  ```shell
  ./build_dev.sh > issues/970_worklog/step1/1.6.1_x265_build.log 2>&1
  ./build/debug/x265 --version > issues/970_worklog/step1/1.6.1_x265_version.txt 2>&1

  ./build/debug/x265 \
    --y4m --frames 720 --preset ultrafast --qp 28 \
    --frame-threads 1 --pools none --no-wpp --bframes 0 \
    --keyint 24 --min-keyint 24 --no-scenecut --no-asm \
    -o issues/970_worklog/step1/1.6.1_x265-sbs.h265 \
    issues/970_worklog/step1/1.3.1_sbs-test.y4m \
    > issues/970_worklog/step1/1.6.1_x265-sbs.log 2>&1
  ```

  **Validate output — 1.6.1**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Binary/version x265 local, bitstream và log build/encode. |
  | Cách kiểm tra | Xác nhận bằng chứng FPA x264 đã có; kiểm tra binary, input, exit code và log encode. |
  | Tiêu chí đạt | Build/encode thành công 720 frame từ cùng fixture bằng CLI local. |
  | Kết quả | Chưa validate. |

- [ ] **1.6.2.** Parse bitstream và kiểm tra decode:

  ```shell
  ffmpeg -nostdin -hide_banner -i issues/970_worklog/step1/1.6.1_x265-sbs.h265 \
    -map 0:v:0 -c:v copy -bsf:v trace_headers -f null - \
    > issues/970_worklog/step1/1.6.2_x265-sbs_headers.log 2>&1

  ffmpeg -nostdin -v error -i issues/970_worklog/step1/1.6.1_x265-sbs.h265 -f null -
  ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=codec_name,width,height,pix_fmt,nb_read_frames \
    -of json issues/970_worklog/step1/1.6.1_x265-sbs.h265 \
    > issues/970_worklog/step1/1.6.2_x265-sbs_decoded.json
  ```

  **Validate output — 1.6.2**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Log parser HEVC, decode và metadata decoded. |
  | Cách kiểm tra | Parse bằng công cụ hỗ trợ FPA HEVC; decode và count frame. |
  | Tiêu chí đạt | Parser/decode thành công; đủ 720 frame 640×360, có bằng chứng về sự có mặt/vắng mặt FPA. |
  | Kết quả | Chưa validate. |

- [ ] **1.6.3.** Lưu thêm command `./build/debug/x265 --frame-packing 3` cùng stderr và exit
  code để xác nhận CLI hiện có nhận option không. Đây là phép thử option riêng,
  không thay cho việc encode và đọc bitstream.

  **Validate output — 1.6.3**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Command thử option, stderr và exit code. |
  | Cách kiểm tra | Đọc thông báo CLI và xác định nguyên nhân kết thúc. |
  | Tiêu chí đạt | Xác nhận rõ CLI nhận/từ chối option; không chỉ dựa vào exit code khác 0. |
  | Kết quả | Chưa validate. |

- [ ] **1.6.4.** So sánh sự có mặt và nội dung FPA giữa x264 type 3 và x265 baseline.
  Không so byte stream hoặc checksum pixel giữa H.264 và HEVC để kết luận lỗi:
  hai encoder/codec khác nhau có thể tạo ảnh tái dựng khác nhau tại cùng QP.
  Kết luận chỉ trong phạm vi binary và command đã kiểm tra.

  **Validate output — 1.6.4**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | So sánh signalling x264 type 3 với x265 baseline. |
  | Cách kiểm tra | Đối chiếu log parser và phiên bản/command đã thử. |
  | Tiêu chí đạt | Kết luận có căn cứ FPA; không dùng chênh lệch byte/pixel giữa hai codec để kết luận lỗi. |
  | Kết quả | Chưa validate. |

### 1.7. Ghi kết quả và chốt requirement — TODO

#### 1.7.1. Tạo `issues/970_worklog/step1/1.7.1.1_notes.md`

- [ ] **1.7.1.1.** Ghi input hash, metadata, phiên bản binary và command thực tế.

  **Validate output — 1.7.1.1**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Phần input/công cụ/command trong `1.7.1.1_notes.md`. |
  | Cách kiểm tra | Đối chiếu hash, metadata, version và command với artifact. |
  | Tiêu chí đạt | Thông tin đúng dữ liệu thực tế, đủ đường dẫn tái lập. |
  | Kết quả | Chưa validate. |

- [ ] **1.7.1.2.** Đính kèm ảnh SBS giả và giải thích tại sao nó không có chiều sâu stereo thật.

  **Validate output — 1.7.1.2**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Ảnh và giải thích SBS giả trong notes. |
  | Cách kiểm tra | Mở liên kết ảnh, đọc mô tả hai view. |
  | Tiêu chí đạt | Ảnh truy cập được; nêu rõ không có disparity/chiều sâu stereo. |
  | Kết quả | Chưa validate. |

- [ ] **1.7.1.3.** Lập bảng control x264 / x264 type 3 / x265 baseline: codec, FPA, arrangement
  type, lịch phát, số frame decode, exit code và liên kết log.

  **Validate output — 1.7.1.3**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Bảng ba ca trong notes. |
  | Cách kiểm tra | Đối chiếu codec, FPA/type, lịch phát, frame, exit code và log. |
  | Tiêu chí đạt | Đủ ba ca, mọi kết luận có artifact tương ứng. |
  | Kết quả | Chưa validate. |

- [ ] **1.7.1.4.** Ghi bằng chứng checksum ảnh của hai ca x264 và khác biệt metadata quan sát được.

  **Validate output — 1.7.1.4**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Bằng chứng checksum và metadata trong notes. |
  | Cách kiểm tra | Đối chiếu framemd5 và log parser hai ca x264. |
  | Tiêu chí đạt | Ghi đúng kết quả pixel và khác biệt metadata; vấn đề chưa giải thích phải nêu rõ. |
  | Kết quả | Chưa validate. |

- [ ] **1.7.1.5.** Chốt requirement rút ra từ experiment và câu hỏi còn mở cho phần đọc code.

  **Validate output — 1.7.1.5**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Requirement và câu hỏi mở trong notes. |
  | Cách kiểm tra | Rà kết luận với thí nghiệm đã chạy. |
  | Tiêu chí đạt | Tách rõ yêu cầu rút ra, điều đã biết và câu hỏi cần đọc code. |
  | Kết quả | Chưa validate. |

#### 1.7.2. Kiểm tra tiêu chí hoàn thành Step 01

- [ ] **1.7.2.1.** Tạo lại cùng SBS fixture từ MP4 đã tải; phân biệt metadata SBS, pixel SBS
  và stereo có chiều sâu thực sự.

  **Validate output — 1.7.2.1**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Bằng chứng fixture tái lập được và phân biệt các khái niệm SBS. |
  | Cách kiểm tra | Rà command tạo fixture, metadata/pixel và giải thích trong notes. |
  | Tiêu chí đạt | Có thông tin tái lập cùng fixture; phân biệt pixel SBS, metadata SBS và stereo thật. |
  | Kết quả | Chưa validate. |

- [ ] **1.7.2.2.** Encode thành công hai ca x264 chỉ khác option frame packing.

  **Validate output — 1.7.2.2**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Bằng chứng hoàn thành hai ca encode x264. |
  | Cách kiểm tra | Rà command và log ở 1.4. |
  | Tiêu chí đạt | Cả hai encode thành công; tham số encode chỉ khác frame packing. |
  | Kết quả | Chưa validate. |

- [ ] **1.7.2.3.** Chứng minh bằng parser rằng ca feature có FPA/type 3, control không có.

  **Validate output — 1.7.2.3**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Bằng chứng FPA/type 3 từ parser. |
  | Cách kiểm tra | Rà bảng kết quả và log ở 1.5.1. |
  | Tiêu chí đạt | Feature có FPA/type 3, control không có; parser đã xác nhận hỗ trợ. |
  | Kết quả | Chưa validate. |

- [ ] **1.7.2.4.** Xác nhận ảnh decode của hai ca x264, số frame và trạng thái decode.

  **Validate output — 1.7.2.4**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Bằng chứng decode và pixel hai ca x264. |
  | Cách kiểm tra | Rà số frame, trạng thái decode và checksum ở 1.5.2. |
  | Tiêu chí đạt | Decode đủ 720 frame mỗi ca, pixel trùng; không còn chênh lệch ảnh hưởng kết luận chưa giải thích. |
  | Kết quả | Chưa validate. |

- [ ] **1.7.2.5.** Đối chiếu x265 trên cùng input và mô tả chính xác phần signalling thiếu.

  **Validate output — 1.7.2.5**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Kết luận signalling x265 còn thiếu. |
  | Cách kiểm tra | Rà đối chiếu ở 1.6 và bằng chứng trong notes. |
  | Tiêu chí đạt | Mô tả chính xác theo binary/command đã thử, đủ log hỗ trợ. |
  | Kết quả | Chưa validate. |

## Sau Step 01

Chỉ chi tiết hóa step tiếp theo sau khi có artifact của thí nghiệm black-box.
Mỗi step có command tái lập, artifact và tiêu chí hoàn thành. Chưa mở rộng
sang FFmpeg wrapper hoặc kiểm tra TV trong thí nghiệm metadata đầu tiên.

### 2.1. Đọc/debug x264

- [ ] **2.1.1.** Theo luồng CLI → param → validation → SEI writer → NAL.

  **Validate output — 2.1.1**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Ghi chép luồng CLI → NAL. |
  | Cách kiểm tra | Theo dõi code/debug sau Step 01. |
  | Tiêu chí đạt | Có tham chiếu cho từng chặng CLI, param, validation, writer và NAL. |
  | Kết quả | Chưa validate. |

- [ ] **2.1.2.** Giải thích các field và lịch phát đã quan sát trong Step 01.

  **Validate output — 2.1.2**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Giải thích field và lịch phát đã quan sát. |
  | Cách kiểm tra | Đối chiếu code/runtime với artifact Step 01. |
  | Tiêu chí đạt | Mỗi giải thích có bằng chứng; điểm chưa rõ được ghi riêng. |
  | Kết quả | Chưa validate. |

- [ ] **2.1.3.** Mở rộng type 4/5, parity, B-frame và ảnh hưởng reference list.

  **Validate output — 2.1.3**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Kết quả mở rộng type 4/5, parity, B-frame và reference list. |
  | Cách kiểm tra | Chốt ca thử sau Step 01, đối chiếu runtime/bitstream. |
  | Tiêu chí đạt | Các trường hợp được kiểm chứng hoặc ghi rõ giới hạn chưa kiểm chứng. |
  | Kết quả | Chưa validate. |

- [ ] **2.1.4.** Đọc source có sẵn tại `./x264`: `x264.c`, `common/base.c`, `x264.h`,
  `encoder/encoder.c` và `encoder/set.c` (`x264_sei_frame_packing_write`).

  **Validate output — 2.1.4**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Ghi chú các vị trí source liên quan. |
  | Cách kiểm tra | Đọc các file liệt kê và xác định writer/caller. |
  | Tiêu chí đạt | Có vị trí code và vai trò của từng file trong luồng frame packing. |
  | Kết quả | Chưa validate. |

### 2.2. Đối chiếu chuẩn và chốt thiết kế x265

- [ ] **2.2.1.** Đối chiếu H.264/H.265 syntax và semantics bằng tài liệu chuẩn.
  Không sao chép nguyên payload H.264 sang HEVC khi chưa đối chiếu syntax
  và persistence/cancel semantics.

  **Validate output — 2.2.1**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Bảng đối chiếu syntax/semantics H.264 và HEVC. |
  | Cách kiểm tra | Đối chiếu tài liệu chuẩn, gồm persistence/cancel. |
  | Tiêu chí đạt | Các khác biệt có dẫn chiếu; không sao chép payload khi chưa xác minh. |
  | Kết quả | Chưa validate. |

- [ ] **2.2.2.** Chốt param/API/CLI và lịch phát SEI phía x265.

  **Validate output — 2.2.2**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Thiết kế param/API/CLI và lịch phát x265. |
  | Cách kiểm tra | Đối chiếu requirement và bảng chuẩn. |
  | Tiêu chí đạt | Chốt phạm vi, validation tham số, lịch phát và tiêu chí kiểm thử. |
  | Kết quả | Chưa validate. |

### 2.3. Triển khai và validation

- [ ] **2.3.1.** Triển khai, kiểm tra bitstream và regression khi tắt option.

  **Validate output — 2.3.1**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Patch và kết quả kiểm tra bitstream/regression. |
  | Cách kiểm tra | Chạy ca bật/tắt option theo thiết kế đã chốt. |
  | Tiêu chí đạt | Bitstream đúng thiết kế; regression khi tắt option đạt. |
  | Kết quả | Chưa validate. |

- [ ] **2.3.2.** Validation với stereo thật và thiết bị hỗ trợ HEVC/FPA.

  **Validate output — 2.3.2**

  | Nội dung | Chi tiết |
  | --- | --- |
  | Output | Báo cáo nguồn stereo thật và thiết bị HEVC/FPA. |
  | Cách kiểm tra | Thử thực tế, ghi thiết bị, command, bitstream và hành vi. |
  | Tiêu chí đạt | Có bằng chứng thiết bị xử lý đúng signalling; chưa thử thì không đánh dấu đạt. |
  | Kết quả | Chưa validate. |

