/*****************************************************************************
 * Copyright (C) 2013-2020 MulticoreWare, Inc
 *
 * Authors: Chung Shin Yee <shinyee@multicorewareinc.com>
 *          Min Chen <chenm003@163.com>
 *          Steve Borho <steve@borho.org>
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
#include "frame.h"
#include "framedata.h"
#include "wavefront.h"
#include "param.h"

#include "encoder.h"
#include "frameencoder.h"
#include "common.h"
#include "slicetype.h"
#include "nal.h"
#include "temporalfilter.h"
#include "flowlog.h"

#include <iostream>

namespace X265_NS {
void weightAnalyse(Slice& slice, Frame& frame, x265_param& param);

FrameEncoder::FrameEncoder()
{
    m_reconfigure = false;
    m_isFrameEncoder = true;
    m_threadActive = true;
    m_activeWorkerCount = 0;
    m_completionCount = 0;
    m_outStreams = NULL;
    m_backupStreams = NULL;
    m_substreamSizes = NULL;
    m_nr = NULL;
    m_tld = NULL;
    m_rows = NULL;
    m_top = NULL;
    m_param = NULL;
    m_cuGeoms = NULL;
    m_ctuGeomMap = NULL;
    m_localTldIdx = 0;
    memset(&m_rce, 0, sizeof(RateControlEntry));
    for (int layer = 0; layer < MAX_LAYERS; layer++)
    {
        m_prevOutputTime[layer] = x265_mdate();
        m_slicetypeWaitTime[layer] = 0;
        m_frame[layer] = NULL;
    }
}

void FrameEncoder::destroy()
{
    if (m_pool)
    {
        if (!m_jpId)
        {
            int numTLD = m_pool->m_numWorkers;
            if (!m_param->bEnableWavefront)
                numTLD += m_pool->m_numProviders;
            for (int i = 0; i < numTLD; i++)
                m_tld[i].destroy();
            delete [] m_tld;
        }
    }
    else
    {
        m_tld->destroy();
        delete m_tld;
    }

    delete[] m_rows;
    delete[] m_outStreams;
    delete[] m_backupStreams;
    X265_FREE(m_sliceBaseRow);
    X265_FREE((void*)m_bAllRowsStop);
    X265_FREE((void*)m_vbvResetTriggerRow);
    X265_FREE(m_sliceMaxBlockRow);
    X265_FREE(m_cuGeoms);
    X265_FREE(m_ctuGeomMap);
    X265_FREE(m_substreamSizes);
    X265_FREE(m_nr);
    X265_FREE(m_retFrameBuffer);

    m_frameFilter.destroy();

    if (m_param->bEmitHRDSEI || !!m_param->interlaceMode)
    {
        delete m_rce.picTimingSEI;
        delete m_rce.hrdTiming;
    }
}

bool FrameEncoder::init(Encoder *top, int numRows, int numCols)
{
    m_top = top;
    m_param = top->m_param;
    m_numRows = numRows;
    m_numCols = numCols;
    m_reconfigure = false;
    m_filterRowDelay = ((m_param->bEnableSAO && m_param->bSaoNonDeblocked)
                        || (!m_param->bEnableLoopFilter && m_param->bEnableSAO)) ?
                        2 : (m_param->bEnableSAO || m_param->bEnableLoopFilter ? 1 : 0);
    m_filterRowDelayCus = m_filterRowDelay * numCols;
    m_rows = new CTURow[m_numRows];
    bool ok = !!m_numRows;

    m_sliceBaseRow = X265_MALLOC(uint32_t, m_param->maxSlices + 1);
    m_bAllRowsStop = X265_MALLOC(bool, m_param->maxSlices);
    m_vbvResetTriggerRow = X265_MALLOC(int, m_param->maxSlices);
    ok &= !!m_sliceBaseRow;
    m_sliceGroupSize = (uint16_t)(m_numRows + m_param->maxSlices - 1) / m_param->maxSlices;
    uint32_t sliceGroupSizeAccu = (m_numRows << 8) / m_param->maxSlices;    
    uint32_t rowSum = sliceGroupSizeAccu;
    uint32_t sidx = 0;
    for (uint32_t i = 0; i < m_numRows; i++)
    {
        const uint32_t rowRange = (rowSum >> 8);
        if ((i >= rowRange) & (sidx != m_param->maxSlices - 1))
        {
            rowSum += sliceGroupSizeAccu;
            m_sliceBaseRow[++sidx] = i;
        }
    }
    X265_CHECK(sidx < m_param->maxSlices, "sliceID check failed!");
    m_sliceBaseRow[0] = 0;
    m_sliceBaseRow[m_param->maxSlices] = m_numRows;

    m_sliceMaxBlockRow = X265_MALLOC(uint32_t, m_param->maxSlices + 1);
    ok &= !!m_sliceMaxBlockRow;
    uint32_t maxBlockRows = (m_param->sourceHeight + (16 - 1)) / 16;
    sliceGroupSizeAccu = (maxBlockRows << 8) / m_param->maxSlices;
    rowSum = sliceGroupSizeAccu;
    sidx = 0;
    for (uint32_t i = 0; i < maxBlockRows; i++)
    {
        const uint32_t rowRange = (rowSum >> 8);
        if ((i >= rowRange) & (sidx != m_param->maxSlices - 1))
        {
            rowSum += sliceGroupSizeAccu;
            m_sliceMaxBlockRow[++sidx] = i;
        }
    }
    m_sliceMaxBlockRow[0] = 0;
    m_sliceMaxBlockRow[m_param->maxSlices] = maxBlockRows;

    /* determine full motion search range */
    int range  = m_param->searchRange;       /* fpel search */
    range += !!(m_param->searchMethod < 2);  /* diamond/hex range check lag */
    range += NTAPS_LUMA / 2;                 /* subpel filter half-length */
    range += 2 + (MotionEstimate::hpelIterationCount(m_param->subpelRefine) + 1) / 2; /* subpel refine steps */
    m_refLagRows = /*(m_param->maxSlices > 1 ? 1 : 0) +*/ 1 + ((range + m_param->maxCUSize - 1) / m_param->maxCUSize);

    // NOTE: 2 times of numRows because both Encoder and Filter in same queue
    if (!WaveFront::init(m_numRows * 2))
    {
        x265_log(m_param, X265_LOG_ERROR, "unable to initialize wavefront queue\n");
        m_pool = NULL;
    }

    m_frameFilter.init(top, this, numRows, numCols);

    // initialize HRD parameters of SPS
    if (m_param->bEmitHRDSEI || !!m_param->interlaceMode)
    {
        m_rce.picTimingSEI = new SEIPictureTiming;
        m_rce.hrdTiming = new HRDTiming;

        ok &= m_rce.picTimingSEI && m_rce.hrdTiming;
    }

    if (m_param->noiseReductionIntra || m_param->noiseReductionInter)
        m_nr = X265_MALLOC(NoiseReduction, 1);
    if (m_nr)
        memset(m_nr, 0, sizeof(NoiseReduction));
    else
        m_param->noiseReductionIntra = m_param->noiseReductionInter = 0;

    // 7.4.7.1 - Ceil( Log2( PicSizeInCtbsY ) ) bits
    {
        unsigned long tmp;
        BSR(tmp, (numRows * numCols - 1));
        m_sliceAddrBits = (uint16_t)(tmp + 1);
    }

    m_tmeDeps.resize(m_numRows);

    m_retFrameBuffer = X265_MALLOC(Frame*, m_param->numLayers);
    for (int layer = 0; layer < m_param->numLayers; layer++)
        m_retFrameBuffer[layer] = NULL;
    return ok;
}

/* Generate a complete list of unique geom sets for the current picture dimensions */
bool FrameEncoder::initializeGeoms()
{
    /* Geoms only vary between CTUs in the presence of picture edges */
    int maxCUSize = m_param->maxCUSize;
    int minCUSize = m_param->minCUSize;
    int heightRem = m_param->sourceHeight & (maxCUSize - 1);
    int widthRem = m_param->sourceWidth & (maxCUSize - 1);
    int allocGeoms = 1; // body
    if (heightRem && widthRem)
        allocGeoms = 4; // body, right, bottom, corner
    else if (heightRem || widthRem)
        allocGeoms = 2; // body, right or bottom

    m_ctuGeomMap = X265_MALLOC(uint32_t, m_numRows * m_numCols);
    m_cuGeoms = X265_MALLOC(CUGeom, allocGeoms * CUGeom::MAX_GEOMS);
    if (!m_cuGeoms || !m_ctuGeomMap)
        return false;

    // body
    CUData::calcCTUGeoms(maxCUSize, maxCUSize, maxCUSize, minCUSize, m_cuGeoms);
    memset(m_ctuGeomMap, 0, sizeof(uint32_t) * m_numRows * m_numCols);
    if (allocGeoms == 1)
        return true;

    int countGeoms = 1;
    if (widthRem)
    {
        // right
        CUData::calcCTUGeoms(widthRem, maxCUSize, maxCUSize, minCUSize, m_cuGeoms + countGeoms * CUGeom::MAX_GEOMS);
        for (uint32_t i = 0; i < m_numRows; i++)
        {
            uint32_t ctuAddr = m_numCols * (i + 1) - 1;
            m_ctuGeomMap[ctuAddr] = countGeoms * CUGeom::MAX_GEOMS;
        }
        countGeoms++;
    }
    if (heightRem)
    {
        // bottom
        CUData::calcCTUGeoms(maxCUSize, heightRem, maxCUSize, minCUSize, m_cuGeoms + countGeoms * CUGeom::MAX_GEOMS);
        for (uint32_t i = 0; i < m_numCols; i++)
        {
            uint32_t ctuAddr = m_numCols * (m_numRows - 1) + i;
            m_ctuGeomMap[ctuAddr] = countGeoms * CUGeom::MAX_GEOMS;
        }
        countGeoms++;

        if (widthRem)
        {
            // corner
            CUData::calcCTUGeoms(widthRem, heightRem, maxCUSize, minCUSize, m_cuGeoms + countGeoms * CUGeom::MAX_GEOMS);

            uint32_t ctuAddr = m_numCols * m_numRows - 1;
            m_ctuGeomMap[ctuAddr] = countGeoms * CUGeom::MAX_GEOMS;
            countGeoms++;
        }
        X265_CHECK(countGeoms == allocGeoms, "geometry match check failure\n");
    }

    return true;
}

bool FrameEncoder::startCompressFrame(Frame* curFrame[MAX_LAYERS])
{
    for (int layer = 0; layer < m_param->numLayers; layer++)
    {
        m_slicetypeWaitTime[layer] = x265_mdate() - m_prevOutputTime[layer];
        m_frame[layer] = curFrame[layer];
        curFrame[layer]->m_encData->m_frameEncoderID = m_jpId;
        curFrame[layer]->m_encData->m_jobProvider = this;
        curFrame[layer]->m_encData->m_slice->m_mref = m_mref;
    }
    m_sliceType.set(curFrame[0]->m_lowres.sliceType);
    X265_FLOW_LOG("PassEncoder",
                  "[CHANNEL channel=frame-start encoder=%d frame=%d] PUBLISH object=Frame@%p event=enable owner_before=Encoder owner_after=FrameEncoder-%d",
                  m_jpId, curFrame[0]->m_poc, curFrame[0], m_jpId);

    if (!m_cuGeoms)
    {
        if (!initializeGeoms())
            return false;
    }

    m_enable.trigger();
    return true;
}

void FrameEncoder::threadMain()
{
    THREAD_NAME("Frame", m_jpId);
    X265_FLOW_LOG("FrameEncoder",
                  "[THREAD worker=FrameEncoder-%d role=frame-worker] START pool=%p owner=FrameEncoder",
                  m_jpId, m_pool);

    if (m_pool)
    {
        m_pool->setCurrentThreadAffinity();

        /* the first FE on each NUMA node is responsible for allocating thread
         * local data for all worker threads in that pool. If WPP is disabled, then
         * each FE also needs a TLD instance */
        if (!m_jpId)
        {
            int numTLD = m_pool->m_numWorkers;
            if (!m_param->bEnableWavefront)
                numTLD += m_pool->m_numProviders;

            m_tld = new ThreadLocalData[numTLD];
            for (int i = 0; i < numTLD; i++)
            {
                m_tld[i].analysis.initSearch(*m_param, m_top->m_scalingList);
                m_tld[i].analysis.create(m_tld);
            }

            for (int i = 0; i < m_pool->m_numProviders; i++)
            {
                if (m_pool->m_jpTable[i]->m_isFrameEncoder) /* ugh; over-allocation and other issues here */
                {
                    FrameEncoder *peer = dynamic_cast<FrameEncoder*>(m_pool->m_jpTable[i]);
                    peer->m_tld = m_tld;
                }
            }
        }

        if (m_param->bEnableWavefront)
            m_localTldIdx = -1; // cause exception if used
        else
            m_localTldIdx = m_pool->m_numWorkers + m_jpId;
    }
    else
    {
        m_tld = new ThreadLocalData;
        m_tld->analysis.initSearch(*m_param, m_top->m_scalingList);
        m_tld->analysis.create(NULL);
        m_localTldIdx = 0;
    }

    m_done.trigger();     /* signal that thread is initialized */
    X265_FLOW_LOG("FrameEncoder",
                  "[CHANNEL channel=frame-start encoder=%d] WAIT_BEGIN reason=no-frame owner=FrameEncoder-%d",
                  m_jpId, m_jpId);
    m_enable.wait();      /* Encoder::encode() triggers this event */
    X265_FLOW_LOG("FrameEncoder",
                  "[CHANNEL channel=frame-start encoder=%d frame=%d] WAIT_END reason=frame-ready object=Frame@%p owner=FrameEncoder-%d",
                  m_jpId, m_frame[0] ? m_frame[0]->m_poc : -1, m_frame[0], m_jpId);

    while (m_threadActive)
    {
        if (m_param->bCTUInfo)
        {
            while (!m_frame[0]->m_ctuInfo)
                m_frame[0]->m_copied.wait();
        }
        if ((m_param->bAnalysisType == AVC_INFO) && !strlen(m_param->analysisSave) && !strlen(m_param->analysisLoad) && !(IS_X265_TYPE_I(m_frame[0]->m_lowres.sliceType)))
        {
            while (((m_frame[0]->m_analysisData.interData == NULL && m_frame[0]->m_analysisData.intraData == NULL) || (uint32_t)m_frame[0]->m_poc != m_frame[0]->m_analysisData.poc))
                m_frame[0]->m_copyMVType.wait();
        }

        for (int layer = 0; layer < m_param->numLayers; layer++)
            compressFrame(layer);
        X265_FLOW_LOG("FrameEncoder",
                      "[CHANNEL channel=frame-done encoder=%d frame=%d] PUBLISH object=Frame@%p nals=%u owner_before=FrameEncoder-%d owner_after=Encoder",
                      m_jpId, m_frame[0] ? m_frame[0]->m_poc : -1, m_frame[0],
                      m_nalList.m_numNal, m_jpId);
        m_done.trigger(); /* FrameEncoder::getEncodedPicture() blocks for this event */
        X265_FLOW_LOG("FrameEncoder",
                      "[CHANNEL channel=frame-start encoder=%d] WAIT_BEGIN reason=no-frame owner=FrameEncoder-%d",
                      m_jpId, m_jpId);
        m_enable.wait();
        X265_FLOW_LOG("FrameEncoder",
                      "[CHANNEL channel=frame-start encoder=%d frame=%d] WAIT_END reason=%s object=Frame@%p owner=FrameEncoder-%d",
                      m_jpId, m_frame[0] ? m_frame[0]->m_poc : -1,
                      m_threadActive ? "frame-ready" : "shutdown", m_frame[0], m_jpId);
    }
    X265_FLOW_LOG("FrameEncoder",
                  "[THREAD worker=FrameEncoder-%d role=frame-worker] STOP owner=FrameEncoder",
                  m_jpId);
}

void FrameEncoder::WeightAnalysis::processTasks(int /* workerThreadId */)
{
    Frame* frame = master.m_frame[master.m_sLayerId];
    weightAnalyse(*frame->m_encData->m_slice, *frame, *master.m_param);
}


uint32_t getBsLength( int32_t code )
{
    uint32_t ucode = (code <= 0) ? -code << 1 : (code << 1) - 1;

    ++ucode;
    unsigned long idx;
    BSR( idx, ucode );
    uint32_t length = (uint32_t)idx * 2 + 1;

    return length;
}

bool FrameEncoder::writeToneMapInfo(x265_sei_payload *payload)
{
    bool payloadChange = false;
    if (m_top->m_prevTonemapPayload.payload != NULL && payload->payloadSize == m_top->m_prevTonemapPayload.payloadSize)
    {
        if (memcmp(m_top->m_prevTonemapPayload.payload, payload->payload, payload->payloadSize) != 0)
            payloadChange = true;
    }
    else
    {
        payloadChange = true;
        if (m_top->m_prevTonemapPayload.payload != NULL)
            x265_free(m_top->m_prevTonemapPayload.payload);
        m_top->m_prevTonemapPayload.payload = (uint8_t*)x265_malloc(sizeof(uint8_t)* payload->payloadSize);
    }

    if (payloadChange)
    {
        m_top->m_prevTonemapPayload.payloadType = payload->payloadType;
        m_top->m_prevTonemapPayload.payloadSize = payload->payloadSize;
        memcpy(m_top->m_prevTonemapPayload.payload, payload->payload, payload->payloadSize);
    }

    bool isIDR = m_frame[0]->m_lowres.sliceType == X265_TYPE_IDR;
    return (payloadChange || isIDR);
}

void FrameEncoder::writeTrailingSEIMessages(int layer)
{
    Slice* slice = m_frame[layer]->m_encData->m_slice;
    int planes = (m_param->internalCsp != X265_CSP_I400) ? 3 : 1;
    int32_t payloadSize = 0;

    if (m_param->decodedPictureHashSEI == 1)
    {
        m_seiReconPictureDigest.m_method = SEIDecodedPictureHash::MD5;
        for (int i = 0; i < planes; i++)
            MD5Final(&m_seiReconPictureDigest.m_state[i], m_seiReconPictureDigest.m_digest[i]);
        payloadSize = 1 + 16 * planes;
    }
    else if (m_param->decodedPictureHashSEI == 2)
    {
        m_seiReconPictureDigest.m_method = SEIDecodedPictureHash::CRC;
        for (int i = 0; i < planes; i++)
            crcFinish(m_seiReconPictureDigest.m_crc[i], m_seiReconPictureDigest.m_digest[i]);
        payloadSize = 1 + 2 * planes;
    }
    else if (m_param->decodedPictureHashSEI == 3)
    {
        m_seiReconPictureDigest.m_method = SEIDecodedPictureHash::CHECKSUM;
        for (int i = 0; i < planes; i++)
            checksumFinish(m_seiReconPictureDigest.m_checksum[i], m_seiReconPictureDigest.m_digest[i]);
        payloadSize = 1 + 4 * planes;
    }

    m_seiReconPictureDigest.setSize(payloadSize);
    /* Reset gián tiếp: hàm này không gọi m_bs.resetBits() trực tiếp.
     * Tham số false bên dưới là isNested = 0: writeSEImessages sẽ reset m_bs
     * trước khi ghi decoded picture hash, rồi căn chỉnh và serialize ngay.
     * Trước lời gọi, m_bs có thể còn slice header của VCL vừa đóng gói;
     * bỏ header đó để NAL_SUFFIX_SEI chỉ chứa SEI hash, không lẫn dữ liệu VCL.
     * VCL đã nằm trong m_nalList nên vẫn được giữ nguyên. Suffix hash này
     * được ghi thành NAL riêng kể cả khi bật bSingleSeiNal cho prefix SEI. */
    m_seiReconPictureDigest.writeSEImessages(m_bs, *slice->m_sps, NAL_UNIT_SUFFIX_SEI, m_nalList, false, layer);
}

void FrameEncoder::compressFrame(int layer)
{
    /* Hướng dẫn đọc: xem work-log/compressFrame_block_explain.md tại workspace.
     * Các Block 1-18 bên dưới dùng cùng số thứ tự với tài liệu đó.
     * Hàm điều phối encode một picture: chuẩn bị -> chọn QP -> xử lý CTU rows
     * -> ghi bitstream/NAL -> cập nhật rate control và giải phóng references.
     * layer là chỉ số layer/view (thường bằng 0 khi encode một layer),
     * không phải độ sâu CU hay temporal layer.
     * Frame chứa ảnh gốc/tái tạo; Slice chứa thông tin cú pháp và cấu hình.
     * m_bs là buffer tạm để xây cú pháp, m_outStreams chứa dữ liệu CABAC,
     * còn m_nalList giữ các NAL đã được đóng gói để trả ra ngoài. */

    /* Cách hiểu mọi resetBits bên dưới: đưa số byte hợp lệ và số bit đang ghi
     * dở về 0 để lần ghi tiếp bắt đầu từ đầu buffer. Không memset toàn bộ vùng
     * nhớ, không giải phóng allocation và không xóa NAL trong m_nalList.
     * serialize chép dữ liệu sang NAL buffer nhưng không tự reset m_bs.
     * Reset m_bs cũng không reset xác suất CABAC hay dữ liệu m_outStreams. */

    /* Block 1 - Reset trạng thái và thống kê cho picture hiện tại.
     * Đo thời gian worker/ME/chờ, reset tiến độ rows và trạng thái VBV.
     * SSD (sai số bình phương) và SSIM được tích lũy lại từ đầu cho layer này;
     * FrameEncoder được tái sử dụng nên cần bỏ thống kê của picture trước. */
    ProfileScopeEvent(frameThread);
    X265_FLOW_LOG("FrameEncoder",
                  "[FRAME encoder=%d frame=%d layer=%d] BEGIN object=Frame@%p stage=compress owner=FrameEncoder-%d",
                  m_jpId, m_frame[layer]->m_poc, layer, m_frame[layer], m_jpId);

    m_startCompressTime[layer] = x265_mdate();
    m_totalActiveWorkerCount = 0;
    m_activeWorkerCountSamples = 0;
    m_totalWorkerElapsedTime[layer] = 0;
    m_totalThreadedMETime[layer] = 0;
    m_totalThreadedMEWait[layer] = 0;
    m_totalNoWorkerTime[layer] = 0;
    m_countRowBlocks = 0;
    m_allRowsAvailableTime[layer] = 0;
    m_stallStartTime[layer] = 0;

    m_completionCount = 0;
    memset((void*)m_bAllRowsStop, 0, sizeof(bool) * m_param->maxSlices);
    memset((void*)m_vbvResetTriggerRow, -1, sizeof(int) * m_param->maxSlices);
    m_rowSliceTotalBits[0] = 0;
    m_rowSliceTotalBits[1] = 0;

    m_SSDY[layer] = m_SSDU[layer] = m_SSDV[layer] = 0;
    m_ssim[layer] = 0;
    m_ssimCnt[layer] = 0;
    memset(&(m_frame[layer]->m_encData->m_frameStats), 0, sizeof(m_frame[layer]->m_encData->m_frameStats));
    m_sLayerId = layer;

    /* Block 2 - Chuẩn bị bản đồ cạnh cho EDGE_BASED_RSKIP.
     * Analysis đọc bản đồ này để hỗ trợ dừng thử chia nhỏ CU ở vùng đơn giản.
     * Nếu dùng Edge AQ thì bước phân tích trước đã chuẩn bị bản đồ tương ứng,
     * nên nhánh này chỉ tính khi aqMode khác X265_AQ_EDGE. */
    if (m_param->rc.aqMode != X265_AQ_EDGE && m_param->recursionSkipMode == EDGE_BASED_RSKIP)
    {
        int height = m_frame[layer]->m_fencPic->m_picHeight;
        int width = m_frame[layer]->m_fencPic->m_picWidth;
        intptr_t stride = m_frame[layer]->m_fencPic->m_stride;

        /* m_picOrg[0] là plane Y gốc; stride tính bằng phần tử pixel giữa 2 hàng.
         * NULL/false: không lưu/tính hướng cạnh; 1: ghi điểm có cạnh bằng 1,
         * điểm không có cạnh bằng 0 vào m_edgeBitPic trong vùng được xử lý. */
        if (!computeEdge(m_frame[layer]->m_edgeBitPic, m_frame[layer]->m_fencPic->m_picOrg[0], NULL, stride, height, width, false, 1))
        {
            x265_log(m_param, X265_LOG_ERROR, " Failed to compute edge !");
        }
    }

    /* Block 3 - Ghi các NAL mở đầu: EOS nếu cần, AUD, rồi headers nếu bật.
     * Access unit (AU) là nhóm NAL phục vụ giải mã một picture. */
    /* Emit access unit delimiter unless this is the first frame and the user is
     * not repeating headers (since AUD is supposed to be the first NAL in the access
     * unit) */
    Slice* slice = m_frame[layer]->m_encData->m_slice;

    /* Mỗi picture/layer mở một lifecycle mới trên writer của frame thread.
     * Single-SEI dùng buffer riêng; chế độ không gộp vẫn ghi trực tiếp vào m_bs.
     * begin chưa cấp phát buffer: nhóm không có message sẽ không tạo SEI NAL. */
    m_prefixSei.begin(m_param->bSingleSeiNal != 0);

    /* 3.1 - Param chỉ bật/tắt EOS; thời điểm ghi do loại frame quyết định.
     * Trước mỗi IDR có POC != 0, ghi EOS để kết thúc coded video sequence cũ.
     * Điều kiện là IDR, không phải mọi I/keyframe hay mọi ranh giới GOP;
     * một sequence có thể chứa nhiều GOP. IDR đầu tiên chưa có sequence cũ. */
    if (m_param->bEnableEndOfSequence && m_frame[layer]->m_lowres.sliceType == X265_TYPE_IDR && m_frame[layer]->m_poc)
    {
        /* Reset 1/6 - Chuẩn bị EOS từ buffer rỗng.
         * FrameEncoder dùng lại m_bs qua nhiều pictures/layers, nên buffer có
         * thể còn cú pháp NAL trước. EOS không được mang theo nội dung đó;
         * reset rồi serialize NAL_UNIT_EOS mà không ghi payload ảnh vào m_bs. */
        m_bs.resetBits();
        m_nalList.serialize(NAL_UNIT_EOS, m_bs);
    }

    /* 3.2 - AUD đánh dấu đầu AU; bỏ qua picture đầu nếu không repeat headers,
     * theo quy tắc thứ tự NAL được mô tả ở comment phía trên. */
    if (m_param->bEnableAccessUnitDelimiters && (m_frame[layer]->m_poc || m_param->bRepeatHeaders))
    {
        /* Reset 2/6 - Bắt đầu payload AUD mới, bỏ cú pháp NAL trước còn sót.
         * Sau reset: codeAUD ghi pic_type, writeByteAlignment kết thúc payload,
         * serialize đóng gói riêng thành NAL_ACCESS_UNIT_DELIMITER.
         * Nếu vừa reset để ghi EOS thì buffer có thể đã rỗng; AUD vẫn tự
         * chuẩn bị buffer vì nhánh EOS không phải lúc nào cũng chạy. */
        m_bs.resetBits();
        m_entropyCoder.setBitstream(&m_bs);
        // codeAUD ghi pic_type 3 bit: I -> 0, P -> 1, B -> 2.
        m_entropyCoder.codeAUD(*slice);
        // Thêm bit 1 rồi đệm bit 0 đến hết byte trước khi đóng gói NAL.
        m_bs.writeByteAlignment();
        m_nalList.serialize(NAL_UNIT_ACCESS_UNIT_DELIMITER, m_bs);
        /* Không reset m_bs sau AUD để chuẩn bị single-SEI nữa.
         * Writer sẽ gom SEI trong buffer riêng, nên AUD còn trong m_bs không
         * thể lẫn vào nhóm. Khi không gộp, mỗi SEI vẫn tự reset m_bs như trước. */
    }
    /* 3.3 - Repeat headers: tại keyframe, ghi lại VPS/SPS/PPS và SEI headers
     * theo cấu hình. Các khóa bảo vệ dữ liệu PPS/RPS dùng chung giữa frames.
     * RPS (Reference Picture Set) mô tả các picture tham chiếu cần giữ. */
    if (m_frame[layer]->m_lowres.bKeyframe && m_param->bRepeatHeaders)
    {
        if (m_param->bOptRefListLengthPPS)
        {
            ScopedLock refIdxLock(m_top->m_sliceRefIdxLock);
            m_top->updateRefIdx();
        }
        if (m_top->m_param->rc.bStatRead  && m_top->m_param->bMultiPassOptRPS)
        {
            ScopedLock refIdxLock(m_top->m_rpsInSpsLock);
            if (!m_top->computeSPSRPSIndex())
            {
                x265_log(m_param, X265_LOG_ERROR, "compute commonly RPS failed!\n");
                m_top->m_aborted = true;
            }
            m_top->getStreamHeaders(m_nalList, m_entropyCoder, m_bs, m_prefixSei);
        }
        else
            m_top->getStreamHeaders(m_nalList, m_entropyCoder, m_bs, m_prefixSei);
    }

    // Khi đọc stats multipass, lấy chỉ số RPS đã chọn theo encode order.
    if (m_top->m_param->rc.bStatRead && m_top->m_param->bMultiPassOptRPS)
        m_frame[layer]->m_encData->m_slice->m_rpsIdx = (m_top->m_rateControl->m_rce2Pass + m_frame[layer]->m_encodeOrder)->rpsIdx;

    /* Block 4 - Weighted prediction (dự đoán có trọng số).
     * Điều chỉnh pixel reference để dự đoán tốt hơn khi ảnh sáng/tối dần
     * hoặc thay đổi tương phản, dù nội dung và chuyển động gần như giống nhau.
     * Công thức khái niệm: prediction = weight * reference_pixel + offset.
     * weight là hệ số nhân; offset dịch độ sáng lên/xuống. Cách tính thực tế
     * dùng số nguyên, dịch bit, làm tròn và giới hạn giá trị pixel.
     * Ví dụ reference = 100, pixel hiện tại = 120:
     *   Không điều chỉnh: prediction = 100, residual = 120 - 100 = 20.
     *   weight = 1, offset = 20: prediction = 120, residual = 0.
     * Residual (pixel gốc - prediction) nhỏ hơn thường cần ít bit mã hóa hơn.
     * P-slice dùng list L0; B-slice dùng L0/L1 và có thể kết hợp hai dự đoán.
     * Nhánh này chỉ dùng ở base layer (!layer), khi PPS bật weight cho P/B.
     * analysisLoad: dùng lại weights đã lưu; nếu không thì weightAnalyse
     * ước tính mới, mượn worker rảnh qua tryBondPeers hoặc chạy trên frame thread.
     * Nếu không dùng weighted prediction thì disableWeights ở cuối block.
     * Block này chuẩn bị tham số; Block 5 gắn chúng vào motion references.
     * Tham số được báo trong slice header để decoder tạo cùng prediction. */
    // Weighted Prediction parameters estimation.
    bool bUseWeightP = slice->m_sliceType == P_SLICE && slice->m_pps->bUseWeightPred && !layer;
    bool bUseWeightB = slice->m_sliceType == B_SLICE && slice->m_pps->bUseWeightedBiPred && !layer;

    WeightParam* reuseWP = NULL;
    if (m_param->analysisLoad[0] && (bUseWeightP || bUseWeightB))
        reuseWP = (WeightParam*)m_frame[layer]->m_analysisData.wt;

    if (bUseWeightP || bUseWeightB)
    {
#if DETAILED_CU_STATS
        m_cuStats.countWeightAnalyze++;
        ScopedElapsedTime time(m_cuStats.weightAnalyzeTime);
#endif
        if (strlen(m_param->analysisLoad))
        {
            for (int list = 0; list < slice->isInterB() + 1; list++) 
            {
                for (int plane = 0; plane < (m_param->internalCsp != X265_CSP_I400 ? 3 : 1); plane++)
                {
                    for (int ref = 1; ref < slice->m_numRefIdx[list]; ref++)
                        SET_WEIGHT(slice->m_weightPredTable[list][ref][plane], false, 1 << reuseWP->log2WeightDenom, reuseWP->log2WeightDenom, 0);
                    slice->m_weightPredTable[list][0][plane] = *(reuseWP++);
                }
            }
        }
        else
        {
            WeightAnalysis wa(*this);
            if (m_pool && wa.tryBondPeers(*this, 1))
                /* use an idle worker for weight analysis */
                wa.waitForExit();
            else
                weightAnalyse(*slice, *m_frame[layer], *m_param);
        }
    }
    else
        slice->disableWeights();

    // analysisSave chuẩn bị nơi lưu weights để lần encode sau có thể tái sử dụng.
    if (strlen(m_param->analysisSave) && (bUseWeightP || bUseWeightB))
        reuseWP = (WeightParam*)m_frame[layer]->m_analysisData.wt;
    /* Block 5 - Khởi tạo motion references: I dùng 0 list, P dùng L0,
     * B dùng L0 và L1. m_mref gắn ảnh tái tạo cùng weights nếu có.
     * Dự đoán phải dùng reconstructed picture vì decoder cũng chỉ có ảnh này;
     * dùng ảnh gốc làm reference sẽ khiến encoder và decoder không khớp nhau. */
    // Generate motion references
    int numPredDir = slice->isInterP() ? 1 : slice->isInterB() ? 2 : 0;
    for (int l = 0; l < numPredDir; l++)
    {
        for (int ref = 0; ref < slice->m_numRefIdx[l]; ref++)
        {
            WeightParam *w = NULL;
            if ((bUseWeightP || bUseWeightB) && slice->m_weightPredTable[l][ref][0].wtPresent)
                w = slice->m_weightPredTable[l][ref];
            slice->m_refReconPicList[l][ref] = slice->m_refFrameList[l][ref]->m_reconPic[0];
            m_mref[l][ref].init(slice->m_refReconPicList[l][ref], w, *m_param);
        }
        if (strlen(m_param->analysisSave) && (bUseWeightP || bUseWeightB))
        {
            for (int i = 0; i < (m_param->internalCsp != X265_CSP_I400 ? 3 : 1); i++)
                *(reuseWP++) = slice->m_weightPredTable[l][0][i];
        }

    }

    /* Block 6 - Chọn QP qua rate control và chuẩn bị AQ cho layer bổ sung.
     * TLD (Thread Local Data) là vùng làm việc riêng của worker, chứa analysis
     * và quantization. numTLD quyết định số vùng cần thiết lập/tổng hợp.
     * QP cao thường lượng tử hóa mạnh hơn: ít bit hơn, sai số lớn hơn. */
    int numTLD;
    if (m_pool)
        numTLD = m_param->bEnableWavefront ? m_pool->m_numWorkers : m_pool->m_numWorkers + m_pool->m_numProviders;
    else
        numTLD = 1;

    /* Get the QP for this frame from rate control. This call may block until
     * frames ahead of it in encode order have called rateControlEnd() */
    // Base layer chọn QP theo CQP/CRF/ABR và VBV; layer khác dùng lại newQp.
    int qp = (layer == 0) ? m_top->m_rateControl->rateControlStart(m_frame[layer], &m_rce, m_top) : (int)m_rce.newQp;

    m_rce.newQp = qp;

    /* AQ điều chỉnh QP theo vùng ảnh. Với multiview, sao chép AQ/CU-tree
     * offsets từ base layer; với scalable layers, reset các offsets về 0.
     * Nhánh hevcAq làm tương tự trên các độ sâu AQ được cấu hình. */
    if (!!layer && m_top->m_lookahead->m_bAdaptiveQuant)
    {
        int ncu;
        if (m_param->rc.qgSize == 8)
            ncu = m_top->m_rateControl->m_ncu * 4;
        else
            ncu = m_top->m_rateControl->m_ncu;
        if (m_param->numViews > 1)
        {
            for (int i = 0; i < ncu; i++)
            {
                m_frame[layer]->m_lowres.qpCuTreeOffset[i] = m_frame[0]->m_lowres.qpCuTreeOffset[i];
                m_frame[layer]->m_lowres.qpAqOffset[i] = m_frame[0]->m_lowres.qpAqOffset[i];
            }
        }
        else if (m_param->numScalableLayers > 1)
        {
            memset(m_frame[layer]->m_lowres.qpCuTreeOffset, 0, sizeof(double)*ncu);
            memset(m_frame[layer]->m_lowres.qpAqOffset, 0, sizeof(double)* ncu);
        }

        m_frame[layer]->m_encData->m_avgQpAq = m_frame[0]->m_encData->m_avgQpAq;
        m_frame[layer]->m_encData->m_avgQpRc = m_frame[0]->m_encData->m_avgQpRc;
        if (!!m_param->rc.hevcAq)
        {
            for (uint32_t d = 0; d < 4; d++)
            {
                int ctuSizeIdx = 6 - g_log2Size[m_param->maxCUSize];
                int aqDepth = g_log2Size[m_param->maxCUSize] - g_log2Size[m_param->rc.qgSize];
                if (!aqLayerDepth[ctuSizeIdx][aqDepth][d])
                    continue;
                PicQPAdaptationLayer* pcAQLayer0 = &m_frame[0]->m_lowres.pAQLayer[d];
                PicQPAdaptationLayer* pcAQLayer1 = &m_frame[layer]->m_lowres.pAQLayer[d];
                const uint32_t aqPartWidth = m_frame[0]->m_lowres.pAQLayer[d].aqPartWidth;
                const uint32_t aqPartHeight = m_frame[0]->m_lowres.pAQLayer[d].aqPartHeight;
                double* pcQP0 = pcAQLayer0->dQpOffset;
                double* pcCuTree0 = pcAQLayer0->dCuTreeOffset;
                double* pcQP1 = pcAQLayer1->dQpOffset;
                double* pcCuTree1 = pcAQLayer1->dCuTreeOffset;
                if (m_param->numViews > 1)
                {
                    for (uint32_t y = 0; y < m_frame[0]->m_fencPic->m_picHeight; y += aqPartHeight)
                    {
                        for (uint32_t x = 0; x < m_frame[0]->m_fencPic->m_picWidth; x += aqPartWidth, pcQP0++, pcCuTree0++, pcQP1++, pcCuTree1++)
                        {
                            *pcQP1 = *pcQP0;
                            *pcCuTree1 = *pcCuTree0;
                        }
                    }
                }
                else if (m_param->numScalableLayers > 1)
                {
                    int numAQPartInWidth = (m_frame[0]->m_fencPic->m_picWidth + aqPartWidth - 1) / aqPartWidth;
                    int numAQPartInHeight = (m_frame[0]->m_fencPic->m_picHeight + aqPartHeight - 1) / aqPartHeight;
                    memset(m_frame[layer]->m_lowres.pAQLayer[d].dQpOffset, 0, sizeof(double)*numAQPartInWidth* numAQPartInHeight);
                    memset(m_frame[layer]->m_lowres.pAQLayer[d].dCuTreeOffset, 0, sizeof(double)* numAQPartInWidth* numAQPartInHeight);
                }
            }
        }
    }
    /* Block 7 - Lọc temporal/noise reduction và hoàn thiện QP của slice.
     * MCSTF dùng các ảnh lân cận theo thời gian cùng motion compensation để
     * lọc ảnh trước khi nén, khi cả cấu hình và frame/GOP cho phép. */
    if (m_param->bEnableTemporalFilter && m_top->isFilterThisframe(m_frame[layer]->m_mcstf->m_sliceTypeConfig, m_frame[layer]->m_lowres.sliceType) && m_frame[layer]->m_lowres.filterThisGOP)
    {
        m_frame[layer]->m_mcstf->m_QP = qp;
        m_frame[layer]->m_mcstf->bilateralFilter(m_frame[layer], m_frame[layer]->m_mcstfRefList, m_pool);
    }

    /* Gắn bộ hệ số và bộ đếm noise reduction vào quantizer của từng TLD.
     * QP vượt QP_MAX_SPEC khi có VBV dùng hệ số emergency để giảm số bit;
     * đường thông thường dùng bộ thống kê/hệ số riêng của worker. */
    if (m_nr)
    {
        if (qp > QP_MAX_SPEC && m_frame[layer]->m_param->rc.vbvBufferSize)
        {
            for (int i = 0; i < numTLD; i++)
            {
                m_tld[i].analysis.m_quant.m_frameNr[m_jpId].offset = m_top->m_offsetEmergency[qp - QP_MAX_SPEC - 1];
                m_tld[i].analysis.m_quant.m_frameNr[m_jpId].residualSum = m_top->m_residualSumEmergency;
                m_tld[i].analysis.m_quant.m_frameNr[m_jpId].count = m_top->m_countEmergency;
            }
        }
        else
        {
            if (m_param->noiseReductionIntra || m_param->noiseReductionInter)
            {
                for (int i = 0; i < numTLD; i++)
                {
                    m_tld[i].analysis.m_quant.m_frameNr[m_jpId].offset = m_tld[i].analysis.m_quant.m_frameNr[m_jpId].nrOffsetDenoise;
                    m_tld[i].analysis.m_quant.m_frameNr[m_jpId].residualSum = m_tld[i].analysis.m_quant.m_frameNr[m_jpId].nrResidualSum;
                    m_tld[i].analysis.m_quant.m_frameNr[m_jpId].count = m_tld[i].analysis.m_quant.m_frameNr[m_jpId].nrCount;
                }
            }
            else
            {
                for (int i = 0; i < numTLD; i++)
                    m_tld[i].analysis.m_quant.m_frameNr[m_jpId].offset = NULL;
            }
        }
    }

    /* Clip slice QP to 0-51 spec range before encoding */
    // Giới hạn QP theo bit depth; HDR10Opt bổ sung offset cho chroma Cb/Cr.
    slice->m_sliceQp = x265_clip3(-QP_BD_OFFSET, QP_MAX_SPEC, qp);
    if (m_param->bHDR10Opt)
    {
        int qpCb = x265_clip3(-12, 0, (int)floor((m_top->m_cB * ((-.46) * qp + 9.26)) + 0.5 ));
        int qpCr = x265_clip3(-12, 0, (int)floor((m_top->m_cR * ((-.46) * qp + 9.26)) + 0.5 ));
        slice->m_chromaQpOffset[0] = slice->m_pps->chromaQpOffset[0] + qpCb < -12 ? (qpCb + (-12 - (slice->m_pps->chromaQpOffset[0] + qpCb))) : qpCb;
        slice->m_chromaQpOffset[1] = slice->m_pps->chromaQpOffset[1] + qpCr < -12 ? (qpCr + (-12 - (slice->m_pps->chromaQpOffset[1] + qpCr))) : qpCr;
    }

    // Thu thập chi phí slice_qp_delta để chọn QP khởi tạo trong PPS hiệu quả hơn.
    if (m_param->bOptQpPPS && m_param->bRepeatHeaders)
    {
        ScopedLock qpLock(m_top->m_sliceQpLock);
        for (int i = 0; i < (QP_MAX_MAX + 1); i++)
        {
            int delta = slice->m_sliceQp - (i + 1);
            int codeLength = getBsLength( delta );
            m_top->m_iBitsCostSum[i] += codeLength;
        }
        m_top->m_iFrameNum++;
    }
    // Khởi tạo xác suất CABAC theo slice/QP; đây là context gốc cho các rows.
    m_initSliceContext.resetEntropy(*slice);

    /* Block 8 - Khởi tạo in-loop filters (deblocking/SAO) và trạng thái rows.
     * Chặn rows trước khi reset để worker chưa xử lý dữ liệu đang khởi tạo;
     * mỗi CTURow nhận CABAC context ban đầu và sliceId của nó. */
    m_frameFilter.start(m_frame[layer], m_initSliceContext);

    /* ensure all rows are blocked prior to initializing row CTU counters */
    WaveFront::clearEnabledRowMask();

    WaveFront::setLayerId(layer);
    /* reset entropy coders and compute slice id */
    m_entropyCoder.load(m_initSliceContext);
    for (uint32_t sliceId = 0; sliceId < m_param->maxSlices; sliceId++)   
        for (uint32_t row = m_sliceBaseRow[sliceId]; row < m_sliceBaseRow[sliceId + 1]; row++)
            m_rows[row].init(m_initSliceContext, sliceId);   

    // reset slice counter for rate control update
    m_sliceCnt = 0;

    /* Block 9 - Cấp phát/reset CABAC substreams.
     * WPP: một substream cho mỗi hàng CTU; nhiều substreams ghép vào một
     * slice NAL, không phải mỗi substream là một NAL riêng.
     * Không SAO: row coder ghi trực tiếp vào m_outStreams. Có SAO: cần final
     * coding sau khi quyết định filter đã có (encodeSlice ở Block 14).
     * m_substreamSizes dùng để ghi entry-point offsets trong slice header. */
    uint32_t numSubstreams = m_param->bEnableWavefront ? slice->m_sps->numCuInHeight : m_param->maxSlices;
    X265_CHECK(m_param->bEnableWavefront || (m_param->maxSlices == 1), "Multiple slices without WPP unsupport now!");
    if (!m_outStreams)
    {
        m_outStreams = new Bitstream[numSubstreams];
        if (!m_param->bEnableWavefront)
            m_backupStreams = new Bitstream[numSubstreams];
        m_substreamSizes = X265_MALLOC(uint32_t, numSubstreams);
        if (!slice->m_bUseSao)
        {
            for (uint32_t i = 0; i < numSubstreams; i++)
                m_rows[i].rowGoOnCoder.setBitstream(&m_outStreams[i]);
        }
    }
    else
    {
        for (uint32_t i = 0; i < numSubstreams; i++)
        {
            m_outStreams[i].resetBits();
            if (!slice->m_bUseSao)
                m_rows[i].rowGoOnCoder.setBitstream(&m_outStreams[i]);
            else
                m_rows[i].rowGoOnCoder.setBitstream(NULL);
        }
    }

    /* Block 10 - Ghi prefix SEI trước dữ liệu VCL của picture.
     * SEI là metadata: HRD/timing, recovery, transfer characteristics, grain,
     * và user data. Mỗi loại chỉ được ghi khi điều kiện tương ứng bật.
     * bSingleSeiNal = 0: writer dùng m_bs và reset/serialize từng message như cũ.
     * bSingleSeiNal = 1: writer gom trong buffer riêng rồi flush trước VCL.
     * encodeOrder là thứ tự mã hóa, có thể khác POC (thứ tự hiển thị). */
    m_rce.encodeOrder = m_frame[layer]->m_encodeOrder;
    int prevBPSEI = m_rce.encodeOrder ? m_top->m_lastBPSEI : 0;

    if (m_frame[layer]->m_lowres.bKeyframe)
    {
        if (m_param->bEmitHRDSEI)
        {
            // Buffering Period mô tả trạng thái ban đầu của CPB theo mô hình HRD.
            SEIBufferingPeriod* bpSei = &m_top->m_rateControl->m_bufPeriodSEI;

            // since the temporal layer HRD is not ready, we assumed it is fixed
            bpSei->m_auCpbRemovalDelayDelta = 1;
            bpSei->m_cpbDelayOffset = 0;
            bpSei->m_dpbDelayOffset = 0;
            bpSei->m_concatenationFlag = (m_param->bEnableHRDConcatFlag && !m_frame[layer]->m_poc) ? true : false;

            // hrdFullness() calculates the initial CPB removal delay and offset
            m_top->m_rateControl->hrdFullness(bpSei);
            m_prefixSei.append(*bpSei, m_bs, *slice->m_sps, m_nalList, layer);

            m_top->m_lastBPSEI = m_rce.encodeOrder;
        }

        if (m_frame[layer]->m_lowres.sliceType == X265_TYPE_IDR && m_param->bEmitIDRRecoverySEI)
        {
            // Recovery Point tại IDR: điểm phục hồi ngay tại picture này (POC count 0).
            /* Recovery Point SEI require the SPS to be "activated" */
            SEIRecoveryPoint sei;
            sei.m_recoveryPocCnt = 0;
            sei.m_exactMatchingFlag = true;
            sei.m_brokenLinkFlag = false;
            m_prefixSei.append(sei, m_bs, *slice->m_sps, m_nalList, layer);
        }
    }

    if ((m_param->bEmitHRDSEI || !!m_param->interlaceMode))
    {
        /* Picture Timing mô tả frame/field và độ trễ lấy AU khỏi CPB,
         * xuất picture khỏi DPB; độ dài các trường HRD lấy từ SPS/VUI. */
        SEIPictureTiming *sei = m_rce.picTimingSEI;
        const VUI *vui = &slice->m_sps->vuiParameters;
        const HRDInfo *hrd = &vui->hrdParameters;
        int poc = slice->m_poc;

        if (vui->frameFieldInfoPresentFlag)
        {
            if (m_param->interlaceMode > 0)
            {
                if( m_param->interlaceMode == 2 )
                {   
                    // m_picStruct should be set to 3 or 4 when field feature is enabled
                    if (m_param->bField)
                        // 3: Top field, bottom field, in that order; 4: Bottom field, top field, in that order
                        sei->m_picStruct = (slice->m_fieldNum == 1) ? 4 : 3;
                    else
                        sei->m_picStruct = (poc & 1) ? 1 /* top */ : 2 /* bottom */;
                }     
                else if (m_param->interlaceMode == 1)
                {
                    if (m_param->bField)
                        sei->m_picStruct = (slice->m_fieldNum == 1) ? 3: 4;
                    else
                        sei->m_picStruct = (poc & 1) ? 2 /* bottom */ : 1 /* top */;
                }
            }
            else if (m_param->bEnableFrameDuplication)
                sei->m_picStruct = m_frame[layer]->m_picStruct;
            else
                sei->m_picStruct = m_param->pictureStructure;

            sei->m_sourceScanType = m_param->interlaceMode ? 0 : 1;

            sei->m_duplicateFlag = false;
        }

        if (vui->hrdParametersPresentFlag)
        {
            // The m_aucpbremoval delay specifies how many clock ticks the
            // access unit associated with the picture timing SEI message has to
            // wait after removal of the access unit with the most recent
            // buffering period SEI message
            sei->m_auCpbRemovalDelay = X265_MIN(X265_MAX(1, m_rce.encodeOrder - prevBPSEI), (1 << hrd->cpbRemovalDelayLength));
            sei->m_picDpbOutputDelay = slice->m_sps->numReorderPics[m_frame[layer]->m_tempLayer] + poc - m_rce.encodeOrder;
        }

        m_prefixSei.append(*sei, m_bs, *slice->m_sps, m_nalList, layer);
    }

    // Alternative TC báo transfer characteristics ưu tiên tại picture IRAP.
    if (m_param->preferredTransferCharacteristics > -1 && slice->isIRAP())
    {
        SEIAlternativeTC m_seiAlternativeTC;
        m_seiAlternativeTC.m_preferredTransferCharacteristics = m_param->preferredTransferCharacteristics;
        m_prefixSei.append(m_seiAlternativeTC, m_bs, *slice->m_sps, m_nalList, layer);
    }
    /* Write Film grain characteristics if present */
    if (this->m_top->m_filmGrainIn)
    {
        FilmGrainCharacteristics m_filmGrain;
        /* Read the Film grain model file */
        readModel(&m_filmGrain, this->m_top->m_filmGrainIn);
        m_prefixSei.append(m_filmGrain, m_bs, *slice->m_sps, m_nalList, layer);
    }
    /* Write Aom film grain characteristics if present */
    if (this->m_top->m_aomFilmGrainIn)
    {
        AomFilmGrainCharacteristics m_aomFilmGrain;
        /* Read the Film grain model file */
        readAomModel(&m_aomFilmGrain, this->m_top->m_aomFilmGrainIn);
        m_prefixSei.append(m_aomFilmGrain, m_bs, *slice->m_sps, m_nalList);
    }
    /* User SEI lấy từ payload của ứng dụng: unregistered hoặc registered T.35.
     * Với HDR10 optimization, writeToneMapInfo kiểm tra có cần phát lại payload.
     * Hàm này không có nhánh phát Frame Packing Arrangement (FPA) chuyên dụng;
     * FPA là một loại SEI message, không phải một NAL type riêng. */
    /* Write user SEI */
    for (int i = 0; i < m_frame[layer]->m_userSEI.numPayloads; i++)
    {
        x265_sei_payload *payload = &m_frame[layer]->m_userSEI.payloads[i];
        if (payload->payloadType == USER_DATA_UNREGISTERED)
        {
            SEIuserDataUnregistered sei;
            sei.m_userData = payload->payload;
            sei.setSize(payload->payloadSize);
            m_prefixSei.append(sei, m_bs, *slice->m_sps, m_nalList, layer);
        }
        else if (payload->payloadType == USER_DATA_REGISTERED_ITU_T_T35)
        {
            bool writeSei = m_param->bDhdr10opt ? writeToneMapInfo(payload) : true;
            if (writeSei)
            {
                SEIuserDataRegistered sei;
                sei.m_userData = payload->payload;
                sei.setSize(payload->payloadSize);
                m_prefixSei.append(sei, m_bs, *slice->m_sps, m_nalList, layer);
            }
        }
        else
            x265_log(m_param, X265_LOG_ERROR, "Unrecognized SEI type\n");
    }

    /* Flush theo số messages thực sự đã append, không suy luận từ options.
     * Không có message: đóng nhóm mà không phát NAL rỗng. Khi không gộp,
     * messages đã serialize riêng; flush chỉ kết thúc lifecycle.
     * Nhóm single-SEI đã chứa cả SEI headers và SEI picture, giữ thứ tự append. */
    if (!m_prefixSei.flush(m_nalList))
        m_top->m_aborted = true;
    /* Block 11 - Đồng bộ rate control giữa các frame threads.
     * CQP/CRF không VBV có thể mở dependency sớm cho frame sau;
     * ABR/VBV cần chờ cập nhật thống kê giữa frame để điều chỉnh bitrate.
     * Dynamic Refine lấy dữ liệu huấn luyện; mapping WPP bên dưới đổi giữa
     * row thật và chỉ số scheduler, xen kẽ các slices khi có nhiều slice. */
    /* CQP and CRF (without capped VBV) doesn't use mid-frame statistics to 
     * tune RateControl parameters for other frames.
     * Hence, for these modes, update m_startEndOrder and unlock RC for previous threads waiting in
     * RateControlEnd here, after the slice contexts are initialized. For the rest - ABR
     * and VBV, unlock only after rateControlUpdateStats of this frame is called */
    if (m_param->rc.rateControlMode != X265_RC_ABR && !m_top->m_rateControl->m_isVbv)
    {
        m_top->m_rateControl->m_startEndOrder.incr();

        if (m_rce.encodeOrder < m_param->frameNumThreads - 1)
            m_top->m_rateControl->m_startEndOrder.incr(); // faked rateControlEnd calls for negative frames
    }

    if (m_param->bDynamicRefine)
        computeAvgTrainingData(layer);

    /* Analyze CTU rows, most of the hard work is done here.  Frame is
     * compressed in a wave-front pattern if WPP is enabled. Row based loop
     * filters runs behind the CTU compression and reconstruction */

    for (uint32_t sliceId = 0; sliceId < m_param->maxSlices; sliceId++)
        m_rows[m_sliceBaseRow[sliceId]].active = true;
    
    if (m_param->bEnableWavefront)
    {
        int i = 0;
        for (uint32_t rowInSlice = 0; rowInSlice < m_sliceGroupSize; rowInSlice++)
        {
            for (uint32_t sliceId = 0; sliceId < m_param->maxSlices; sliceId++)
            {
                const uint32_t sliceStartRow = m_sliceBaseRow[sliceId];
                const uint32_t sliceEndRow = m_sliceBaseRow[sliceId + 1] - 1;
                const uint32_t row = sliceStartRow + rowInSlice;
                if (row > sliceEndRow)
                    continue;
                m_row_to_idx[row] = i;
                m_idx_to_row[i] = row;
                i += 1;
            }
        }
    }

    /* Block 12 - Nén CTU rows và chạy in-loop filters.
     * processRowEncoder thực hiện analysis/prediction/transform/quantization/
     * reconstruction qua các hàm bên dưới; compressFrame điều phối công việc.
     * WPP cho rows chạy song song nhưng vẫn phải tôn trọng dependency của
     * reference pixels, tiến độ hàng trên và CABAC context. */
    if (m_param->bEnableWavefront)
    {
        for (uint32_t rowInSlice = 0; rowInSlice < m_sliceGroupSize; rowInSlice++)
        {
            for (uint32_t sliceId = 0; sliceId < m_param->maxSlices; sliceId++)
            {
                const uint32_t sliceStartRow = m_sliceBaseRow[sliceId];
                const uint32_t sliceEndRow = m_sliceBaseRow[sliceId + 1] - 1;
                const uint32_t row = sliceStartRow + rowInSlice;

                X265_CHECK(row < m_numRows, "slices row fault was detected");

                if (row > sliceEndRow)
                    continue;

                // block until all reference frames have reconstructed the rows we need
                for (int l = 0; l < numPredDir; l++)
                {
                    for (int ref = 0; ref < slice->m_numRefIdx[l]; ref++)
                    {
                        Frame *refpic = slice->m_refFrameList[l][ref];

#if ENABLE_SCC_EXT
                        /*Exempt the current pic as reference*/
                        if (m_param->bEnableSCC && refpic->m_poc == m_frame[layer]->m_poc)
                            continue;
#endif

                        // NOTE: we unnecessary wait row that beyond current slice boundary
                        const int rowIdx = X265_MIN(sliceEndRow, (row + m_refLagRows));

                        /* Đợi reference sẵn sàng đến row + m_refLagRows để ME
                         * có đủ pixel cho phạm vi tìm kiếm; không đọc ảnh tái
                         * tạo/lọc còn dở của frame thread khác. */
                        while (refpic->m_reconRowFlag[rowIdx].get() == 0)
                            refpic->m_reconRowFlag[rowIdx].waitForChange(0);

                        if ((bUseWeightP || bUseWeightB) && m_mref[l][ref].isWeighted)
                            m_mref[l][ref].applyWeight(rowIdx, m_numRows, sliceEndRow, sliceId);
                    }
                }
                
                // Reference đã sẵn sàng; dependency nội bộ của WPP vẫn được giữ.
                enableRowEncoder(m_row_to_idx[row]); /* clear external dependency for this row */

                if (m_top->m_threadedME && !slice->isIntra())
                {
                    ScopedLock lock(m_tmeDepLock);
                    m_tmeDeps[row].external = true;
                    m_top->m_threadedME->enqueueReadyRows(row, layer, this);
                }

                if (!rowInSlice)
                {
                    m_row0WaitTime[layer] = x265_mdate();
                    enqueueRowEncoder(m_row_to_idx[row]); /* clear internal dependency, start wavefront */
                }
                tryWakeOne();
            } // end of loop rowInSlice
        } // end of loop sliceId

        m_allRowsAvailableTime[layer] = x265_mdate();
        tryWakeOne(); /* ensure one thread is active or help-wanted flag is set prior to blocking */
        static const int block_ms = 250;
        // Đợi sự kiện hoàn tất từ workers; khi timeout thì thử đánh thức worker.
        while (m_completionEvent.timedWait(block_ms))
            tryWakeOne();
    }
    else
    {
        /* Không WPP: frame thread nén rows tuần tự, vẫn đợi reference rows.
         * Filter chạy trễ m_filterRowDelay vì cần dữ liệu từ rows lân cận;
         * các lượt cuối chỉ chạy filter để xử lý hết các rows còn lại. */
        for (uint32_t i = 0; i < m_numRows + m_filterRowDelay; i++)
        {
            // compress
            if (i < m_numRows)
            {
                // block until all reference frames have reconstructed the rows we need
                for (int l = 0; l < numPredDir; l++)
                {
                    int list = l;
                    for (int ref = 0; ref < slice->m_numRefIdx[list]; ref++)
                    {
                        Frame *refpic = slice->m_refFrameList[list][ref];

#if ENABLE_SCC_EXT
                        /*Exempt the current pic as reference*/
                        if (m_param->bEnableSCC && refpic->m_poc == m_frame[layer]->m_poc)
                            continue;
#endif

                        const int rowIdx = X265_MIN(m_numRows - 1, (i + m_refLagRows));
                        while (refpic->m_reconRowFlag[rowIdx].get() == 0)
                            refpic->m_reconRowFlag[rowIdx].waitForChange(0);

                        if ((bUseWeightP || bUseWeightB) && m_mref[l][ref].isWeighted)
                            m_mref[list][ref].applyWeight(rowIdx, m_numRows, m_numRows, 0);
                    }
                }

                if (m_top->m_threadedME && !slice->isIntra())
                {
                    ScopedLock lock(m_tmeDepLock);
                    m_tmeDeps[i].external = true;
                    m_top->m_threadedME->enqueueReadyRows(i, layer, this);
                }

                if (!i)
                    m_row0WaitTime[layer] = x265_mdate();
                else if (i == m_numRows - 1)
                    m_allRowsAvailableTime[layer] = x265_mdate();
                processRowEncoder(i, m_tld[m_localTldIdx], layer);
            }

            // filter
            if (i >= m_filterRowDelay)
                m_frameFilter.processRow(i - m_filterRowDelay, layer);
        }
    }
    /* Block 13 - Rows đã hoàn tất: tính VMAF nếu build hỗ trợ, reset threaded
     * ME dependencies, hoàn thiện hash cho multislice, thu thập Dynamic Refine
     * và reset dữ liệu MCSTF. Sau đó cộng row statistics thành frame statistics
     * để ghi two-pass stats/CSV (MV bits, coefficient bits, tỷ lệ CU...). */
#if ENABLE_LIBVMAF
    vmafFrameLevelScore();
#endif

    m_tmeDepLock.acquire();
    m_tmeDeps.clear();
    m_tmeDeps.resize(m_numRows);
    m_tmeDepLock.release();

    if (m_param->maxSlices > 1)
    {
        PicYuv *reconPic = m_frame[layer]->m_reconPic[0];
        uint32_t height = reconPic->m_picHeight;
        initDecodedPictureHashSEI(0, 0, height, layer);
    } 

    if (m_param->bDynamicRefine && m_top->m_startPoint <= m_frame[layer]->m_encodeOrder) //Avoid collecting data that will not be used by future frames.
        collectDynDataFrame(layer);

    if (m_param->bEnableTemporalFilter && m_top->isFilterThisframe(m_frame[layer]->m_mcstf->m_sliceTypeConfig, m_frame[layer]->m_lowres.sliceType))
    {
        //Reset the MCSTF context in Frame Encoder and Frame
        for (int i = 0; i < (m_frame[layer]->m_mcstf->m_range << 1); i++)
        {
            memset(m_frame[layer]->m_mcstfRefList[i].mvs0, 0, sizeof(MV) * ((m_param->sourceWidth / 16) * (m_param->sourceHeight / 16)));
            memset(m_frame[layer]->m_mcstfRefList[i].mvs1, 0, sizeof(MV) * ((m_param->sourceWidth / 16) * (m_param->sourceHeight / 16)));
            memset(m_frame[layer]->m_mcstfRefList[i].mvs2, 0, sizeof(MV) * ((m_param->sourceWidth / 16) * (m_param->sourceHeight / 16)));
            memset(m_frame[layer]->m_mcstfRefList[i].mvs,  0, sizeof(MV) * ((m_param->sourceWidth / 4) * (m_param->sourceHeight / 4)));
            memset(m_frame[layer]->m_mcstfRefList[i].noise, 0, sizeof(int) * ((m_param->sourceWidth / 4) * (m_param->sourceHeight / 4)));
            memset(m_frame[layer]->m_mcstfRefList[i].error, 0, sizeof(int) * ((m_param->sourceWidth / 4) * (m_param->sourceHeight / 4)));

            m_frame[layer]->m_mcstf->m_numRef = 0;
        }
    }


    if (m_param->rc.bStatWrite)
    {
        int totalI = 0, totalP = 0, totalSkip = 0;

        // accumulate intra,inter,skip cu count per frame for 2 pass
        for (uint32_t i = 0; i < m_numRows; i++)
        {
            m_frame[layer]->m_encData->m_frameStats.mvBits    += m_rows[i].rowStats.mvBits;
            m_frame[layer]->m_encData->m_frameStats.coeffBits += m_rows[i].rowStats.coeffBits;
            m_frame[layer]->m_encData->m_frameStats.miscBits  += m_rows[i].rowStats.miscBits;
            totalI                                     += m_rows[i].rowStats.intra8x8Cnt;
            totalP                                     += m_rows[i].rowStats.inter8x8Cnt;
            totalSkip                                  += m_rows[i].rowStats.skip8x8Cnt;
        }
        int totalCuCount = totalI + totalP + totalSkip;
        m_frame[layer]->m_encData->m_frameStats.percent8x8Intra = (double)totalI / totalCuCount;
        m_frame[layer]->m_encData->m_frameStats.percent8x8Inter = (double)totalP / totalCuCount;
        m_frame[layer]->m_encData->m_frameStats.percent8x8Skip  = (double)totalSkip / totalCuCount;
    }

    if (m_param->csvLogLevel >= 1)
    {
        for (uint32_t i = 0; i < m_numRows; i++)
        {
            m_frame[layer]->m_encData->m_frameStats.cntIntraNxN += m_rows[i].rowStats.cntIntraNxN;
            m_frame[layer]->m_encData->m_frameStats.totalCu += m_rows[i].rowStats.totalCu;
            m_frame[layer]->m_encData->m_frameStats.totalCtu += m_rows[i].rowStats.totalCtu;
            m_frame[layer]->m_encData->m_frameStats.lumaDistortion += m_rows[i].rowStats.lumaDistortion;
            m_frame[layer]->m_encData->m_frameStats.chromaDistortion += m_rows[i].rowStats.chromaDistortion;
            m_frame[layer]->m_encData->m_frameStats.psyEnergy += m_rows[i].rowStats.psyEnergy;
            m_frame[layer]->m_encData->m_frameStats.ssimEnergy += m_rows[i].rowStats.ssimEnergy;
            m_frame[layer]->m_encData->m_frameStats.resEnergy += m_rows[i].rowStats.resEnergy;
            for (uint32_t depth = 0; depth <= m_param->maxCUDepth; depth++)
            {
                m_frame[layer]->m_encData->m_frameStats.cntSkipCu[depth] += m_rows[i].rowStats.cntSkipCu[depth];
                m_frame[layer]->m_encData->m_frameStats.cntMergeCu[depth] += m_rows[i].rowStats.cntMergeCu[depth];
                for (int m = 0; m < INTER_MODES; m++)
                    m_frame[layer]->m_encData->m_frameStats.cuInterDistribution[depth][m] += m_rows[i].rowStats.cuInterDistribution[depth][m];
                for (int n = 0; n < INTRA_MODES; n++)
                    m_frame[layer]->m_encData->m_frameStats.cuIntraDistribution[depth][n] += m_rows[i].rowStats.cuIntraDistribution[depth][n];
            }
        }
        m_frame[layer]->m_encData->m_frameStats.percentIntraNxN = (double)(m_frame[layer]->m_encData->m_frameStats.cntIntraNxN * 100) / m_frame[layer]->m_encData->m_frameStats.totalCu;

        for (uint32_t depth = 0; depth <= m_param->maxCUDepth; depth++)
        {
            m_frame[layer]->m_encData->m_frameStats.percentSkipCu[depth] = (double)(m_frame[layer]->m_encData->m_frameStats.cntSkipCu[depth] * 100) / m_frame[layer]->m_encData->m_frameStats.totalCu;
            m_frame[layer]->m_encData->m_frameStats.percentMergeCu[depth] = (double)(m_frame[layer]->m_encData->m_frameStats.cntMergeCu[depth] * 100) / m_frame[layer]->m_encData->m_frameStats.totalCu;
            for (int n = 0; n < INTRA_MODES; n++)
                m_frame[layer]->m_encData->m_frameStats.percentIntraDistribution[depth][n] = (double)(m_frame[layer]->m_encData->m_frameStats.cuIntraDistribution[depth][n] * 100) / m_frame[layer]->m_encData->m_frameStats.totalCu;
            uint64_t cuInterRectCnt = 0; // sum of Nx2N, 2NxN counts
            cuInterRectCnt += m_frame[layer]->m_encData->m_frameStats.cuInterDistribution[depth][1] + m_frame[layer]->m_encData->m_frameStats.cuInterDistribution[depth][2];
            m_frame[layer]->m_encData->m_frameStats.percentInterDistribution[depth][0] = (double)(m_frame[layer]->m_encData->m_frameStats.cuInterDistribution[depth][0] * 100) / m_frame[layer]->m_encData->m_frameStats.totalCu;
            m_frame[layer]->m_encData->m_frameStats.percentInterDistribution[depth][1] = (double)(cuInterRectCnt * 100) / m_frame[layer]->m_encData->m_frameStats.totalCu;
            m_frame[layer]->m_encData->m_frameStats.percentInterDistribution[depth][2] = (double)(m_frame[layer]->m_encData->m_frameStats.cuInterDistribution[depth][3] * 100) / m_frame[layer]->m_encData->m_frameStats.totalCu;
        }
    }

    if (m_param->csvLogLevel >= 2)
    {
        m_frame[layer]->m_encData->m_frameStats.avgLumaDistortion = (double)(m_frame[layer]->m_encData->m_frameStats.lumaDistortion) / m_frame[layer]->m_encData->m_frameStats.totalCtu;
        m_frame[layer]->m_encData->m_frameStats.avgChromaDistortion = (double)(m_frame[layer]->m_encData->m_frameStats.chromaDistortion) / m_frame[layer]->m_encData->m_frameStats.totalCtu;
        m_frame[layer]->m_encData->m_frameStats.avgPsyEnergy = (double)(m_frame[layer]->m_encData->m_frameStats.psyEnergy) / m_frame[layer]->m_encData->m_frameStats.totalCtu;
        m_frame[layer]->m_encData->m_frameStats.avgSsimEnergy = (double)(m_frame[layer]->m_encData->m_frameStats.ssimEnergy) / m_frame[layer]->m_encData->m_frameStats.totalCtu;
        m_frame[layer]->m_encData->m_frameStats.avgResEnergy = (double)(m_frame[layer]->m_encData->m_frameStats.resEnergy) / m_frame[layer]->m_encData->m_frameStats.totalCtu;
    }

    /* Block 14 - Final entropy coding khi bật SAO.
     * Các quyết định CU đã có; encodeSlice ghi cú pháp SAO và CTU qua CABAC
     * vào substreams. Không SAO thì rows đã ghi dữ liệu ở Block 12.
     * Phân tích CU chọn cách mã hóa; CABAC biểu diễn quyết định đó thành bit. */
    /* Reset 3/6 - Chuyển buffer tạm từ headers/SEI riêng sang slice header.
     * Các NAL trước đã được serialize; bỏ bản còn sót trong m_bs để không
     * ghép metadata vào header VCL. Với một slice, codeSliceHeader phía dưới
     * dùng trực tiếp buffer đã reset này. Với nhiều slices, mỗi lượt còn reset
     * riêng. encodeSlice (nếu có SAO) ghi CTU vào m_outStreams, nên reset m_bs
     * không làm mất CABAC slice data và cũng không reset CABAC context. */
    m_bs.resetBits();
    m_entropyCoder.load(m_initSliceContext);
    m_entropyCoder.setBitstream(&m_bs);

    // finish encode of each CTU row, only required when SAO is enabled
    if (slice->m_bUseSao)
        encodeSlice(0, layer);

    /* Block 15 - Đóng gói slice header + CABAC substreams thành VCL NAL.
     * serializeSubstreams ghép/escape dữ liệu vào buffer phụ của m_nalList,
     * trả độ dài để ghi WPP entry-point offsets; serialize sẽ nối dữ liệu đó
     * sau header trong m_bs. Một slice NAL có thể chứa nhiều row substreams.
     * Nhiều slices: lặp theo nhóm rows; một slice: gom toàn bộ substreams.
     * NAL type lấy từ slice, temporal ID được ghi theo cấu hình sublayers. */
    m_entropyCoder.setBitstream(&m_bs);

    if (m_param->maxSlices > 1)
    {
        uint32_t nextSliceRow = 0;

        for(uint32_t sliceId = 0; sliceId < m_param->maxSlices; sliceId++)
        {
            /* Reset 4/6 - Mỗi slice cần một header mới trong m_bs.
             * Lượt đầu có thể đã rỗng nhờ Reset 3; các lượt sau bỏ header của
             * slice trước đã serialize. Sau reset, ghi header/entry points
             * của slice hiện tại rồi ghép các row substreams tương ứng.
             * Không xóa NAL slice trước hay CABAC data trong m_outStreams.
             * Đây là một vị trí code nhưng chạy một lần cho mỗi slice. */
            m_bs.resetBits();

            const uint32_t sliceAddr = nextSliceRow * m_numCols;
            if (m_param->bOptRefListLengthPPS)
            {
                ScopedLock refIdxLock(m_top->m_sliceRefIdxLock);
                m_top->analyseRefIdx(slice->m_numRefIdx);
            }
            m_entropyCoder.codeSliceHeader(*slice, *m_frame[layer]->m_encData, sliceAddr, m_sliceAddrBits, slice->m_sliceQp, layer);

            // Find rows of current slice
            const uint32_t prevSliceRow = nextSliceRow;
            while(nextSliceRow < m_numRows && m_rows[nextSliceRow].sliceId == sliceId)
                nextSliceRow++;

            // serialize each row, record final lengths in slice header
            uint32_t maxStreamSize = m_nalList.serializeSubstreams(&m_substreamSizes[prevSliceRow], (nextSliceRow - prevSliceRow), &m_outStreams[prevSliceRow]);

            // complete the slice header by writing WPP row-starts
            m_entropyCoder.setBitstream(&m_bs);
            if (slice->m_pps->bEntropyCodingSyncEnabled)
                m_entropyCoder.codeSliceHeaderWPPEntryPoints(&m_substreamSizes[prevSliceRow], (nextSliceRow - prevSliceRow - 1), maxStreamSize);
            
            m_bs.writeByteAlignment();

            m_nalList.serialize(slice->m_nalUnitType, m_bs, layer, (!!m_param->bEnableTemporalSubLayers ? m_frame[layer]->m_tempLayer + 1 : (1 + (slice->m_nalUnitType == NAL_UNIT_CODED_SLICE_TSA_N))));
        }
    }
    else
    {
        if (m_param->bOptRefListLengthPPS)
        {
            ScopedLock refIdxLock(m_top->m_sliceRefIdxLock);
            m_top->analyseRefIdx(slice->m_numRefIdx);
        }
        m_entropyCoder.codeSliceHeader(*slice, *m_frame[layer]->m_encData, 0, 0, slice->m_sliceQp, layer);

        // serialize each row, record final lengths in slice header
        uint32_t maxStreamSize = m_nalList.serializeSubstreams(m_substreamSizes, numSubstreams, m_outStreams);

        // complete the slice header by writing WPP row-starts
        m_entropyCoder.setBitstream(&m_bs);
        if (slice->m_pps->bEntropyCodingSyncEnabled)
            m_entropyCoder.codeSliceHeaderWPPEntryPoints(m_substreamSizes, (slice->m_sps->numCuInHeight - 1), maxStreamSize);
        m_bs.writeByteAlignment();

        m_nalList.serialize(slice->m_nalUnitType, m_bs, layer, (!!m_param->bEnableTemporalSubLayers ? m_frame[layer]->m_tempLayer + 1 : (1 + (slice->m_nalUnitType == NAL_UNIT_CODED_SLICE_TSA_N))));
    }

    /* Block 16 - Ghi suffix SEI và tính số bit dùng cho rate control.
     * Decoded Picture Hash (MD5/CRC/checksum) nằm sau VCL để kiểm tra ảnh tái tạo.
     * Vòng tính bytes bỏ prefix/suffix SEI và trừ prefix theo công thức bên dưới,
     * nên accessUnitBits ở đây không phải tổng mọi byte đầu ra.
     * Layer bổ sung trừ phần base layer khỏi tổng tích lũy. */
    if (m_param->decodedPictureHashSEI)
        writeTrailingSEIMessages(layer);

    uint64_t bytes = 0;
    for (uint32_t i = 0; i < m_nalList.m_numNal; i++)
    {
        int type = m_nalList.m_nal[i].type;

        // exclude SEI
        if (type != NAL_UNIT_PREFIX_SEI && type != NAL_UNIT_SUFFIX_SEI)
        {
            bytes += m_nalList.m_nal[i].sizeBytes;
            // and exclude start code prefix
            bytes -= (!i || type == NAL_UNIT_SPS || type == NAL_UNIT_PPS) ? 4 : 3;
        }
    }
    m_accessUnitBits[layer] = (layer) ? (bytes - (m_accessUnitBits[0] >> 3)) << 3 : bytes << 3;

    int filler = 0;
    // Chỉ base layer gọi rateControlEnd: cập nhật bits thực tế và yêu cầu filler.
    /* rateControlEnd may also block for earlier frames to call rateControlUpdateStats */
    if (!layer && m_top->m_rateControl->rateControlEnd(m_frame[layer], m_accessUnitBits[layer], &m_rce, &filler) < 0)
        m_top->m_aborted = true;

#if ENABLE_ALPHA
    if (layer && m_param->numScalableLayers > 1)
        m_frame[layer]->m_encData->m_avgQpAq = m_frame[layer]->m_encData->m_avgQpRc;
#endif
#if ENABLE_MULTIVIEW
    if (layer && m_param->numViews > 1)
    {
        double avgQpAq = 0;
        for (uint32_t i = 0; i < slice->m_sps->numCuInHeight; i++)
            avgQpAq += m_frame[layer]->m_encData->m_rowStat[i].sumQpAq;

        avgQpAq /= (slice->m_sps->numCUsInFrame * m_param->num4x4Partitions);
        m_frame[layer]->m_encData->m_avgQpAq = avgQpAq;
    }
#endif

    /* Block 17 - Bổ sung filler và RPU nếu cần.
     * Rate control trả filler theo bit; trừ overhead rồi đổi sang byte 0xff,
     * đóng gói FILLER_DATA và cập nhật lại kích thước tính cho rate control. */
    if (filler > 0)
    {
        filler = (filler - FILLER_OVERHEAD * 8) >> 3;
        /* Reset 5/6 - Bắt đầu payload FILLER_DATA riêng.
         * m_bs có thể còn slice header, hoặc suffix SEI hash nếu vừa phát hash.
         * Các NAL đó đã serialize nên bỏ phần còn sót trong buffer tạm.
         * Tiếp theo chỉ ghi các byte 0xff và bit kết thúc/căn chỉnh, tránh
         * đóng gói nhầm cú pháp slice/SEI vào NAL filler. Chỉ chạy khi có filler. */
        m_bs.resetBits();
        while (filler > 0)
        {
            m_bs.write(0xff, 8);
            filler--;
        }
        m_bs.writeByteAlignment();
        m_nalList.serialize(NAL_UNIT_FILLER_DATA, m_bs);
        bytes += m_nalList.m_nal[m_nalList.m_numNal - 1].sizeBytes;
        bytes -= 3; //exclude start code prefix
        m_accessUnitBits[layer] = bytes << 3;
    }

    // RPU là metadata bổ sung (Dolby Vision), ở đây ghi vào NAL_UNSPECIFIED.
    if (m_frame[layer]->m_rpu.payloadSize)
    {
        /* Reset 6/6 - Bắt đầu payload RPU riêng khi frame có metadata này.
         * Bỏ dữ liệu filler nếu vừa ghi filler, hoặc nội dung NAL trước đó.
         * Sau reset, chép đúng các byte m_rpu.payload và serialize UNSPECIFIED;
         * RPU không bị nối sau phần filler/slice/SEI còn sót trong m_bs.
         * Các NAL đã serialize vẫn còn nguyên trong m_nalList. */
        m_bs.resetBits();
        for (int i = 0; i < m_frame[layer]->m_rpu.payloadSize; i++)
            m_bs.write(m_frame[layer]->m_rpu.payload[i], 8);
        m_nalList.serialize(NAL_UNIT_UNSPECIFIED, m_bs);
    }

    /* Block 18 - Hoàn tất picture và trả quyền giữ references.
     * Giảm m_countRefEncoders để báo encoder này không còn dùng reference;
     * reference chỉ được tái sử dụng khi các ràng buộc giữ ảnh khác cũng hết.
     * Tổng hợp noise reduction từ TLDs, cập nhật hệ số rồi phân phối lại;
     * cộng CU statistics nếu build bật và ghi timestamp kết thúc frame. */
    m_endCompressTime[layer] = x265_mdate();
    X265_FLOW_LOG("FrameEncoder",
                  "[FRAME encoder=%d frame=%d layer=%d] DONE object=Frame@%p stage=compress nals=%u access_unit_bits=%llu elapsed_us=%lld owner=FrameEncoder-%d",
                  m_jpId, m_frame[layer]->m_poc, layer, m_frame[layer],
                  m_nalList.m_numNal, (unsigned long long)m_accessUnitBits[layer],
                  (long long)(m_endCompressTime[layer] - m_startCompressTime[layer]), m_jpId);

    /* Decrement referenced frame reference counts, allow them to be recycled */
    for (int l = 0; l < numPredDir; l++)
    {
        for (int ref = 0; ref < slice->m_numRefIdx[l]; ref++)
        {
            Frame *refpic = slice->m_refFrameList[l][ref];
            ATOMIC_DEC(&refpic->m_countRefEncoders);
        }
    }

    if (m_nr)
    {
        bool nrEnabled = (m_rce.newQp < QP_MAX_SPEC || !m_param->rc.vbvBufferSize) && (m_param->noiseReductionIntra || m_param->noiseReductionInter);

        if (nrEnabled)
        {
            /* Accumulate NR statistics from all worker threads */
            for (int i = 0; i < numTLD; i++)
            {
                NoiseReduction* nr = &m_tld[i].analysis.m_quant.m_frameNr[m_jpId];
                for (int cat = 0; cat < MAX_NUM_TR_CATEGORIES; cat++)
                {
                    for (int coeff = 0; coeff < MAX_NUM_TR_COEFFS; coeff++)
                        m_nr->nrResidualSum[cat][coeff] += nr->nrResidualSum[cat][coeff];

                    m_nr->nrCount[cat] += nr->nrCount[cat];
                }
            }

            noiseReductionUpdate();

            /* Copy updated NR coefficients back to all worker threads */
            for (int i = 0; i < numTLD; i++)
            {
                NoiseReduction* nr = &m_tld[i].analysis.m_quant.m_frameNr[m_jpId];
                memcpy(nr->nrOffsetDenoise, m_nr->nrOffsetDenoise, sizeof(uint16_t)* MAX_NUM_TR_CATEGORIES * MAX_NUM_TR_COEFFS);
                memset(nr->nrCount, 0, sizeof(uint32_t)* MAX_NUM_TR_CATEGORIES);
                memset(nr->nrResidualSum, 0, sizeof(uint32_t)* MAX_NUM_TR_CATEGORIES * MAX_NUM_TR_COEFFS);
            }
        }
    }

#if DETAILED_CU_STATS
    /* Accumulate CU statistics from each worker thread, we could report
     * per-frame stats here, but currently we do not. */
    for (int i = 0; i < numTLD; i++)
        m_cuStats.accumulate(m_tld[i].analysis.m_stats[m_jpId], *m_param);
#endif

    m_endFrameTime[layer] = x265_mdate();
}

void FrameEncoder::initDecodedPictureHashSEI(int row, int cuAddr, int height, int layer)
{
    PicYuv *reconPic = m_frame[layer]->m_reconPic[0];
    uint32_t width = reconPic->m_picWidth;	
    intptr_t stride = reconPic->m_stride;
    uint32_t maxCUHeight = m_param->maxCUSize;

    const uint32_t hChromaShift = CHROMA_H_SHIFT(m_param->internalCsp);
    const uint32_t vChromaShift = CHROMA_V_SHIFT(m_param->internalCsp);

    if (m_param->decodedPictureHashSEI == 1)
    {
        if (!row)
            MD5Init(&m_seiReconPictureDigest.m_state[0]);

        updateMD5Plane(m_seiReconPictureDigest.m_state[0], reconPic->getLumaAddr(cuAddr), width, height, stride);
        if (m_param->internalCsp != X265_CSP_I400)
        {
            if (!row)
            {
                MD5Init(&m_seiReconPictureDigest.m_state[1]);
                MD5Init(&m_seiReconPictureDigest.m_state[2]);
            }

            width >>= hChromaShift;
            height >>= vChromaShift;
            stride = reconPic->m_strideC;

            updateMD5Plane(m_seiReconPictureDigest.m_state[1], reconPic->getCbAddr(cuAddr), width, height, stride);
            updateMD5Plane(m_seiReconPictureDigest.m_state[2], reconPic->getCrAddr(cuAddr), width, height, stride);
        }
    }
    else if (m_param->decodedPictureHashSEI == 2)
    {

        if (!row)
            m_seiReconPictureDigest.m_crc[0] = 0xffff;

        updateCRC(reconPic->getLumaAddr(cuAddr), m_seiReconPictureDigest.m_crc[0], height, width, stride);
        if (m_param->internalCsp != X265_CSP_I400)
        {
            width >>= hChromaShift;
            height >>= vChromaShift;
            stride = reconPic->m_strideC;
            m_seiReconPictureDigest.m_crc[1] = m_seiReconPictureDigest.m_crc[2] = 0xffff;

            updateCRC(reconPic->getCbAddr(cuAddr), m_seiReconPictureDigest.m_crc[1], height, width, stride);
            updateCRC(reconPic->getCrAddr(cuAddr), m_seiReconPictureDigest.m_crc[2], height, width, stride);
        }
    }
    else if (m_param->decodedPictureHashSEI == 3)
    {
        if (!row)
            m_seiReconPictureDigest.m_checksum[0] = 0;

        updateChecksum(reconPic->m_picOrg[0], m_seiReconPictureDigest.m_checksum[0], height, width, stride, row, maxCUHeight);
        if (m_param->internalCsp != X265_CSP_I400)
        {
            width >>= hChromaShift;
            height >>= vChromaShift;
            stride = reconPic->m_strideC;
            maxCUHeight >>= vChromaShift;

            if (!row)
                m_seiReconPictureDigest.m_checksum[1] = m_seiReconPictureDigest.m_checksum[2] = 0;

            updateChecksum(reconPic->m_picOrg[1], m_seiReconPictureDigest.m_checksum[1], height, width, stride, row, maxCUHeight);
            updateChecksum(reconPic->m_picOrg[2], m_seiReconPictureDigest.m_checksum[2], height, width, stride, row, maxCUHeight);
        }
    }
}

void FrameEncoder::encodeSlice(uint32_t sliceAddr, int layer)
{
    Slice* slice = m_frame[layer]->m_encData->m_slice;
    const uint32_t widthInLCUs = slice->m_sps->numCuInWidth;
    const uint32_t lastCUAddr = (slice->m_endCUAddr + m_param->num4x4Partitions - 1) / m_param->num4x4Partitions;
    const uint32_t numSubstreams = m_param->bEnableWavefront ? slice->m_sps->numCuInHeight : 1;

    SAOParam* saoParam = slice->m_sps->bUseSAO && slice->m_bUseSao ? m_frame[layer]->m_encData->m_saoParam : NULL;
    for (uint32_t cuAddr = sliceAddr; cuAddr < lastCUAddr; cuAddr++)
    {
        uint32_t col = cuAddr % widthInLCUs;
        uint32_t row = cuAddr / widthInLCUs;
        uint32_t subStrm = row % numSubstreams;
        CUData* ctu = m_frame[layer]->m_encData->getPicCTU(cuAddr);

        m_entropyCoder.setBitstream(&m_outStreams[subStrm]);

        // Synchronize cabac probabilities with upper-right CTU if it's available and we're at the start of a line.
        if (m_param->bEnableWavefront && !col && row)
        {
            m_entropyCoder.copyState(m_initSliceContext);
            m_entropyCoder.loadContexts(m_rows[row - 1].bufferedEntropy);
        }

        // Initialize slice context
        if (ctu->m_bFirstRowInSlice && !col)
            m_entropyCoder.load(m_initSliceContext);

        if (saoParam)
        {
            if (saoParam->bSaoFlag[0] || saoParam->bSaoFlag[1])
            {
                int mergeLeft = col && saoParam->ctuParam[0][cuAddr].mergeMode == SAO_MERGE_LEFT;
                int mergeUp = !ctu->m_bFirstRowInSlice && saoParam->ctuParam[0][cuAddr].mergeMode == SAO_MERGE_UP;
                if (col)
                    m_entropyCoder.codeSaoMerge(mergeLeft);
                if (!ctu->m_bFirstRowInSlice && !mergeLeft)
                    m_entropyCoder.codeSaoMerge(mergeUp);
                if (!mergeLeft && !mergeUp)
                {
                    if (saoParam->bSaoFlag[0])
                        m_entropyCoder.codeSaoOffset(saoParam->ctuParam[0][cuAddr], 0);
                    if (saoParam->bSaoFlag[1])
                    {
                        m_entropyCoder.codeSaoOffset(saoParam->ctuParam[1][cuAddr], 1);
                        m_entropyCoder.codeSaoOffset(saoParam->ctuParam[2][cuAddr], 2);
                    }
                }
            }
            else
            {
                for (int i = 0; i < (m_param->internalCsp != X265_CSP_I400 ? 3 : 1); i++)
                    saoParam->ctuParam[i][cuAddr].reset();
            }
        }

        // final coding (bitstream generation) for this CU
        X265_FLOW_LOG("FrameEncoder",
                      "[CTU encoder=%d frame=%d layer=%d ctu=%u row=%u col=%u] BEGIN stage=final-bitstream object=CUData@%p step_owner=FrameEncoder-%d",
                      m_jpId, m_frame[layer]->m_poc, layer, cuAddr, row, col, ctu, m_jpId);
        m_entropyCoder.encodeCTU(*ctu, m_cuGeoms[m_ctuGeomMap[cuAddr]]);
        X265_FLOW_LOG("FrameEncoder",
                      "[CTU encoder=%d frame=%d layer=%d ctu=%u row=%u col=%u] DONE stage=final-bitstream step_owner=FrameEncoder-%d",
                      m_jpId, m_frame[layer]->m_poc, layer, cuAddr, row, col, m_jpId);

        if (m_param->bEnableWavefront)
        {
            if (col == 1)
                // Store probabilities of second CTU in line into buffer
                m_rows[row].bufferedEntropy.loadContexts(m_entropyCoder);

            if (col == widthInLCUs - 1)
                m_entropyCoder.finishSlice();
        }
    }

    if (!m_param->bEnableWavefront)
        m_entropyCoder.finishSlice();
}

void FrameEncoder::processRow(int row, int threadId, int layer)
{
    int64_t startTime = x265_mdate();
    if (++m_activeWorkerCount == 1 && m_stallStartTime[layer])
        m_totalNoWorkerTime[layer].add(x265_mdate() - m_stallStartTime[layer]);

    const uint32_t realRow = m_idx_to_row[row >> 1];
    const uint32_t typeNum = m_idx_to_row[row & 1];

    if (!typeNum)
    {
        processRowEncoder(realRow, m_tld[threadId], layer);
    }
    else
    {
        m_frameFilter.processRow(realRow, layer);

        // NOTE: Active next row
        if (realRow != m_sliceBaseRow[m_rows[realRow].sliceId + 1] - 1)
            enqueueRowFilter(m_row_to_idx[realRow + 1]);
    }

    if (--m_activeWorkerCount == 0)
        m_stallStartTime[layer] = x265_mdate();

    m_totalWorkerElapsedTime[layer].add(x265_mdate() - startTime);
}

// Called by worker threads
void FrameEncoder::processRowEncoder(int intRow, ThreadLocalData& tld, int layer)
{
    const uint32_t row = (uint32_t)intRow;
    CTURow& curRow = m_rows[row];

    if (m_param->bEnableWavefront)
    {
        ScopedLock self(curRow.lock);
        if (!curRow.active)
            /* VBV restart is in progress, exit out */
            return;
        if (curRow.busy)
        {
            /* On multi-socket Windows servers, we have seen problems with
             * ATOMIC_CAS which resulted in multiple worker threads processing
             * the same CU row, which often resulted in bad pointer accesses. We
             * believe the problem is fixed, but are leaving this check in place
             * to prevent crashes in case it is not */
            x265_log(m_param, X265_LOG_WARNING,
                     "internal error - simultaneous row access detected. Please report HW to x265-devel@videolan.org\n");
            return;
        }
        curRow.busy = true;
    }

    /* When WPP is enabled, every row has its own row coder instance. Otherwise
     * they share row 0 */
    Entropy& rowCoder = m_param->bEnableWavefront ? curRow.rowGoOnCoder : m_rows[0].rowGoOnCoder;
    FrameData& curEncData = *m_frame[layer]->m_encData;
    Slice *slice = curEncData.m_slice;

    const uint32_t numCols = m_numCols;
    const uint32_t lineStartCUAddr = row * numCols;
    bool bIsVbv = m_param->rc.vbvBufferSize > 0 && m_param->rc.vbvMaxBitrate > 0;

    const uint32_t sliceId = curRow.sliceId;
    uint32_t maxBlockCols = (m_frame[layer]->m_fencPic->m_picWidth + (16 - 1)) / 16;
    uint32_t noOfBlocks = m_param->maxCUSize / 16;
    const uint32_t bFirstRowInSlice = ((row == 0) || (m_rows[row - 1].sliceId != curRow.sliceId)) ? 1 : 0;
    const uint32_t bLastRowInSlice = ((row == m_numRows - 1) || (m_rows[row + 1].sliceId != curRow.sliceId)) ? 1 : 0;
    const uint32_t endRowInSlicePlus1 = m_sliceBaseRow[sliceId + 1];
    const uint32_t rowInSlice = row - m_sliceBaseRow[sliceId];

    // Load SBAC coder context from previous row and initialize row state.
    if (bFirstRowInSlice && !curRow.completed)        
        rowCoder.load(m_initSliceContext);     

    // calculate mean QP for consistent deltaQP signalling calculation
    if (m_param->bOptCUDeltaQP)
    {
        ScopedLock self(curRow.lock);
        if (!curRow.avgQPComputed)
        {
            if (m_param->bEnableWavefront || !row)
            {
                double meanQPOff = 0;
                bool isReferenced = IS_REFERENCED(m_frame[layer]);
                double *qpoffs = (isReferenced && m_param->rc.cuTree) ? m_frame[layer]->m_lowres.qpCuTreeOffset : m_frame[layer]->m_lowres.qpAqOffset;
                if (qpoffs)
                {
                    uint32_t loopIncr = (m_param->rc.qgSize == 8) ? 8 : 16;

                    uint32_t cuYStart = 0, height = m_frame[layer]->m_fencPic->m_picHeight;
                    if (m_param->bEnableWavefront)
                    {
                        cuYStart = intRow * m_param->maxCUSize;
                        height = cuYStart + m_param->maxCUSize;
                    }

                    uint32_t qgSize = m_param->rc.qgSize, width = m_frame[layer]->m_fencPic->m_picWidth;
                    uint32_t maxOffsetCols = (m_frame[layer]->m_fencPic->m_picWidth + (loopIncr - 1)) / loopIncr;
                    uint32_t count = 0;
                    for (uint32_t cuY = cuYStart; cuY < height && (cuY < m_frame[layer]->m_fencPic->m_picHeight); cuY += qgSize)
                    {
                        for (uint32_t cuX = 0; cuX < width; cuX += qgSize)
                        {
                            double qp_offset = 0;
                            uint32_t cnt = 0;

                            for (uint32_t block_yy = cuY; block_yy < cuY + qgSize && block_yy < m_frame[layer]->m_fencPic->m_picHeight; block_yy += loopIncr)
                            {
                                for (uint32_t block_xx = cuX; block_xx < cuX + qgSize && block_xx < width; block_xx += loopIncr)
                                {
                                    int idx = ((block_yy / loopIncr) * (maxOffsetCols)) + (block_xx / loopIncr);
                                    qp_offset += qpoffs[idx];
                                    cnt++;
                                }
                            }
                            qp_offset /= cnt;
                            meanQPOff += qp_offset;
                            count++;
                        }
                    }
                    meanQPOff /= count;
                }
                rowCoder.m_meanQP = slice->m_sliceQp + meanQPOff;
            }
            else
            {
                rowCoder.m_meanQP = m_rows[0].rowGoOnCoder.m_meanQP;
            }
            curRow.avgQPComputed = 1;
        }
    }

    // Initialize restrict on MV range in slices
    tld.analysis.m_sliceMinY = -(int32_t)(rowInSlice * m_param->maxCUSize * 4) + 3 * 4;
    tld.analysis.m_sliceMaxY = (int32_t)((endRowInSlicePlus1 - 1 - row) * (m_param->maxCUSize * 4) - 4 * 4);

    // Handle single row slice
    if (tld.analysis.m_sliceMaxY < tld.analysis.m_sliceMinY)
        tld.analysis.m_sliceMaxY = tld.analysis.m_sliceMinY = 0;

    if (m_top->m_threadedME && !slice->isIntra())
    {
        ScopedLock lock(m_tmeDepLock);
        m_tmeDeps[row].internal = true;
        m_top->m_threadedME->enqueueReadyRows(row, layer, this);
    }

    while (curRow.completed < numCols)
    {
        ProfileScopeEvent(encodeCTU);

        const uint32_t col = curRow.completed;
        const uint32_t cuAddr = lineStartCUAddr + col;
        CUData* ctu = curEncData.getPicCTU(cuAddr);
        const uint32_t bLastCuInSlice = (bLastRowInSlice & (col == numCols - 1)) ? 1 : 0;

        /* Must wait for TME to finish before initCTU because both threads
         * operate on the same CUData — the encoder's initCTU would corrupt
         * data that deriveMVsForCTU is still reading. */
        if (m_top->m_threadedME && slice->m_sliceType != I_SLICE)
        {
            int64_t waitStart = x265_mdate();
            bool waited = false;

            while (m_frame[layer]->m_ctuMEFlags[cuAddr].get() == 0)
            {
#ifdef DETAILED_CU_STATS
                tld.analysis.m_stats[m_jpId].countTmeBlockedCTUs++;
#endif
                m_frame[layer]->m_ctuMEFlags[cuAddr].waitForChange(0);
                waited = true;
            }

            int64_t waitEnd = x265_mdate();
            if (waited)
                ATOMIC_ADD(&m_totalThreadedMEWait[layer], waitEnd - waitStart);
        }

        ctu->initCTU(*m_frame[layer], cuAddr, slice->m_sliceQp, bFirstRowInSlice, bLastRowInSlice, bLastCuInSlice);

        if (!layer && bIsVbv)
        {
            if (col == 0 && !m_param->bEnableWavefront)
            {
                m_backupStreams[0].copyBits(&m_outStreams[0]);
                curRow.bufferedEntropy.copyState(rowCoder);
                curRow.bufferedEntropy.loadContexts(rowCoder);
            }
            if (bFirstRowInSlice && m_vbvResetTriggerRow[curRow.sliceId] != intRow)
            {
                curEncData.m_rowStat[row].rowQp = curEncData.m_avgQpRc;
                curEncData.m_rowStat[row].rowQpScale = x265_qp2qScale(curEncData.m_avgQpRc);
            }

            FrameData::RCStatCU& cuStat = curEncData.m_cuStat[cuAddr];
            if (m_param->bEnableWavefront && rowInSlice >= col && !bFirstRowInSlice && m_vbvResetTriggerRow[curRow.sliceId] != intRow)
                cuStat.baseQp = curEncData.m_cuStat[cuAddr - numCols + 1].baseQp;
            else if (!m_param->bEnableWavefront && !bFirstRowInSlice && m_vbvResetTriggerRow[curRow.sliceId] != intRow)
                cuStat.baseQp = curEncData.m_rowStat[row - 1].rowQp;
            else
                cuStat.baseQp = curEncData.m_rowStat[row].rowQp;

            /* TODO: use defines from slicetype.h for lowres block size */
            uint32_t block_y = (ctu->m_cuPelY >> m_param->maxLog2CUSize) * noOfBlocks;
            uint32_t block_x = (ctu->m_cuPelX >> m_param->maxLog2CUSize) * noOfBlocks;
            if (!strlen(m_param->analysisLoad) || !m_param->bDisableLookahead)
            {
                cuStat.vbvCost = 0;
                cuStat.intraVbvCost = 0;

                for (uint32_t h = 0; h < noOfBlocks && block_y < m_sliceMaxBlockRow[sliceId + 1]; h++, block_y++)
                {
                    uint32_t idx = block_x + (block_y * maxBlockCols);

                    for (uint32_t w = 0; w < noOfBlocks && (block_x + w) < maxBlockCols; w++, idx++)
                    {
                        cuStat.vbvCost += m_frame[layer]->m_lowres.lowresCostForRc[idx] & LOWRES_COST_MASK;
                        cuStat.intraVbvCost += m_frame[layer]->m_lowres.intraCost[idx];
                    }
                }
            }
        }
        else
            curEncData.m_cuStat[cuAddr].baseQp = curEncData.m_avgQpRc;

        if (m_param->bEnableWavefront && !col && !bFirstRowInSlice)
        {
            // Load SBAC coder context from previous row and initialize row state.
            rowCoder.copyState(m_initSliceContext);
            rowCoder.loadContexts(m_rows[row - 1].bufferedEntropy);
        }
        if (m_param->dynamicRd && (int32_t)(m_rce.qpaRc - m_rce.qpNoVbv) > 0)
            ctu->m_vbvAffected = true;

        // Does all the CU analysis, returns best top level mode decision
        X265_FLOW_LOG("FrameEncoder",
                      "[CTU encoder=%d frame=%d layer=%d ctu=%u row=%u col=%u] BEGIN stage=analysis object=CUData@%p owner=FrameEncoder-%d",
                      m_jpId, m_frame[layer]->m_poc, layer, cuAddr, row, col, ctu, m_jpId);
        Mode& best = tld.analysis.compressCTU(*ctu, *m_frame[layer], m_cuGeoms[m_ctuGeomMap[cuAddr]], rowCoder);
        X265_FLOW_LOG("FrameEncoder",
                      "[CTU encoder=%d frame=%d layer=%d ctu=%u row=%u col=%u] DONE stage=analysis pred_mode=%u depth=%u rd_cost=%llu bits=%u owner=FrameEncoder-%d",
                      m_jpId, m_frame[layer]->m_poc, layer, cuAddr, row, col,
                      (unsigned)best.cu.m_predMode[0], (unsigned)best.cu.m_cuDepth[0],
                      (unsigned long long)best.rdCost, best.totalBits, m_jpId);

        /* startPoint > encodeOrder is true when the start point changes for
        a new GOP but few frames from the previous GOP is still incomplete.
        The data of frames in this interval will not be used by any future frames. */
        if (m_param->bDynamicRefine && m_top->m_startPoint <= m_frame[layer]->m_encodeOrder)
            collectDynDataRow(*ctu, &curRow.rowStats);

        // take a sample of the current active worker count
        m_totalActiveWorkerCount.fetchAdd(m_activeWorkerCount);
        ATOMIC_INC(&m_activeWorkerCountSamples);

        /* advance top-level row coder to include the context of this CTU.
         * if SAO is disabled, rowCoder writes the final CTU bitstream */
        X265_FLOW_LOG("FrameEncoder",
                      "[CTU encoder=%d frame=%d layer=%d ctu=%u row=%u col=%u] BEGIN stage=row-context object=CUData@%p owner=FrameEncoder-%d",
                      m_jpId, m_frame[layer]->m_poc, layer, cuAddr, row, col, ctu, m_jpId);
        rowCoder.encodeCTU(*ctu, m_cuGeoms[m_ctuGeomMap[cuAddr]]);
        X265_FLOW_LOG("FrameEncoder",
                      "[CTU encoder=%d frame=%d layer=%d ctu=%u row=%u col=%u] DONE stage=row-context owner=FrameEncoder-%d",
                      m_jpId, m_frame[layer]->m_poc, layer, cuAddr, row, col, m_jpId);

        if (m_param->bEnableWavefront && col == 1)
            // Save CABAC state for next row
            curRow.bufferedEntropy.loadContexts(rowCoder);

        /* SAO parameter estimation using non-deblocked pixels for CTU bottom and right boundary areas */
        if (slice->m_bUseSao && m_param->bSaoNonDeblocked)
            m_frameFilter.m_parallelFilter[row].m_sao.calcSaoStatsCu_BeforeDblk(m_frame[layer], col, row);

        /* Deblock with idle threading */
        if (m_param->bEnableLoopFilter | slice->m_bUseSao)
        {
            // NOTE: in VBV mode, we may reencode anytime, so we can't do Deblock stage-Horizon and SAO
            if (!bIsVbv)
            {
                // Delay one row to avoid intra prediction conflict
                if (m_pool && !bFirstRowInSlice)
                {                    
                    int allowCol = col;

                    // avoid race condition on last column
                    if (rowInSlice >= 2)
                    {
                        allowCol = X265_MIN(((col == numCols - 1) ? m_frameFilter.m_parallelFilter[row - 2].m_lastDeblocked.get()
                                                                  : m_frameFilter.m_parallelFilter[row - 2].m_lastCol.get()), (int)col);
                    }
                    m_frameFilter.m_parallelFilter[row - 1].m_allowedCol.set(allowCol);
                }

                // Last Row may start early
                if (m_pool && bLastRowInSlice)
                {
                    // Deblocking last row
                    int allowCol = col;

                    // avoid race condition on last column
                    if (rowInSlice >= 2)
                    {
                        allowCol = X265_MIN(((col == numCols - 1) ? m_frameFilter.m_parallelFilter[row - 1].m_lastDeblocked.get()
                                                                  : m_frameFilter.m_parallelFilter[row - 1].m_lastCol.get()), (int)col);
                    }
                    m_frameFilter.m_parallelFilter[row].m_allowedCol.set(allowCol);
                }
            } // end of !bIsVbv
        }
        // Both Loopfilter and SAO Disabled
        else
        {
            m_frameFilter.m_parallelFilter[row].processPostCu(col);
        }

        // Completed CU processing
        curRow.completed++;

        FrameStats frameLog;
        curEncData.m_rowStat[row].sumQpAq += collectCTUStatistics(*ctu, &frameLog);

        // copy number of intra, inter cu per row into frame stats for 2 pass
        if (m_param->rc.bStatWrite)
        {
            curRow.rowStats.mvBits    += best.mvBits;
            curRow.rowStats.coeffBits += best.coeffBits;
            curRow.rowStats.miscBits  += best.totalBits - (best.mvBits + best.coeffBits);

            for (uint32_t depth = 0; depth <= m_param->maxCUDepth; depth++)
            {
                /* 1 << shift == number of 8x8 blocks at current depth */
                int shift = 2 * (m_param->maxCUDepth - depth);
                int cuSize = m_param->maxCUSize >> depth;

                curRow.rowStats.intra8x8Cnt += (cuSize == 8) ? (int)(frameLog.cntIntra[depth] + frameLog.cntIntraNxN) :
                                                               (int)(frameLog.cntIntra[depth] << shift);

                curRow.rowStats.inter8x8Cnt += (int)(frameLog.cntInter[depth] << shift);
                curRow.rowStats.skip8x8Cnt += (int)((frameLog.cntSkipCu[depth] + frameLog.cntMergeCu[depth]) << shift);
            }
        }
        curRow.rowStats.totalCtu++;
        curRow.rowStats.lumaDistortion   += best.lumaDistortion;
        curRow.rowStats.chromaDistortion += best.chromaDistortion;
        curRow.rowStats.psyEnergy        += best.psyEnergy;
        curRow.rowStats.ssimEnergy       += best.ssimEnergy;
        curRow.rowStats.resEnergy        += best.resEnergy;
        curRow.rowStats.cntIntraNxN      += frameLog.cntIntraNxN;
        curRow.rowStats.totalCu          += frameLog.totalCu;
        for (uint32_t depth = 0; depth <= m_param->maxCUDepth; depth++)
        {
            curRow.rowStats.cntSkipCu[depth] += frameLog.cntSkipCu[depth];
            curRow.rowStats.cntMergeCu[depth] += frameLog.cntMergeCu[depth];
            for (int m = 0; m < INTER_MODES; m++)
                curRow.rowStats.cuInterDistribution[depth][m] += frameLog.cuInterDistribution[depth][m];
            for (int n = 0; n < INTRA_MODES; n++)
                curRow.rowStats.cuIntraDistribution[depth][n] += frameLog.cuIntraDistribution[depth][n];
        }

        curEncData.m_cuStat[cuAddr].totalBits = best.totalBits;
        x265_emms();

        if (!layer && bIsVbv)
        {   
            // Update encoded bits, satdCost, baseQP for each CU if tune grain is disabled
            FrameData::RCStatCU& cuStat = curEncData.m_cuStat[cuAddr];    
            if ((m_param->bEnableWavefront && ((cuAddr == m_sliceBaseRow[sliceId] * numCols) || !m_param->rc.bEnableConstVbv)) || !m_param->bEnableWavefront)
            {
                curEncData.m_rowStat[row].rowSatd.fetchAdd(cuStat.vbvCost);
                curEncData.m_rowStat[row].rowIntraSatd.fetchAdd(cuStat.intraVbvCost);
                curEncData.m_rowStat[row].encodedBits.fetchAdd(cuStat.totalBits);
                curEncData.m_rowStat[row].sumQpRc += cuStat.baseQp;
                curEncData.m_rowStat[row].numEncodedCUs = cuAddr;
            }
            
            // If current block is at row end checkpoint, call vbv ratecontrol.
            if (!m_param->bEnableWavefront && col == numCols - 1)
            {
                double qpBase = curEncData.m_cuStat[cuAddr].baseQp;
                curRow.reEncode = m_top->m_rateControl->rowVbvRateControl(m_frame[layer], row, &m_rce, qpBase, m_sliceBaseRow, sliceId);
                qpBase = x265_clip3((double)m_param->rc.qpMin, (double)m_param->rc.qpMax, qpBase);
                curEncData.m_rowStat[row].rowQp = qpBase;
                curEncData.m_rowStat[row].rowQpScale = x265_qp2qScale(qpBase);
                if (curRow.reEncode < 0)
                {
                    x265_log(m_param, X265_LOG_DEBUG, "POC %d row %d - encode restart required for VBV, to %.2f from %.2f\n",
                        m_frame[layer]->m_poc, row, qpBase, curEncData.m_cuStat[cuAddr].baseQp);

                    m_vbvResetTriggerRow[curRow.sliceId] = row;
                    m_outStreams[0].copyBits(&m_backupStreams[0]);

                    rowCoder.copyState(curRow.bufferedEntropy);
                    rowCoder.loadContexts(curRow.bufferedEntropy);

                    curRow.completed = 0;
                    memset(&curRow.rowStats, 0, sizeof(curRow.rowStats));
                    curEncData.m_rowStat[row].numEncodedCUs = 0;
                    curEncData.m_rowStat[row].encodedBits = 0;
                    curEncData.m_rowStat[row].rowSatd = 0;
                    curEncData.m_rowStat[row].rowIntraSatd = 0;
                    curEncData.m_rowStat[row].sumQpRc = 0;
                    curEncData.m_rowStat[row].sumQpAq = 0;
                }
            }
            // If current block is at row diagonal checkpoint, call vbv ratecontrol.
            else if (m_param->bEnableWavefront && rowInSlice == col && !bFirstRowInSlice)
            {
                if (m_param->rc.bEnableConstVbv)
                {
                    uint32_t startCuAddr = numCols * row;
                    uint32_t EndCuAddr = startCuAddr + col;

                    for (int32_t r = row; r >= (int32_t)m_sliceBaseRow[sliceId]; r--)
                    {
                        for (uint32_t c = startCuAddr; c <= EndCuAddr && c <= numCols * (r + 1) - 1; c++)
                        {
                            curEncData.m_rowStat[r].rowSatd.fetchAdd(curEncData.m_cuStat[c].vbvCost);
                            curEncData.m_rowStat[r].rowIntraSatd.fetchAdd(curEncData.m_cuStat[c].intraVbvCost);
                            curEncData.m_rowStat[r].encodedBits.fetchAdd(curEncData.m_cuStat[c].totalBits);
                            curEncData.m_rowStat[r].sumQpRc += curEncData.m_cuStat[c].baseQp;
                            curEncData.m_rowStat[r].numEncodedCUs = c;
                        }
                        if (curRow.reEncode < 0)
                            break;
                        startCuAddr = EndCuAddr - numCols;
                        EndCuAddr = startCuAddr + 1;
                    }
                }
                double qpBase = curEncData.m_cuStat[cuAddr].baseQp;
                curRow.reEncode = m_top->m_rateControl->rowVbvRateControl(m_frame[layer], row, &m_rce, qpBase, m_sliceBaseRow, sliceId);
                qpBase = x265_clip3((double)m_param->rc.qpMin, (double)m_param->rc.qpMax, qpBase);
                curEncData.m_rowStat[row].rowQp = qpBase;
                curEncData.m_rowStat[row].rowQpScale = x265_qp2qScale(qpBase);
                if (curRow.reEncode < 0)
                {
                    x265_log(m_param, X265_LOG_DEBUG, "POC %d row %d - encode restart required for VBV, to %.2f from %.2f\n",
                             m_frame[layer]->m_poc, row, qpBase, curEncData.m_cuStat[cuAddr].baseQp);

                    // prevent the WaveFront::findJob() method from providing new jobs
                    m_vbvResetTriggerRow[curRow.sliceId] = row;
                    m_bAllRowsStop[curRow.sliceId] = true;

                    for (uint32_t r = m_sliceBaseRow[sliceId + 1] - 1; r >= row; r--)
                    {
                        CTURow& stopRow = m_rows[r];

                        if (r != row)
                        {
                            /* if row was active (ready to be run) clear active bit and bitmap bit for this row */
                            stopRow.lock.acquire();
                            while (stopRow.active)
                            {
                                if (dequeueRow(m_row_to_idx[r] * 2))
                                    stopRow.active = false;
                                else
                                {
                                    /* we must release the row lock to allow the thread to exit */
                                    stopRow.lock.release();
                                    GIVE_UP_TIME();
                                    stopRow.lock.acquire();
                                }
                            }
                            stopRow.lock.release();

                            bool bRowBusy = true;
                            do
                            {
                                stopRow.lock.acquire();
                                bRowBusy = stopRow.busy;
                                stopRow.lock.release();

                                if (bRowBusy)
                                {
                                    GIVE_UP_TIME();
                                }
                            }
                            while (bRowBusy);
                        }

                        m_outStreams[r].resetBits();
                        stopRow.completed = 0;
                        memset(&stopRow.rowStats, 0, sizeof(stopRow.rowStats));
                        curEncData.m_rowStat[r].numEncodedCUs = 0;
                        curEncData.m_rowStat[r].encodedBits = 0;
                        curEncData.m_rowStat[r].rowSatd = 0;
                        curEncData.m_rowStat[r].rowIntraSatd = 0;
                        curEncData.m_rowStat[r].sumQpRc = 0;
                        curEncData.m_rowStat[r].sumQpAq = 0;
                    }

                    m_bAllRowsStop[curRow.sliceId] = false;
                }
            }
        }

        if (m_param->bEnableWavefront && curRow.completed >= 2 && !bLastRowInSlice &&
            (!m_bAllRowsStop[curRow.sliceId] || intRow + 1 < m_vbvResetTriggerRow[curRow.sliceId]))
        {
            /* activate next row */
            ScopedLock below(m_rows[row + 1].lock);

            if (m_rows[row + 1].active == false &&
                m_rows[row + 1].completed + 2 <= curRow.completed)
            {
                m_rows[row + 1].active = true;
                enqueueRowEncoder(m_row_to_idx[row + 1]);
                tryWakeOne(); /* wake up a sleeping thread or set the help wanted flag */
            }
        }

        ScopedLock self(curRow.lock);
        if ((m_bAllRowsStop[curRow.sliceId] && intRow > m_vbvResetTriggerRow[curRow.sliceId]) ||
            (!bFirstRowInSlice && ((curRow.completed < numCols - 1) || (m_rows[row - 1].completed < numCols)) && m_rows[row - 1].completed < curRow.completed + 2))
        {
            curRow.active = false;
            curRow.busy = false;
            ATOMIC_INC(&m_countRowBlocks);
            return;
        }
    }

    /* this row of CTUs has been compressed */
    if (m_param->bEnableWavefront && m_param->rc.bEnableConstVbv)
    {
        if (bLastRowInSlice)       
        {
            for (uint32_t r = m_sliceBaseRow[sliceId]; r < m_sliceBaseRow[sliceId + 1]; r++)
            {
                for (uint32_t c = curEncData.m_rowStat[r].numEncodedCUs + 1; c < numCols * (r + 1); c++)
                {
                    curEncData.m_rowStat[r].rowSatd.fetchAdd(curEncData.m_cuStat[c].vbvCost);
                    curEncData.m_rowStat[r].rowIntraSatd.fetchAdd(curEncData.m_cuStat[c].intraVbvCost);
                    curEncData.m_rowStat[r].encodedBits.fetchAdd(curEncData.m_cuStat[c].totalBits);
                    curEncData.m_rowStat[r].sumQpRc += curEncData.m_cuStat[c].baseQp;
                    curEncData.m_rowStat[r].numEncodedCUs = c;
                }
            }
        }
    }

    /* If encoding with ABR, update update bits and complexity in rate control
     * after a number of rows so the next frame's rateControlStart has more
     * accurate data for estimation. At the start of the encode we update stats
     * after half the frame is encoded, but after this initial period we update
     * after refLagRows (the number of rows reference frames must have completed
     * before referencees may begin encoding) */
    if ((!layer) && (m_param->rc.rateControlMode == X265_RC_ABR || bIsVbv))
    {
        uint32_t rowCount = 0;
        uint32_t maxRows = m_sliceBaseRow[sliceId + 1] - m_sliceBaseRow[sliceId];

        if (!m_rce.encodeOrder)
            rowCount = maxRows - 1; 
        else if ((uint32_t)m_rce.encodeOrder <= 2 * (m_param->fpsNum / m_param->fpsDenom))
            rowCount = X265_MIN((maxRows + 1) / 2, maxRows - 1);
        else
            rowCount = X265_MIN(m_refLagRows / m_param->maxSlices, maxRows - 1);

        if (rowInSlice == rowCount)
        {
            m_rowSliceTotalBits[sliceId] = 0;
            if (bIsVbv && !(m_param->rc.bEnableConstVbv && m_param->bEnableWavefront))
            {
                for (uint32_t i = m_sliceBaseRow[sliceId]; i < rowCount + m_sliceBaseRow[sliceId]; i++)
                    m_rowSliceTotalBits[sliceId] += curEncData.m_rowStat[i].encodedBits;
            }
            else
            {
                uint32_t startAddr = m_sliceBaseRow[sliceId] * numCols;
                uint32_t finishAddr = startAddr + rowCount * numCols;
                
                for (uint32_t cuAddr = startAddr; cuAddr < finishAddr; cuAddr++)
                    m_rowSliceTotalBits[sliceId] += curEncData.m_cuStat[cuAddr].totalBits;
            }

            if (ATOMIC_INC(&m_sliceCnt) == (int)m_param->maxSlices)
            {
                m_rce.rowTotalBits = 0;
                for (uint32_t i = 0; i < m_param->maxSlices; i++)
                    m_rce.rowTotalBits += m_rowSliceTotalBits[i];
                m_top->m_rateControl->rateControlUpdateStats(&m_rce);
            }
        }
    }

    /* flush row bitstream (if WPP and no SAO) or flush frame if no WPP and no SAO */
    /* end_of_sub_stream_one_bit / end_of_slice_segment_flag */
       if (!slice->m_bUseSao && (m_param->bEnableWavefront || bLastRowInSlice))
               rowCoder.finishSlice();


    /* Processing left Deblock block with current threading */
    if ((m_param->bEnableLoopFilter | slice->m_bUseSao) & (rowInSlice >= 2))
    {
        /* Check conditional to start previous row process with current threading */
        if (m_frameFilter.m_parallelFilter[row - 2].m_lastDeblocked.get() == (int)numCols)
        {
            /* stop threading on current row and restart it */
            m_frameFilter.m_parallelFilter[row - 1].m_allowedCol.set(numCols);
            m_frameFilter.m_parallelFilter[row - 1].processTasks(-1);
        }
    }

    /* trigger row-wise loop filters */
    if (m_param->bEnableWavefront)
    {
        if (rowInSlice >= m_filterRowDelay)
        {
            enableRowFilter(m_row_to_idx[row - m_filterRowDelay]);

            /* NOTE: Activate filter if first row (row 0) */
            if (rowInSlice == m_filterRowDelay)
                enqueueRowFilter(m_row_to_idx[row - m_filterRowDelay]);
            tryWakeOne();
        }

        if (bLastRowInSlice)
        {
            for (uint32_t i = endRowInSlicePlus1 - m_filterRowDelay; i < endRowInSlicePlus1; i++)
            {
                enableRowFilter(m_row_to_idx[i]);
            }
            tryWakeOne();
        }

        // handle specially case - single row slice
        if  (bFirstRowInSlice & bLastRowInSlice)
        {
            enqueueRowFilter(m_row_to_idx[row]);
            tryWakeOne();
        }
    }

    curRow.busy = false;

    // CHECK_ME: Does it always FALSE condition?
    if (ATOMIC_INC(&m_completionCount) == 2 * (int)m_numRows)
        m_completionEvent.trigger();
}

void FrameEncoder::collectDynDataRow(CUData& ctu, FrameStats* rowStats)
{
    for (uint32_t i = 0; i < X265_REFINE_INTER_LEVELS; i++)
    {
        for (uint32_t depth = 0; depth < m_param->maxCUDepth; depth++)
        {
            int offset = (depth * X265_REFINE_INTER_LEVELS) + i;
            if (ctu.m_collectCUCount[offset])
            {
                rowStats->rowVarDyn[offset] += ctu.m_collectCUVariance[offset];
                rowStats->rowRdDyn[offset] += ctu.m_collectCURd[offset];
                rowStats->rowCntDyn[offset] += ctu.m_collectCUCount[offset];
            }
        }
    }
}

void FrameEncoder::collectDynDataFrame(int layer)
{
    for (uint32_t row = 0; row < m_numRows; row++)
    {
        for (uint32_t refLevel = 0; refLevel < X265_REFINE_INTER_LEVELS; refLevel++)
        {
            for (uint32_t depth = 0; depth < m_param->maxCUDepth; depth++)
            {
                int offset = (depth * X265_REFINE_INTER_LEVELS) + refLevel;
                int curFrameIndex = m_frame[layer]->m_encodeOrder - m_top->m_startPoint;
                int index = (curFrameIndex * X265_REFINE_INTER_LEVELS * m_param->maxCUDepth) + offset;
                if (m_rows[row].rowStats.rowCntDyn[offset])
                {
                    m_top->m_variance[index] += m_rows[row].rowStats.rowVarDyn[offset];
                    m_top->m_rdCost[index] += m_rows[row].rowStats.rowRdDyn[offset];
                    m_top->m_trainingCount[index] += m_rows[row].rowStats.rowCntDyn[offset];
                }
            }
        }
    }
}

void FrameEncoder::computeAvgTrainingData(int layer)
{
    if (m_frame[layer]->m_lowres.bScenecut || m_frame[layer]->m_lowres.bKeyframe)
    {
        m_top->m_startPoint = m_frame[layer]->m_encodeOrder;
        int size = (m_param->keyframeMax + m_param->lookaheadDepth) * m_param->maxCUDepth * X265_REFINE_INTER_LEVELS;
        memset(m_top->m_variance, 0, size * sizeof(uint64_t));
        memset(m_top->m_rdCost, 0, size * sizeof(uint64_t));
        memset(m_top->m_trainingCount, 0, size * sizeof(uint32_t));
    }
    if (m_frame[layer]->m_encodeOrder - m_top->m_startPoint < 2 * m_param->frameNumThreads)
        m_frame[layer]->m_classifyFrame = false;
    else
        m_frame[layer]->m_classifyFrame = true;

    int size = m_param->maxCUDepth * X265_REFINE_INTER_LEVELS;
    memset(m_frame[layer]->m_classifyRd, 0, size * sizeof(uint64_t));
    memset(m_frame[layer]->m_classifyVariance, 0, size * sizeof(uint64_t));
    memset(m_frame[layer]->m_classifyCount, 0, size * sizeof(uint32_t));
    if (m_frame[layer]->m_classifyFrame)
    {
        uint32_t limit = m_frame[layer]->m_encodeOrder - m_top->m_startPoint - m_param->frameNumThreads;
        for (uint32_t i = 1; i < limit; i++)
        {
            for (uint32_t j = 0; j < X265_REFINE_INTER_LEVELS; j++)
            {
                for (uint32_t depth = 0; depth < m_param->maxCUDepth; depth++)
                {
                    int offset = (depth * X265_REFINE_INTER_LEVELS) + j;
                    int index = (i* X265_REFINE_INTER_LEVELS * m_param->maxCUDepth) + offset;
                    if (m_top->m_trainingCount[index])
                    {
                        m_frame[layer]->m_classifyRd[offset] += m_top->m_rdCost[index] / m_top->m_trainingCount[index];
                        m_frame[layer]->m_classifyVariance[offset] += m_top->m_variance[index] / m_top->m_trainingCount[index];
                        m_frame[layer]->m_classifyCount[offset] += m_top->m_trainingCount[index];
                    }
                }
            }
        }
        /* Calculates the average feature values of historic frames that are being considered for the current frame */
        int historyCount = m_frame[layer]->m_encodeOrder - m_param->frameNumThreads - m_top->m_startPoint - 1;
        if (historyCount)
        {
            for (uint32_t j = 0; j < X265_REFINE_INTER_LEVELS; j++)
            {
                for (uint32_t depth = 0; depth < m_param->maxCUDepth; depth++)
                {
                    int offset = (depth * X265_REFINE_INTER_LEVELS) + j;
                    m_frame[layer]->m_classifyRd[offset] /= historyCount;
                    m_frame[layer]->m_classifyVariance[offset] /= historyCount;
                }
            }
        }
    }
}

/* collect statistics about CU coding decisions, return total QP */
int FrameEncoder::collectCTUStatistics(const CUData& ctu, FrameStats* log)
{
    int totQP = 0;
    uint32_t depth = 0;
    for (uint32_t absPartIdx = 0; absPartIdx < ctu.m_numPartitions; absPartIdx += ctu.m_numPartitions >> (depth * 2))
    {
        depth = ctu.m_cuDepth[absPartIdx];
        totQP += ctu.m_qp[absPartIdx] * (ctu.m_numPartitions >> (depth * 2));
    }

    if (m_param->csvLogLevel >= 1 || m_param->rc.bStatWrite)
    {
        if (ctu.m_slice->m_sliceType == I_SLICE)
        {
            depth = 0;
            for (uint32_t absPartIdx = 0; absPartIdx < ctu.m_numPartitions; absPartIdx += ctu.m_numPartitions >> (depth * 2))
            {
                depth = ctu.m_cuDepth[absPartIdx];

                log->totalCu++;
                log->cntIntra[depth]++;

                if (ctu.m_predMode[absPartIdx] == MODE_NONE)
                {
                    log->totalCu--;
                    log->cntIntra[depth]--;
                }
                else if (ctu.m_partSize[absPartIdx] != SIZE_2Nx2N)
                {
                    /* TODO: log intra modes at absPartIdx +0 to +3 */
                    X265_CHECK(ctu.m_log2CUSize[absPartIdx] == 3 && ctu.m_slice->m_sps->quadtreeTULog2MinSize < 3, "Intra NxN found at improbable depth\n");
                    log->cntIntraNxN++;
                    log->cntIntra[depth]--;
                }
                else if (ctu.m_lumaIntraDir[absPartIdx] > 1)
                    log->cuIntraDistribution[depth][ANGULAR_MODE_ID]++;
                else
                    log->cuIntraDistribution[depth][ctu.m_lumaIntraDir[absPartIdx]]++;
            }
        }
        else
        {
            depth = 0;
            for (uint32_t absPartIdx = 0; absPartIdx < ctu.m_numPartitions; absPartIdx += ctu.m_numPartitions >> (depth * 2))
            {
                depth = ctu.m_cuDepth[absPartIdx];

                log->totalCu++;

                if (ctu.m_predMode[absPartIdx] == MODE_NONE)
                    log->totalCu--;
                else if (ctu.isSkipped(absPartIdx))
                {
                    if (ctu.m_mergeFlag[0])
                        log->cntMergeCu[depth]++;
                    else
                        log->cntSkipCu[depth]++;
                }
                else if (ctu.isInter(absPartIdx))
                {
                    log->cntInter[depth]++;

                    if (ctu.m_partSize[absPartIdx] < AMP_ID)
                        log->cuInterDistribution[depth][ctu.m_partSize[absPartIdx]]++;
                    else
                        log->cuInterDistribution[depth][AMP_ID]++;
                }
                else if (ctu.isIntra(absPartIdx))
                {
                    log->cntIntra[depth]++;

                    if (ctu.m_partSize[absPartIdx] != SIZE_2Nx2N)
                    {
                        X265_CHECK(ctu.m_log2CUSize[absPartIdx] == 3 && ctu.m_slice->m_sps->quadtreeTULog2MinSize < 3, "Intra NxN found at improbable depth\n");
                        log->cntIntraNxN++;
                        log->cntIntra[depth]--;
                        /* TODO: log intra modes at absPartIdx +0 to +3 */
                    }
                    else if (ctu.m_lumaIntraDir[absPartIdx] > 1)
                        log->cuIntraDistribution[depth][ANGULAR_MODE_ID]++;
                    else
                        log->cuIntraDistribution[depth][ctu.m_lumaIntraDir[absPartIdx]]++;
                }
            }
        }
    }

    return totQP;
}

/* DCT-domain noise reduction / adaptive deadzone from libavcodec */
void FrameEncoder::noiseReductionUpdate()
{
    static const uint32_t maxBlocksPerTrSize[4] = {1 << 18, 1 << 16, 1 << 14, 1 << 12};

    for (int cat = 0; cat < MAX_NUM_TR_CATEGORIES; cat++)
    {
        int trSize = cat & 3;
        int coefCount = 1 << ((trSize + 2) * 2);

        if (m_nr->nrCount[cat] > maxBlocksPerTrSize[trSize])
        {
            for (int i = 0; i < coefCount; i++)
                m_nr->nrResidualSum[cat][i] >>= 1;
            m_nr->nrCount[cat] >>= 1;
        }

        int nrStrength = cat < 8 ? m_param->noiseReductionIntra : m_param->noiseReductionInter;
        uint64_t scaledCount = (uint64_t)nrStrength * m_nr->nrCount[cat];

        for (int i = 0; i < coefCount; i++)
        {
            uint64_t value = scaledCount + m_nr->nrResidualSum[cat][i] / 2;
            uint64_t denom = m_nr->nrResidualSum[cat][i] + 1;
            m_nr->nrOffsetDenoise[cat][i] = (uint16_t)(value / denom);
        }

        // Don't denoise DC coefficients
        m_nr->nrOffsetDenoise[cat][0] = 0;
    }
}

void FrameEncoder::readModel(FilmGrainCharacteristics* m_filmGrain, FILE* filmgrain)
{
    char const* errorMessage = "Error reading FilmGrain characteristics\n";
    FilmGrain m_fg;
    x265_fread((char* )&m_fg, sizeof(bool) * 3 + sizeof(uint8_t), 1, filmgrain, errorMessage);
    m_filmGrain->m_filmGrainCharacteristicsCancelFlag = m_fg.m_filmGrainCharacteristicsCancelFlag;
    m_filmGrain->m_filmGrainCharacteristicsPersistenceFlag = m_fg.m_filmGrainCharacteristicsPersistenceFlag;
    m_filmGrain->m_filmGrainModelId = m_fg.m_filmGrainModelId;
    m_filmGrain->m_separateColourDescriptionPresentFlag = m_fg.m_separateColourDescriptionPresentFlag;
    if (m_filmGrain->m_separateColourDescriptionPresentFlag)
    {
        ColourDescription m_clr;
        x265_fread((char* )&m_clr, sizeof(bool) + sizeof(uint8_t) * 5, 1, filmgrain, errorMessage);
        m_filmGrain->m_filmGrainBitDepthLumaMinus8 = m_clr.m_filmGrainBitDepthLumaMinus8;
        m_filmGrain->m_filmGrainBitDepthChromaMinus8 = m_clr.m_filmGrainBitDepthChromaMinus8;
        m_filmGrain->m_filmGrainFullRangeFlag = m_clr.m_filmGrainFullRangeFlag;
        m_filmGrain->m_filmGrainColourPrimaries = m_clr.m_filmGrainColourPrimaries;
        m_filmGrain->m_filmGrainTransferCharacteristics = m_clr.m_filmGrainTransferCharacteristics;
        m_filmGrain->m_filmGrainMatrixCoeffs = m_clr.m_filmGrainMatrixCoeffs;
    }
    FGPresent m_present;
    x265_fread((char* )&m_present, sizeof(bool) * 3 + sizeof(uint8_t) * 2, 1, filmgrain, errorMessage);
    m_filmGrain->m_blendingModeId = m_present.m_blendingModeId;
    m_filmGrain->m_log2ScaleFactor = m_present.m_log2ScaleFactor;
    m_filmGrain->m_compModel[0].bPresentFlag = m_present.m_presentFlag[0];
    m_filmGrain->m_compModel[1].bPresentFlag = m_present.m_presentFlag[1];
    m_filmGrain->m_compModel[2].bPresentFlag = m_present.m_presentFlag[2];
    for (int i = 0; i < MAX_NUM_COMPONENT; i++)
    {
        if (m_filmGrain->m_compModel[i].bPresentFlag)
        {
            x265_fread((char* )(&m_filmGrain->m_compModel[i].m_filmGrainNumIntensityIntervalMinus1), sizeof(uint8_t), 1, filmgrain, errorMessage);
            x265_fread((char* )(&m_filmGrain->m_compModel[i].numModelValues), sizeof(uint8_t), 1, filmgrain, errorMessage);
            m_filmGrain->m_compModel[i].intensityValues = (FilmGrainCharacteristics::CompModelIntensityValues* ) malloc(sizeof(FilmGrainCharacteristics::CompModelIntensityValues) * (m_filmGrain->m_compModel[i].m_filmGrainNumIntensityIntervalMinus1+1)) ;
            for (int j = 0; j <= m_filmGrain->m_compModel[i].m_filmGrainNumIntensityIntervalMinus1; j++)
            {
                x265_fread((char* )(&m_filmGrain->m_compModel[i].intensityValues[j].intensityIntervalLowerBound), sizeof(uint8_t), 1, filmgrain, errorMessage);
                x265_fread((char* )(&m_filmGrain->m_compModel[i].intensityValues[j].intensityIntervalUpperBound), sizeof(uint8_t), 1, filmgrain, errorMessage);
                m_filmGrain->m_compModel[i].intensityValues[j].compModelValue = (int* ) malloc(sizeof(int) * (m_filmGrain->m_compModel[i].numModelValues));
                for (int k = 0; k < m_filmGrain->m_compModel[i].numModelValues; k++)
                {
                    x265_fread((char* )(&m_filmGrain->m_compModel[i].intensityValues[j].compModelValue[k]), sizeof(int), 1, filmgrain, errorMessage);
                }
            }
        }
    }
}

void compute_film_grain_resolution(int width, int height,
                                   int& apply_units_resolution_log2,
                                   int& apply_horz_resolution,
                                   int& apply_vert_resolution)
{
    unsigned long log2_width, log2_height;
    BSF(log2_width, (unsigned long) width);
    BSF(log2_height, (unsigned long) height);

    int log2 = (log2_width < log2_height) ? log2_width : log2_height;
    apply_units_resolution_log2 = log2;

    int unit = 1 << log2;
    apply_horz_resolution = width / unit;
    apply_vert_resolution = height / unit;

    return;
}

void FrameEncoder::readAomModel(AomFilmGrainCharacteristics* m_aomFilmGrain, FILE* Aomfilmgrain)
{
    char const* errorMessage = "Error reading Aom FilmGrain characteristics\n";
    AomFilmGrain m_afg;
    m_afg.m_chroma_scaling_from_luma = 0;
    int bitCount = 0;
    bitCount += 4; // payload_less_than_4byte_flag(1) + film_grain_param_set_idx(3)
    x265_fread((char*)&m_aomFilmGrain->m_apply_grain, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
    bitCount++;
    x265_fread((char*)&m_aomFilmGrain->m_grain_seed, sizeof(uint16_t), 1, Aomfilmgrain, errorMessage);
    bitCount+=16;
    x265_fread((char*)&m_aomFilmGrain->m_update_grain, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
    bitCount++;
    x265_fread((char*)&m_aomFilmGrain->m_num_y_points, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
    bitCount+=4;

    if (m_aomFilmGrain->m_num_y_points)
    {
        m_aomFilmGrain->point_y_value_increment_bits = 8;
        bitCount += 3;
        m_aomFilmGrain->point_y_scaling_bits = 8;
        bitCount += 2;
        for (int i = 0; i < m_aomFilmGrain->m_num_y_points; i++)
        {
            for (int j = 0; j < 2; j++)
            {
                x265_fread((char*)&m_aomFilmGrain->m_scaling_points_y[i][j], sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
                bitCount+=8;
            }
        }
    }
    x265_fread((char*)&m_aomFilmGrain->m_num_cb_points, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
    bitCount+=4;
    if (m_aomFilmGrain->m_num_cb_points)
    {
        m_aomFilmGrain->point_cb_value_increment_bits = 8;
        bitCount += 3;
        m_aomFilmGrain->point_cb_scaling_bits = 8;
        bitCount += 2;
        m_aomFilmGrain->cb_scaling_offset = 0;
        bitCount += 8;
        for (int i = 0; i < m_aomFilmGrain->m_num_cb_points; i++)
        {
            for (int j = 0; j < 2; j++)
            {
                x265_fread((char*)&m_aomFilmGrain->m_scaling_points_cb[i][j], sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
                bitCount+=8;
            }
        }
    }
    x265_fread((char*)&m_aomFilmGrain->m_num_cr_points, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
    bitCount+=4;
    if (m_aomFilmGrain->m_num_cr_points)
    {
        m_aomFilmGrain->point_cr_value_increment_bits = 8;
        bitCount += 3;
        m_aomFilmGrain->point_cr_scaling_bits = 8;
        bitCount += 2;
        m_aomFilmGrain->cr_scaling_offset = 0;
        bitCount += 8;
        for (int i = 0; i < m_aomFilmGrain->m_num_cr_points; i++)
        {
            for (int j = 0; j < 2; j++)
            {
                x265_fread((char*)&m_aomFilmGrain->m_scaling_points_cr[i][j], sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
                bitCount+=8;
            }
        }
    }
    x265_fread((char*)&m_aomFilmGrain->m_scaling_shift, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
    bitCount+=2;
    x265_fread((char*)&m_aomFilmGrain->m_ar_coeff_lag, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
    bitCount+=2;
    if (m_aomFilmGrain->m_num_y_points)
    {
        bitCount += 2;
        for (int i = 0; i < 24; i++)
        {
            x265_fread((char*)&m_aomFilmGrain->m_ar_coeffs_y[i], sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
            bitCount+=8;
        }
    }
    if (m_aomFilmGrain->m_num_cb_points || m_afg.m_chroma_scaling_from_luma)
    {
        bitCount += 2;
        for (int i = 0; i < 25; i++)
        {
            x265_fread((char*)&m_aomFilmGrain->m_ar_coeffs_cb[i], sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
            bitCount+=8;
        }
    }
    if (m_aomFilmGrain->m_num_cr_points || m_afg.m_chroma_scaling_from_luma)
    {
        bitCount += 2;
        for (int i = 0; i < 25; i++)
        {
            x265_fread((char*)&m_aomFilmGrain->m_ar_coeffs_cr[i], sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
            bitCount+=8;
        }
    }
    x265_fread((char*)&m_aomFilmGrain->m_ar_coeff_shift, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
    bitCount+=2;
    x265_fread((char*)&m_aomFilmGrain->m_grain_scale_shift, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
    bitCount+=2;
    if (m_aomFilmGrain->m_num_cb_points)
    {
        x265_fread((char*)&m_aomFilmGrain->m_cb_mult, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
        bitCount += 8;
        x265_fread((char*)&m_aomFilmGrain->m_cb_luma_mult, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
        bitCount += 8;
        x265_fread((char*)&m_aomFilmGrain->m_cb_offset, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
        bitCount += 9;
    }
    if (m_aomFilmGrain->m_num_cr_points)
    {
        x265_fread((char*)&m_aomFilmGrain->m_cr_mult, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
        bitCount += 8;
        x265_fread((char*)&m_aomFilmGrain->m_cr_luma_mult, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
        bitCount += 8;
        x265_fread((char*)&m_aomFilmGrain->m_cr_offset, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
        bitCount += 9;
    }
    x265_fread((char*)&m_aomFilmGrain->m_overlap_flag, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
    bitCount++;
    x265_fread((char*)&m_aomFilmGrain->m_clip_to_restricted_range, sizeof(int32_t), 1, Aomfilmgrain, errorMessage);
    bitCount++;

    m_aomFilmGrain->luma_only_flag = m_aomFilmGrain->m_num_cb_points == 0 && m_aomFilmGrain->m_num_cr_points == 0;
    bitCount++;
    m_aomFilmGrain->subsamplingX = CHROMA_H_SHIFT(m_param->internalCsp);
    m_aomFilmGrain->subsamplingY = CHROMA_V_SHIFT(m_param->internalCsp);
    if (!m_aomFilmGrain->luma_only_flag)
        bitCount += 2; // subsampling_x(1) + subsampling_y(1)
    compute_film_grain_resolution(m_param->sourceWidth, m_param->sourceHeight, m_aomFilmGrain->units_resolution_log2,
        m_aomFilmGrain->horz_resolution, m_aomFilmGrain->vert_resolution);
    bitCount += 28; // apply_units_resolution_log2(4) + apply_horz_resolution(12) + apply_vert_resolution(12)
    m_aomFilmGrain->predict_scaling_flag = 0;
    bitCount++;
    m_aomFilmGrain->predict_y_scaling_flag = 0;
    m_aomFilmGrain->predict_cb_scaling_flag = 0;
    m_aomFilmGrain->predict_cr_scaling_flag = 0;
    m_aomFilmGrain->m_bitDepth = m_param->internalBitDepth;
    bitCount++; // videosingnaltypepresentflag
    if (m_frame[0]->m_encData->m_slice->m_sps->vuiParameters.videoSignalTypePresentFlag) bitCount += 4; // bit_depth_minus8(3) + cicp_info_present_flag(1)
    if (m_frame[0]->m_encData->m_slice->m_sps->vuiParameters.colourDescriptionPresentFlag) bitCount += 25; // colourPrimaries(8) + transferCharacteristics(8) + matrixCoefficients(8)+ videoFullRangeFlag(1)
    if (!m_aomFilmGrain->luma_only_flag) {
        m_aomFilmGrain->m_chroma_scaling_from_luma = 0;
        bitCount++;
    }

    m_aomFilmGrain->payload_size = (bitCount + 8 - 1) / 8;
    m_aomFilmGrain->payload_bits = m_aomFilmGrain->payload_size < 4 ? 2 : 8;
    bitCount += m_aomFilmGrain->payload_bits;
    m_aomFilmGrain->payload_size = (bitCount + 8 - 1) / 8;
}

#if ENABLE_LIBVMAF
void FrameEncoder::vmafFrameLevelScore()
{
    PicYuv *fenc = m_frame[0]->m_fencPic;
    PicYuv *recon = m_frame[0]->m_reconPic[0];

    x265_vmaf_framedata *vmafframedata = (x265_vmaf_framedata*)x265_malloc(sizeof(x265_vmaf_framedata));
    if (!vmafframedata)
    {
        x265_log(NULL, X265_LOG_ERROR, "vmaf frame data alloc failed\n");
    }

    vmafframedata->height = fenc->m_picHeight;
    vmafframedata->width = fenc->m_picWidth;
    vmafframedata->frame_set = 0;
    vmafframedata->internalBitDepth = m_param->internalBitDepth;
    vmafframedata->reference_frame = fenc;
    vmafframedata->distorted_frame = recon;
    fenc->m_vmafScore = x265_calculate_vmaf_framelevelscore(m_param,vmafframedata);

    if (vmafframedata)
    x265_free(vmafframedata);
}
#endif

Frame** FrameEncoder::getEncodedPicture(NALList& output)
{
    if (m_frame[0] && (m_param->numLayers <= 1 || (MAX_LAYERS > 1 && m_frame[1])))
    {
        /* block here until worker thread completes */
        X265_FLOW_LOG("PassEncoder",
                      "[CHANNEL channel=frame-done encoder=%d frame=%d] WAIT_BEGIN reason=frame-in-progress object=Frame@%p owner=Encoder",
                      m_jpId, m_frame[0]->m_poc, m_frame[0]);
        m_done.wait();
        X265_FLOW_LOG("PassEncoder",
                      "[CHANNEL channel=frame-done encoder=%d frame=%d] WAIT_END reason=frame-complete object=Frame@%p owner=Encoder",
                      m_jpId, m_frame[0]->m_poc, m_frame[0]);

        for (int i = 0; i < m_param->numLayers; i++)
        {
            m_retFrameBuffer[i] = m_frame[i];
            m_frame[i] = NULL;
            m_prevOutputTime[i] = x265_mdate();
        }
        output.takeContents(m_nalList);
        X265_FLOW_LOG("PassEncoder",
                      "[CHANNEL channel=nal-output encoder=%d frame=%d] RECEIVE nal_count=%u buffer=%p bytes=%u owner_before=FrameEncoder-%d owner_after=Encoder",
                      m_jpId, m_retFrameBuffer[0]->m_poc, output.m_numNal,
                      output.m_buffer, output.m_occupancy, m_jpId);
        return m_retFrameBuffer;
    }

    return NULL;
}
}
