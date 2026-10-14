/*****************************************************************************
* Copyright (C) 2013-2020 MulticoreWare, Inc
*
* Authors: Steve Borho <steve@borho.org>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
*
* This program is also available under a commercial proprietary license.
* For more information, contact us at license @ x265.com.
*****************************************************************************/

#include "common.h"
#include "bitstream.h"
#include "slice.h"
#include "sei.h"
#include <new>

using namespace X265_NS;

/* x265's identifying GUID */
const uint8_t SEIuserDataUnregistered::m_uuid_iso_iec_11578[16] = {
    0x2C, 0xA2, 0xDE, 0x09, 0xB5, 0x17, 0x47, 0xDB,
    0xBB, 0x55, 0xA4, 0xFE, 0x7F, 0xC2, 0xFC, 0x4E
};

PrefixSeiWriter::PrefixSeiWriter()
    : m_buffer(NULL), m_messageCount(0), m_layerId(0), m_singleSei(false),
      m_finalized(true), m_failed(false)
{}

PrefixSeiWriter::~PrefixSeiWriter()
{
    delete m_buffer;
}

void PrefixSeiWriter::begin(bool singleSei)
{
    X265_CHECK(m_finalized, "prefix SEI group is still open\n");
    if (!m_finalized)
    {
        m_failed = true;
        return;
    }

    m_singleSei = singleSei;
    m_messageCount = 0;
    m_finalized = false;
    m_failed = false;
    // Chưa cấp phát/reset buffer: chỉ cần buffer riêng khi append single-SEI.
}

void PrefixSeiWriter::append(SEI& sei, Bitstream& bs, const SPS& sps, NALList& list, int layerId)
{
    X265_CHECK(!m_finalized, "prefix SEI group has already been finalized\n");
    if (m_finalized || m_failed)
    {
        m_failed = true;
        return;
    }

    if (!m_singleSei)
    {
        // Không gộp: giữ nguyên reset/ghi/serialize mỗi message bằng buffer caller.
        sei.writeSEImessages(bs, sps, NAL_UNIT_PREFIX_SEI, list, false, layerId);
        return;
    }

    // Một NAL chỉ có một layerId. Đổi layer thì xuất nhóm cũ trước khi gom tiếp.
    if (m_messageCount && m_layerId != layerId && !flushPending(list))
        return;

    if (!m_buffer)
        m_buffer = new (std::nothrow) Bitstream;
    if (!m_buffer || !m_buffer->getFIFO())
    {
        x265_log(NULL, X265_LOG_ERROR, "Unable to allocate prefix SEI buffer\n");
        m_failed = true;
        return;
    }

    if (!m_messageCount)
    {
        /* Append đầu tiên mở nội dung nhóm bằng buffer riêng sạch.
         * AUD/parameter sets tiếp tục dùng bs, không thể xóa SEI đang gom.
         * Các append tiếp theo không reset để giữ messages trước đó. */
        m_buffer->resetBits();
        m_layerId = layerId;
    }

    uint32_t previousBits = m_buffer->getNumberOfWrittenBits();
    sei.writeSEImessages(*m_buffer, sps, NAL_UNIT_PREFIX_SEI, list, true, layerId);
    if (m_buffer->getNumberOfWrittenBits() <= previousBits)
    {
        m_failed = true;
        return;
    }
    m_messageCount++;
}

bool PrefixSeiWriter::flushPending(NALList& list)
{
    if (m_failed)
        return false;
    if (!m_messageCount)
        return true;
    if (list.m_numNal >= NALList::MAX_NAL_UNITS)
    {
        x265_log(NULL, X265_LOG_ERROR, "Too many NAL units for prefix SEI\n");
        m_failed = true;
        return false;
    }

    // Kết thúc một NAL gộp đúng một lần; SEI hiện dùng temporalID = 1.
    m_buffer->writeByteAlignment();
    uint32_t previousNals = list.m_numNal;
    list.serialize(NAL_UNIT_PREFIX_SEI, *m_buffer, m_layerId, 1);
    // serialize trả void; xác nhận NAL đã được thêm trước khi bỏ nhóm đang gom.
    if (list.m_numNal != previousNals + 1)
    {
        m_failed = true;
        return false;
    }
    m_messageCount = 0;
    return true;
}

bool PrefixSeiWriter::flush(NALList& list)
{
    if (m_finalized)
        return !m_failed;
    bool success = flushPending(list);
    m_finalized = true;
    return success;
}

/* marshal a single SEI message sei, storing the marshalled representation
* in bitstream bs */
void SEI::writeSEImessages(Bitstream& bs, const SPS& sps, NalUnitType nalUnitType, NALList& list, int isNested, int layer)
{
    /* Reset SEI - bs là buffer của caller (có thể là FrameEncoder::m_bs).
     * isNested = 0: message tạo một NAL riêng, nên bỏ nội dung NAL trước
     * (AUD/PPS/SEI/slice header...) rồi ghi payload_type, payload_size và payload.
     * Cuối hàm sẽ căn chỉnh nếu cần và serialize ngay. NAL trước đã nằm trong
     * list thì không bị xóa; reset chỉ làm rỗng buffer tạm, giữ lại allocation.
     * isNested != 0: không reset vì phải append vào messages đang tích lũy,
     * cũng không serialize tại cuối hàm; caller quản lý đầu/cuối nhóm SEI.
     * PrefixSeiWriter truyền true với buffer riêng khi gộp Prefix SEI,
     * truyền false với buffer caller khi không gộp. Reset giữa các messages
     * đang gom sẽ làm mất message cũ.
     * writeTrailingSEIMessages luôn truyền false nên suffix hash tự reset,
     * ghi và serialize riêng, không phụ thuộc tùy chọn gộp prefix SEI. */
    if (!isNested)
        bs.resetBits();

    // Lượt này chỉ đếm kích thước bằng BitCounter, chưa ghi payload vào bs.
    BitCounter counter;
    m_bitIf = &counter;
    writeSEI(sps);
    /* count the size of the payload and return the size in bits */
    X265_CHECK(0 == (counter.getNumberOfWrittenBits() & 7), "payload unaligned\n");
    uint32_t payloadData = counter.getNumberOfWrittenBits() >> 3;

    // set bitstream
    m_bitIf = &bs;

    uint32_t payloadType = m_payloadType;
    for (; payloadType >= 0xff; payloadType -= 0xff)
        WRITE_CODE(0xff, 8, "payload_type");
    WRITE_CODE(payloadType, 8, "payload_type");

    uint32_t payloadSize = payloadData;
    for (; payloadSize >= 0xff; payloadSize -= 0xff)
        WRITE_CODE(0xff, 8, "payload_size");
    WRITE_CODE(payloadSize, 8, "payload_size");

    // virtual writeSEI method, write to bs 
    writeSEI(sps);

    if (!isNested)
    {
        /* NAL riêng kết thúc ở đây. serialize không reset bs: dữ liệu vẫn
         * còn trong buffer tạm cho đến reset tiếp theo của caller/hàm này.
         * Nhóm SEI gộp bỏ qua block này để caller còn append message khác. */
        if (nalUnitType != NAL_UNIT_UNSPECIFIED)
            bs.writeByteAlignment();
        list.serialize(nalUnitType, bs, layer, (1 + (nalUnitType == NAL_UNIT_CODED_SLICE_TSA_N)));
    }
}

void SEI::writeByteAlign()
{
    // TODO: expose bs.writeByteAlignment() as virtual function
    if (m_bitIf->getNumberOfWrittenBits() % 8 != 0)
    {
        WRITE_FLAG(1, "bit_equal_to_one");
        while (m_bitIf->getNumberOfWrittenBits() % 8 != 0)
        {
            WRITE_FLAG(0, "bit_equal_to_zero");
        }
    }
}

void SEI::setSize(uint32_t size)
{
    m_payloadSize = size;
}

/* charSet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" */

char* SEI::base64Decode(char encodedString[], int base64EncodeLength, char* decodedString)
{
    int i, j, k = 0;
    // stores the bitstream
    int bitstream = 0;
    // countBits stores the current number of bits in bitstream
    int countBits = 0;

    for (i = 0; i < base64EncodeLength; i += 4)
    {
        bitstream = 0;
        countBits = 0;

        for (j = 0; j < 4; j++)
        {
            if (encodedString[i + j] != '=')
            {
                int value = 0;
                if (encodedString[i + j] >= 'A' && encodedString[i + j] <= 'Z')
                    value = encodedString[i + j] - 'A';
                else if (encodedString[i + j] >= 'a' && encodedString[i + j] <= 'z')
                    value = encodedString[i + j] - 'a' + 26;
                else if (encodedString[i + j] >= '0' && encodedString[i + j] <= '9')
                    value = encodedString[i + j] - '0' + 52;
                else if (encodedString[i + j] == '+')
                    value = 62;
                else if (encodedString[i + j] == '/')
                    value = 63;
                else
                    value = 0;

                bitstream = (bitstream << 6) | value;
                countBits += 6;
            }
        }

        while (countBits >= 8)
        {
            countBits -= 8;
            decodedString[k++] = (bitstream >> countBits) & 0xFF;
        }
    }

    if (k < base64EncodeLength)
    {
        decodedString[k] = '\0';
    }

    return decodedString;
}

