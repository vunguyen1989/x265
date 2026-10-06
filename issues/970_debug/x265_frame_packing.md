# x265 frame packing type 3

Implementation reference: HM `SEIEncoder::initSEIFramePacking`,
`SEIWriter::xWriteSEIFramePacking`, and
`TEncGOP::xCreateIRAPLeadingSEIMessages` in `h265nal/HM/source/Lib/TLibEncoder`.
Syntax/semantics were also checked against the local H.265 (12/2016) text,
sections D.2.16 and D.3.16, including Table D.9.

`--frame-packing 3` signals pre-packed side-by-side input, left view first.
Default/API disable value is -1. Unsupported types and malformed values fail.
The persistent prefix SEI is emitted at every IRAP (IDR or CRA), before VCL.
Its fields follow HM with ID 0, quincunx 0, interpretation 1, persistence 1,
upsampled aspect ratio 0, and all remaining flags/grid/reserved values 0.
The payload uses HEVC persistence, not H.264's repetition-period syntax.

The new public `x265_param::framePacking` field is appended to the struct;
X265_BUILD increases from 217 to 218. Rebuild library consumers against the
updated header. Parameter defaults, parsing, copying, serialization and CLI
help are included. This setting is selected at encoder creation.

Build and integration check from repository root:

```bash
./build_dev.sh
python3 issues/970_debug/test_x265_frame_packing.py
```

The integration check covers disabled/default signalling, open and closed GOP,
SEI grouping with/without repeated headers, every field and IRAP placement,
decoded frame checksums, and rejected values. It uses the existing SBS fixture,
FFmpeg trace_headers and framemd5; temporary streams are removed automatically.

The launch configuration's 720-frame command was encoded and decoded, with
30 FPA messages at 30 IRAPs. A one-frame HM encode of the same SBS fixture
provided the field comparison saved in `x265_hm_fpa_comparison.json`.
Comparison is of syntax/field values, not encoder output bytes.
No physical TV validation has been performed for this implementation.

Validation results are saved in `x265_frame_packing_validation.json`: the
48-frame check and the final build's 25-frame check both passed.
