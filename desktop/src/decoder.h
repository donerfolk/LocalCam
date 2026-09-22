#pragma once
// H.264 (Annex B) to NV12 with Windows' built-in Media Foundation decoder, in low-latency mode.
#include <mftransform.h>
#include <wrl/client.h>

#include <functional>

#include "convert.h"

class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool ok() const { return mft_ != nullptr; }
    HRESULT lastError() const { return error_; }

    // Decodes one access unit and calls onFrame for each picture it yields. False on a decoder error.
    bool decode(const uint8_t* data, size_t size, const std::function<void(const Nv12&)>& onFrame);

private:
    bool setOutputType();

    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    Microsoft::WRL::ComPtr<IMFSample> out_;
    DWORD outSize_ = 0;
    int width_ = 0, height_ = 0, bufferHeight_ = 0, stride_ = 0;
    bool bt601_ = false, fullRange_ = false;
    LONGLONG time_ = 0;
    HRESULT error_ = S_OK;
    bool comInit_ = false;
};
