#include "decoder.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

Decoder::Decoder() {
    comInit_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ComPtr<IMFTransform> t;
    if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t)))) return;
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(t->GetAttributes(&attrs))) attrs->SetUINT32(MF_LOW_LATENCY, TRUE);  // no frame reordering delay

    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (FAILED(t->SetInputType(0, in.Get(), 0))) return;
    mft_ = t;
    if (!setOutputType()) {
        mft_.Reset();
        return;
    }
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

Decoder::~Decoder() {
    out_.Reset();
    mft_.Reset();
    if (comInit_) CoUninitialize();
}

bool Decoder::setOutputType() {
    for (DWORD i = 0;; i++) {
        ComPtr<IMFMediaType> t;
        if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) return false;
        GUID subtype{};
        t->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype != MFVideoFormat_NV12) continue;
        if (FAILED(mft_->SetOutputType(0, t.Get(), 0))) return false;

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);  // padded to 16, e.g. 1920x1088
        width_ = int(w);
        height_ = bufferHeight_ = int(h);
        MFVideoArea area{};
        if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof area, nullptr))) {
            width_ = area.Area.cx;
            height_ = area.Area.cy;
        }
        stride_ = int(MFGetAttributeUINT32(t.Get(), MF_MT_DEFAULT_STRIDE, w));
        bt601_ = MFGetAttributeUINT32(t.Get(), MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709) == MFVideoTransferMatrix_BT601;
        fullRange_ = MFGetAttributeUINT32(t.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235) == MFNominalRange_0_255;
        out_.Reset();
        return true;
    }
}

bool Decoder::decode(const uint8_t* data, size_t size, const std::function<void(const Nv12&)>& onFrame) {
    if (!mft_) return false;
    ComPtr<IMFMediaBuffer> buf;
    ComPtr<IMFSample> sample;
    BYTE* p = nullptr;
    if (FAILED(MFCreateMemoryBuffer(DWORD(size), &buf)) || FAILED(buf->Lock(&p, nullptr, nullptr))) return false;
    std::memcpy(p, data, size);
    buf->Unlock();
    buf->SetCurrentLength(DWORD(size));
    MFCreateSample(&sample);
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(time_);
    sample->SetSampleDuration(333333);
    time_ += 333333;
    if (FAILED(error_ = mft_->ProcessInput(0, sample.Get(), 0))) return false;

    for (;;) {
        MFT_OUTPUT_STREAM_INFO info{};
        mft_->GetOutputStreamInfo(0, &info);
        const bool mftAllocates = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        if (!mftAllocates && (!out_ || outSize_ != info.cbSize)) {
            ComPtr<IMFMediaBuffer> ob;
            if (FAILED(MFCreateMemoryBuffer(info.cbSize, &ob))) return false;
            out_.Reset();
            MFCreateSample(&out_);
            out_->AddBuffer(ob.Get());
            outSize_ = info.cbSize;
        }
        if (!mftAllocates) {  // reused buffer: the decoder refuses one that still holds the last frame
            ComPtr<IMFMediaBuffer> ob;
            out_->GetBufferByIndex(0, &ob);
            ob->SetCurrentLength(0);
        }
        MFT_OUTPUT_DATA_BUFFER od{0, mftAllocates ? nullptr : out_.Get(), 0, nullptr};
        DWORD status = 0;
        const HRESULT hr = mft_->ProcessOutput(0, 1, &od, &status);
        if (od.pEvents) od.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {  // first frame or new resolution
            if (!setOutputType()) return false;
            continue;
        }
        if (FAILED(hr)) {
            error_ = hr;
            return false;
        }

        ComPtr<IMFSample> got;
        got.Attach(od.pSample);
        if (!mftAllocates) got->AddRef();  // od.pSample is our out_, which we keep
        ComPtr<IMFMediaBuffer> mb;
        if (FAILED(got->GetBufferByIndex(0, &mb))) return false;
        ComPtr<IMF2DBuffer> b2;
        BYTE* base = nullptr;
        LONG pitch = stride_;
        const bool is2d = SUCCEEDED(mb.As(&b2)) && SUCCEEDED(b2->Lock2D(&base, &pitch));
        if (!is2d && FAILED(mb->Lock(&base, nullptr, nullptr))) return false;
        onFrame({base, base + size_t(pitch) * bufferHeight_, int(pitch), width_, height_, bt601_, fullRange_});
        if (is2d) b2->Unlock2D();
        else mb->Unlock();
    }
}
