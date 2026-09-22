// Self-checks for the pixel mapping, the H.264 decoder, the WebSocket handshake and the server's limits.
// Run: localcam_tests (or ctest).
#include <winsock2.h>
#include <ws2tcpip.h>

#include <mfapi.h>
#include <ncrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "convert.h"
#include "decoder.h"
#include "server.h"
#include "softcam.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

// A w x h full-range grayscale picture that is black except one white pixel.
struct Picture {
    int w, h;
    std::vector<uint8_t> data;
    Picture(int w, int h, int wx, int wy) : w(w), h(h), data(size_t(w) * h * 3 / 2, 128) {
        std::fill(data.begin(), data.begin() + w * h, 0);
        data[size_t(wy) * w + wx] = 255;
    }
    Nv12 nv12() const { return {data.data(), data.data() + w * h, w, w, h, false, true}; }
};

static bool white(const std::vector<uint8_t>& bgr, int w, int x, int y) { return bgr[(size_t(y) * w + x) * 3] > 200; }

static int whiteCount(const std::vector<uint8_t>& bgr) {
    int n = 0;
    for (size_t i = 0; i < bgr.size(); i += 3) n += bgr[i] > 200;
    return n;
}

// A certificate on a throwaway key, so the test server never touches LocalCam's own key or certificate.
static const wchar_t kTestKey[] = L"LocalCam test key";

static PCCERT_CONTEXT testCert() {
    NCRYPT_PROV_HANDLE prov = 0;
    NCRYPT_KEY_HANDLE key = 0;
    DWORD bits = 2048;
    PCCERT_CONTEXT cert = nullptr;
    if (!NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) &&
        !NCryptCreatePersistedKey(prov, &key, BCRYPT_RSA_ALGORITHM, kTestKey, 0, NCRYPT_OVERWRITE_KEY_FLAG) &&
        !NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&bits), sizeof bits, 0) &&
        !NCryptFinalizeKey(key, 0)) {
        BYTE name[64];
        DWORD n = sizeof name;
        CertStrToNameW(X509_ASN_ENCODING, L"CN=LocalCam test", CERT_X500_NAME_STR, nullptr, name, &n, nullptr);
        CERT_NAME_BLOB subject{n, name};
        CRYPT_KEY_PROV_INFO info{const_cast<wchar_t*>(kTestKey), const_cast<wchar_t*>(MS_KEY_STORAGE_PROVIDER), 0, 0, 0, nullptr, 0};
        CRYPT_ALGORITHM_IDENTIFIER alg{const_cast<LPSTR>(szOID_RSA_SHA256RSA), {}};
        cert = CertCreateSelfSignCertificate(key, &subject, 0, &info, &alg, nullptr, nullptr, nullptr);
    }
    if (key) NCryptFreeObject(key);
    if (prov) NCryptFreeObject(prov);
    return cert;
}

static void deleteTestKey() {
    NCRYPT_PROV_HANDLE prov = 0;
    NCRYPT_KEY_HANDLE key = 0;
    if (!NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) && !NCryptOpenKey(prov, &key, kTestKey, 0, 0))
        NCryptDeleteKey(key, 0);
    if (prov) NCryptFreeObject(prov);
}

static SOCKET dial(int port) {
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(u_short(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    return s;
}

// True when the server closed s within ms. It never sends anything to a client that hasn't finished a TLS hello.
static bool closedWithin(SOCKET s, DWORD ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
    char b;
    const int r = recv(s, &b, 1, 0);
    return r == 0 || (r < 0 && WSAGetLastError() != WSAETIMEDOUT);
}

int main() {
    // Same size: straight copy, and mirror flips columns.
    {
        Picture p(4, 2, 0, 0);
        std::vector<uint8_t> out(4 * 2 * 3);
        drawNv12(p.nv12(), out.data(), 4, 2, 0, false);
        CHECK(white(out, 4, 0, 0) && whiteCount(out) == 1);
        drawNv12(p.nv12(), out.data(), 4, 2, 0, true);
        CHECK(white(out, 4, 3, 0) && whiteCount(out) == 1);
    }
    // Rotations of a 4x2 picture into a 2x4 frame: the top-left pixel lands in each corner in turn.
    {
        Picture p(4, 2, 0, 0);
        std::vector<uint8_t> out(2 * 4 * 3);
        drawNv12(p.nv12(), out.data(), 2, 4, 1, false);  // 90 clockwise: top-left -> top-right
        CHECK(white(out, 2, 1, 0) && whiteCount(out) == 1);
        drawNv12(p.nv12(), out.data(), 2, 4, 3, false);  // 90 counterclockwise: top-left -> bottom-left
        CHECK(white(out, 2, 0, 3) && whiteCount(out) == 1);
        drawNv12(p.nv12(), out.data(), 2, 4, 1, true);   // then mirrored: top-left
        CHECK(white(out, 2, 0, 0) && whiteCount(out) == 1);
        std::vector<uint8_t> same(4 * 2 * 3);
        drawNv12(p.nv12(), same.data(), 4, 2, 2, false);  // 180: bottom-right
        CHECK(white(same, 4, 3, 1) && whiteCount(same) == 1);
    }
    // Letterboxing: a 4x2 picture in a 4x4 frame sits in rows 1-2 with black bars.
    {
        Picture p(4, 2, 1, 1);
        std::vector<uint8_t> out(4 * 4 * 3);
        drawNv12(p.nv12(), out.data(), 4, 4, 0, false);
        CHECK(white(out, 4, 1, 2) && whiteCount(out) == 1);
    }
    // Limited-range BT.709 grey and red.
    {
        std::vector<uint8_t> px = {126, 126, 126, 126, 128, 128};
        std::vector<uint8_t> out(2 * 2 * 3);
        drawNv12({px.data(), px.data() + 4, 2, 2, 2, false, false}, out.data(), 2, 2, 0, false);
        CHECK(out[0] > 124 && out[0] < 131 && out[1] == out[0] && out[2] == out[0]);
        px = {63, 63, 63, 63, 102, 240};  // BT.709 limited red
        drawNv12({px.data(), px.data() + 4, 2, 2, 2, false, false}, out.data(), 2, 2, 0, false);
        CHECK(out[2] > 250 && out[1] < 5 && out[0] < 5);
    }
    // Windows' H.264 decoder: three 128x72 red frames (x264 baseline), one access unit per call like the phone
    // sends them. Every frame must come out right away (low-latency mode) and the output buffer gets reused.
    {
        static const uint8_t clip[] = {
            0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xd9, 0x02, 0x0b, 0xf9, 0x70, 0x11, 0x00, 0x00, 0x03, 0x00,
            0x01, 0x00, 0x00, 0x03, 0x00, 0x3c, 0x0f, 0x12, 0x26, 0x48, 0x00, 0x00, 0x00, 0x01, 0x68, 0xcb, 0x83, 0xcb,
            0x20, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x0b, 0xf1, 0x18, 0xa0, 0x00, 0x24, 0x73, 0x1c, 0x00, 0x04, 0x43,
            0x63, 0x80, 0x00, 0x9a, 0xd4, 0x9c, 0x9c, 0x9c, 0x9c, 0x9c, 0x9c, 0x9d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d,
            0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5e,
            0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x38, 0x15, 0xe0, 0xa6, 0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x54, 0x04,
            0xf8, 0x29, 0x80};
        const size_t cuts[] = {0, 90, 100, sizeof clip};
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        Decoder decoder;
        CHECK(decoder.ok());
        std::vector<uint8_t> out(128 * 72 * 3);
        for (int i = 0; i < 3; i++) {
            int frames = 0;
            CHECK(decoder.decode(clip + cuts[i], cuts[i + 1] - cuts[i], [&](const Nv12& f) {
                frames++;
                CHECK(f.width == 128 && f.height == 72);
                drawNv12(f, out.data(), 128, 72, 0, false);
            }));
            CHECK(frames == 1);
        }
        CHECK(out[2] > 230 && out[1] < 30 && out[0] < 30);  // red (BGR)
    }
    // RFC 6455 section 1.3 example.
    CHECK(wsAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    // Request heads: a header on a line cut off by the size limit must not read past the end.
    CHECK(header("GET / HTTP/1.1\r\nHost: 127.0.0.1:47474\r\n\r\n", "host") == "127.0.0.1:47474");
    CHECK(header("GET / HTTP/1.1\r\nUpgrade:  websocket  ", "Upgrade") == "websocket");
    // Only IP addresses and localhost are ours; a host name means DNS rebinding.
    CHECK(hostAllowed("127.0.0.1:47474") && hostAllowed("203.0.113.7") && hostAllowed("localhost:47474"));
    CHECK(!hostAllowed("evil.example:47474") && !hostAllowed("127.0.0.1.evil.example") && !hostAllowed(""));

    // The server on a test port: one address holds at most 6 connections, and a client that hasn't sent its
    // request 5 s after connecting is dropped, even one trickling bytes to dodge the 10 s idle timeout.
    {
        const int port = 47499;
        const PCCERT_CONTEXT cert = testCert();
        std::string error;
        const bool started = cert && startServer(port, std::string(32, 'a'), "page", cert, [](Conn*) {}, &error);
        CHECK(started);
        if (started) {
            std::vector<SOCKET> held;
            for (int i = 0; i < 6; i++) held.push_back(dial(port));
            const SOCKET extra = dial(port);
            CHECK(closedWithin(extra, 1000));
            CHECK(!closedWithin(held[0], 500));
            for (SOCKET s : held) closesocket(s);
            closesocket(extra);
            Sleep(300);  // the server notices and frees the slots

            const SOCKET idle = dial(port), slow = dial(port);
            const ULONGLONG start = GetTickCount64();
            const unsigned char record[] = {0x16, 0x03, 0x01, 0x3f, 0x00};  // a TLS handshake record announcing 16 KB
            bool cut = false;
            for (int i = 0; i < 10 && !cut; i++) {
                const char b = i < 5 ? char(record[i]) : 0;
                send(slow, &b, 1, 0);
                cut = closedWithin(slow, 1000);
            }
            const ULONGLONG took = GetTickCount64() - start;
            CHECK(cut && took >= 4000 && took < 7000);
            CHECK(closedWithin(idle, 500));
            closesocket(idle);
            closesocket(slow);
        }
        deleteTestKey();
    }

    // The virtual camera: a video app that stops while holding the frame lock it shares with LocalCam must not
    // freeze LocalCam's calls into softcam. Skipped while a LocalCam camera already exists (LocalCam is running).
    if (scCamera cam = scCreateCamera(64, 48, 0)) {
        HANDLE held = CreateEventW(nullptr, TRUE, FALSE, nullptr), done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread holder([&] {
            HANDLE lock = CreateMutexA(nullptr, FALSE, "LocalCam/NamedMutex");  // the name in FrameBuffer.cpp
            WaitForSingleObject(lock, INFINITE);
            SetEvent(held);
            WaitForSingleObject(done, 5000);
            ReleaseMutex(lock);
            CloseHandle(lock);
        });
        WaitForSingleObject(held, INFINITE);
        std::vector<uint8_t> frame(64 * 48 * 3);
        const ULONGLONG start = GetTickCount64();
        scSendFrame(cam, frame.data());
        scDeleteCamera(cam);
        CHECK(GetTickCount64() - start < 2000);
        SetEvent(done);
        holder.join();
        CloseHandle(held);
        CloseHandle(done);
    } else {
        std::printf("note: a LocalCam camera already exists, so the camera lock check was skipped\n");
    }

    std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
