#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#include "server.h"

#include <bcrypt.h>
#include <ncrypt.h>
#include <schannel.h>
#include <security.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <thread>

struct Conn {
    SOCKET sock = INVALID_SOCKET;
    ULONGLONG deadline = 0;   // GetTickCount64 time the request head must be in by; 0 once it is
    CtxtHandle ctx{};
    bool hasCtx = false;
    SecPkgContext_StreamSizes sizes{};
    std::vector<char> in;     // received ciphertext not decrypted yet
    std::vector<char> plain;  // decrypted bytes not consumed yet
    size_t plainPos = 0;
    std::mutex sendMutex;
    ~Conn() {
        if (hasCtx) DeleteSecurityContext(&ctx);
        if (sock != INVALID_SOCKET) closesocket(sock);
    }
};

static std::string lower(std::string s) {
    for (auto& ch : s) ch = char(tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string header(const std::string& head, const char* name) {
    const std::string l = lower(head);
    const size_t p = l.find("\r\n" + lower(name) + ":");
    if (p == std::string::npos) return {};
    size_t b = p + 3 + std::strlen(name), e = head.find("\r\n", b);
    if (e == std::string::npos) e = head.size();
    while (b < e && head[b] == ' ') b++;
    while (e > b && head[e - 1] == ' ') e--;
    return head.substr(b, e - b);
}

bool hostAllowed(const std::string& host) {
    const std::string name = host.substr(0, host.rfind(':'));
    IN_ADDR a;
    return name == "localhost" || inet_pton(AF_INET, name.c_str(), &a) == 1;
}

namespace {

constexpr size_t kMaxMessage = 8 << 20;  // a 1080p keyframe is a few hundred KB
constexpr int kMaxConnections = 16, kMaxPerAddress = 6;  // 6: what a browser opens to one host at most
constexpr DWORD kSetupMs = 5000;  // accept to the end of the request head, so slow senders can't hold slots
constexpr DWORD kIdleMs = 10000;  // phones stream nonstop, so 10 s of silence means gone
constexpr size_t kMaxRecordBytes = 64 << 10;  // TLS records are at most ~18 KB; anything bigger is bogus

CredHandle g_cred;
std::string g_page;
PhoneHandler g_handler;

std::mutex g_tokenMutex;  // the tray can replace the token while connections check it
std::string g_token;

std::mutex g_connMutex;  // guards g_connections and g_perAddress
int g_connections = 0;
std::map<ULONG, int> g_perAddress;  // open connections per peer IPv4 address

std::mutex g_activeMutex;  // guards g_active and g_generation
Conn* g_active = nullptr;
uint64_t g_generation = 0;
std::mutex g_phoneSlot;    // held while a phone handler runs

bool sendRaw(SOCKET s, const char* p, size_t n) {
    while (n) {
        const int r = send(s, p, int(std::min<size_t>(n, 1 << 20)), 0);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

// recv, cut short by the connection's deadline while it has one.
int recvChunk(Conn& c, char* buf, int len) {
    if (c.deadline) {
        const ULONGLONG now = GetTickCount64();
        if (now >= c.deadline) return 0;
        const DWORD left = DWORD(c.deadline - now);
        setsockopt(c.sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&left), sizeof left);
    }
    return recv(c.sock, buf, len, 0);
}

bool handshake(Conn& c) {
    char buf[16384];
    bool needMore = true;
    for (;;) {
        if (needMore) {
            const int n = recvChunk(c, buf, sizeof buf);
            if (n <= 0 || c.in.size() > kMaxRecordBytes) return false;
            c.in.insert(c.in.end(), buf, buf + n);
        }
        SecBuffer ib[2] = {{ULONG(c.in.size()), SECBUFFER_TOKEN, c.in.data()}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBuffer ob[1] = {{0, SECBUFFER_TOKEN, nullptr}};
        SecBufferDesc id{SECBUFFER_VERSION, 2, ib}, od{SECBUFFER_VERSION, 1, ob};
        ULONG attrs = 0;
        const SECURITY_STATUS ss = AcceptSecurityContext(
            &g_cred, c.hasCtx ? &c.ctx : nullptr, &id,
            ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR |
                ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT,
            0, &c.ctx, &od, &attrs, nullptr);
        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            needMore = true;
            continue;
        }
        if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED) c.hasCtx = true;
        if (ob[0].pvBuffer) {
            const bool sent = sendRaw(c.sock, static_cast<char*>(ob[0].pvBuffer), ob[0].cbBuffer);
            FreeContextBuffer(ob[0].pvBuffer);
            if (!sent) return false;
        }
        if (ss != SEC_E_OK && ss != SEC_I_CONTINUE_NEEDED) return false;
        if (ib[1].BufferType == SECBUFFER_EXTRA) c.in.erase(c.in.begin(), c.in.end() - ib[1].cbBuffer);
        else c.in.clear();
        if (ss == SEC_E_OK) return QueryContextAttributes(&c.ctx, SECPKG_ATTR_STREAM_SIZES, &c.sizes) == SEC_E_OK;
        needMore = c.in.empty();
    }
}

// Reads up to len decrypted bytes; 0 means closed or failed.
int tlsRead(Conn& c, char* out, int len) {
    while (c.plainPos == c.plain.size()) {
        if (!c.in.empty()) {
            SecBuffer b[4] = {{ULONG(c.in.size()), SECBUFFER_DATA, c.in.data()},
                              {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc d{SECBUFFER_VERSION, 4, b};
            const SECURITY_STATUS ss = DecryptMessage(&c.ctx, &d, 0, nullptr);
            if (ss == SEC_E_OK) {
                std::vector<char> extra;
                c.plain.clear();
                c.plainPos = 0;
                for (auto& x : b) {
                    auto* p = static_cast<char*>(x.pvBuffer);
                    if (x.BufferType == SECBUFFER_DATA) c.plain.assign(p, p + x.cbBuffer);
                    if (x.BufferType == SECBUFFER_EXTRA) extra.assign(p, p + x.cbBuffer);
                }
                c.in.swap(extra);
                continue;
            }
            if (ss != SEC_E_INCOMPLETE_MESSAGE) return 0;  // close_notify or a real error
        }
        char buf[16384];
        const int n = recvChunk(c, buf, sizeof buf);
        if (n <= 0 || c.in.size() > kMaxRecordBytes) return 0;
        c.in.insert(c.in.end(), buf, buf + n);
    }
    const int n = int(std::min<size_t>(len, c.plain.size() - c.plainPos));
    std::memcpy(out, c.plain.data() + c.plainPos, n);
    c.plainPos += n;
    return n;
}

bool readExact(Conn& c, char* p, size_t n) {
    while (n) {
        const int r = tlsRead(c, p, int(std::min<size_t>(n, 1 << 30)));
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

bool tlsWrite(Conn& c, const char* p, size_t n) {
    const auto& z = c.sizes;
    std::vector<char> buf(z.cbHeader + z.cbMaximumMessage + z.cbTrailer);
    while (n) {
        const ULONG chunk = ULONG(std::min<size_t>(n, z.cbMaximumMessage));
        std::memcpy(buf.data() + z.cbHeader, p, chunk);
        SecBuffer b[4] = {{z.cbHeader, SECBUFFER_STREAM_HEADER, buf.data()},
                          {chunk, SECBUFFER_DATA, buf.data() + z.cbHeader},
                          {z.cbTrailer, SECBUFFER_STREAM_TRAILER, buf.data() + z.cbHeader + chunk},
                          {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc d{SECBUFFER_VERSION, 4, b};
        if (EncryptMessage(&c.ctx, 0, &d, 0) != SEC_E_OK) return false;
        if (!sendRaw(c.sock, buf.data(), b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer)) return false;
        p += chunk;
        n -= chunk;
    }
    return true;
}

bool wsSend(Conn& c, int opcode, const std::string& payload) {
    std::string f(1, char(0x80 | opcode));
    const size_t n = payload.size();
    if (n < 126) {
        f += char(n);
    } else if (n < 65536) {
        f += char(126);
        f += char(n >> 8);
        f += char(n & 255);
    } else {
        f += char(127);
        for (int i = 7; i >= 0; i--) f += char((uint64_t(n) >> (8 * i)) & 255);
    }
    f += payload;
    std::lock_guard<std::mutex> lock(c.sendMutex);
    return tlsWrite(c, f.data(), f.size());
}

bool httpRespond(Conn& c, const char* status, const char* type, const std::string& body) {
    const std::string head = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type +
                             "\r\nContent-Length: " + std::to_string(body.size()) +
                             "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY"
                             "\r\nContent-Security-Policy: frame-ancestors 'none'\r\nReferrer-Policy: no-referrer"
                             "\r\nConnection: close\r\n\r\n";
    std::lock_guard<std::mutex> lock(c.sendMutex);
    return tlsWrite(c, head.data(), head.size()) && tlsWrite(c, body.data(), body.size());
}

bool sameSecret(const std::string& a, const std::string& b) {
    if (a.empty() || a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); i++) d |= a[i] ^ b[i];
    return d == 0;
}

std::string pairingToken() {
    std::lock_guard<std::mutex> lock(g_tokenMutex);
    return g_token;
}

void runPhone(Conn& c) {
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(g_activeMutex);
        generation = ++g_generation;
        if (g_active) {  // the newest phone wins; tell the old one so it doesn't reconnect and fight back
            wsSend(*g_active, 1, R"({"type":"replaced"})");
            shutdown(g_active->sock, SD_BOTH);
        }
    }
    std::lock_guard<std::mutex> slot(g_phoneSlot);
    {
        std::lock_guard<std::mutex> lock(g_activeMutex);
        if (generation != g_generation) return;  // an even newer phone is waiting
        g_active = &c;
    }
    g_handler(&c);
    std::lock_guard<std::mutex> lock(g_activeMutex);
    g_active = nullptr;
}

void handleHttp(Conn& c) {
    std::string head;
    char ch;
    while (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0) {
        if (head.size() >= 8192 || tlsRead(c, &ch, 1) != 1) return;
        head += ch;
    }
    c.deadline = 0;
    setsockopt(c.sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&kIdleMs), sizeof kIdleMs);
    const size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return;
    const std::string method = head.substr(0, sp1), target = head.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    const std::string path = target.substr(0, q), query = q == std::string::npos ? "" : "&" + target.substr(q + 1);

    const std::string host = header(head, "Host");
    if (!hostAllowed(host)) {
        httpRespond(c, "421 Misdirected Request", "text/plain", "Open LocalCam by the PC's IP address.\n");
    } else if (method != "GET") {
        httpRespond(c, "405 Method Not Allowed", "text/plain", "Method not allowed\n");
    } else if (path == "/") {
        httpRespond(c, "200 OK", "text/html; charset=utf-8", g_page);
    } else if (path == "/ws") {
        const size_t t = query.find("&t=");
        const std::string token = t == std::string::npos ? "" : query.substr(t + 3, query.find('&', t + 3) - t - 3);
        const std::string key = header(head, "Sec-WebSocket-Key"), origin = header(head, "Origin");
        if (!sameSecret(token, pairingToken())) {
            logf("rejected a connection with a wrong token");
            httpRespond(c, "403 Forbidden", "text/plain", "Wrong token. Scan the QR code again.\n");
        } else if (!origin.empty() && lower(origin) != "https://" + lower(host)) {  // a page on another site
            logf("rejected a connection from another site");
            httpRespond(c, "403 Forbidden", "text/plain", "Cross-site request refused.\n");
        } else if (lower(header(head, "Upgrade")) != "websocket" || key.empty()) {
            httpRespond(c, "400 Bad Request", "text/plain", "Expected a WebSocket upgrade\n");
        } else {
            const std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                      "Connection: Upgrade\r\nSec-WebSocket-Accept: " + wsAcceptKey(key) + "\r\n\r\n";
            bool sent;
            {
                std::lock_guard<std::mutex> lock(c.sendMutex);
                sent = tlsWrite(c, reply.data(), reply.size());
            }
            if (sent) runPhone(c);
        }
    } else {
        httpRespond(c, "404 Not Found", "text/plain", "Not found\n");
    }
}

bool admit(ULONG ip) {
    std::lock_guard<std::mutex> lock(g_connMutex);
    int& n = g_perAddress[ip];
    if (g_connections >= kMaxConnections || n >= kMaxPerAddress) {
        if (!n) g_perAddress.erase(ip);
        return false;
    }
    n++;
    g_connections++;
    return true;
}

void release(ULONG ip) {
    std::lock_guard<std::mutex> lock(g_connMutex);
    if (!--g_perAddress[ip]) g_perAddress.erase(ip);
    g_connections--;
}

void serve(SOCKET s, ULONG ip) {
    {
        Conn c;
        c.sock = s;
        c.deadline = GetTickCount64() + kSetupMs;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&kIdleMs), sizeof kIdleMs);
        const BOOL on = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
        if (handshake(c)) handleHttp(c);
    }
    release(ip);
}

}  // namespace

int readMessage(Conn* c, std::string& msg) {
    int messageOp = 0;
    msg.clear();
    for (;;) {
        unsigned char h[2];
        if (!readExact(*c, reinterpret_cast<char*>(h), 2)) return 0;
        const bool fin = h[0] & 0x80;
        const int op = h[0] & 0x0F;
        uint64_t len = h[1] & 0x7F;
        if (!(h[1] & 0x80)) return 0;  // clients must mask
        if (len >= 126) {
            unsigned char e[8];
            const int n = len == 126 ? 2 : 8;
            if (!readExact(*c, reinterpret_cast<char*>(e), n)) return 0;
            len = 0;
            for (int i = 0; i < n; i++) len = len << 8 | e[i];
        }
        if (len > kMaxMessage || msg.size() + len > kMaxMessage) return 0;
        unsigned char mask[4];
        if (!readExact(*c, reinterpret_cast<char*>(mask), 4)) return 0;
        std::string payload(size_t(len), '\0');
        if (!readExact(*c, payload.data(), payload.size())) return 0;
        for (size_t i = 0; i < payload.size(); i++) payload[i] ^= mask[i & 3];

        if (op == 8) {  // close
            wsSend(*c, 8, payload.substr(0, 2));
            return 0;
        }
        if (op == 9) {  // ping
            wsSend(*c, 10, payload);
            continue;
        }
        if (op == 10) continue;  // pong
        if (op == 1 || op == 2) {
            if (messageOp) return 0;
            messageOp = op;
            msg = std::move(payload);
        } else if (op == 0 && messageOp) {
            msg += payload;
        } else {
            return 0;
        }
        if (fin) return messageOp;
    }
}

void sendToPhone(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_activeMutex);
    if (g_active) wsSend(*g_active, 1, text);
}

void setToken(const std::string& token) {
    {
        std::lock_guard<std::mutex> lock(g_tokenMutex);
        g_token = token;
    }
    std::lock_guard<std::mutex> lock(g_activeMutex);
    if (g_active) {
        wsSend(*g_active, 1, R"({"type":"unpaired"})");
        shutdown(g_active->sock, SD_BOTH);
    }
}

bool startServer(int port, const std::string& token, const std::string& page, PCCERT_CONTEXT cert,
                 PhoneHandler handler, std::string* error) {
    g_token = token;
    g_page = page;
    g_handler = std::move(handler);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.cCreds = 1;
    sc.paCred = &cert;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER;
    sc.dwFlags = SCH_USE_STRONG_CRYPTO;
    const SECURITY_STATUS ss = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_INBOUND,
                                                         nullptr, &sc, nullptr, nullptr, &g_cred, nullptr);
    if (ss != SEC_E_OK) {
        char msg[80];
        std::snprintf(msg, sizeof msg, "TLS setup failed (0x%08lX)", static_cast<unsigned long>(ss));
        *error = msg;
        return false;
    }

    const SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const BOOL on = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof on);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(u_short(port));
    addr.sin_addr.s_addr = INADDR_ANY;
    if (ls == INVALID_SOCKET || bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) || listen(ls, SOMAXCONN)) {
        *error = "Port " + std::to_string(port) + " is already in use.";
        return false;
    }
    std::thread([ls] {
        for (;;) {
            sockaddr_in peer{};
            int peerLen = sizeof peer;
            const SOCKET s = accept(ls, reinterpret_cast<sockaddr*>(&peer), &peerLen);
            if (s == INVALID_SOCKET) {
                Sleep(100);  // out of sockets or memory: don't spin
                continue;
            }
            const ULONG ip = peer.sin_addr.s_addr;
            if (!admit(ip)) {
                closesocket(s);
                continue;
            }
            std::thread(serve, s, ip).detach();
        }
    }).detach();
    return true;
}

std::string wsAcceptKey(const std::string& clientKey) {
    const std::string s = clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    BYTE hash[20];
    BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, PUCHAR(s.data()), ULONG(s.size()), hash, sizeof hash);
    char out[64];
    DWORD n = sizeof out;
    CryptBinaryToStringA(hash, sizeof hash, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &n);
    return std::string(out, n);
}

bool certUsable(PCCERT_CONTEXT c, const std::string& ip) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER soon{{now.dwLowDateTime, now.dwHighDateTime}};
    soon.QuadPart += 30ull * 24 * 3600 * 10000000;
    const FILETIME soonFt{soon.LowPart, soon.HighPart};
    if (CompareFileTime(&soonFt, &c->pCertInfo->NotAfter) >= 0) return false;
    if (ip.empty()) return true;

    IN_ADDR want{};
    inet_pton(AF_INET, ip.c_str(), &want);
    PCERT_EXTENSION ext = CertFindExtension(szOID_SUBJECT_ALT_NAME2, c->pCertInfo->cExtension, c->pCertInfo->rgExtension);
    CERT_ALT_NAME_INFO* info = nullptr;
    DWORD size = 0;
    if (!ext || !CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME, ext->Value.pbData, ext->Value.cbData,
                                     CRYPT_DECODE_ALLOC_FLAG, nullptr, &info, &size))
        return false;
    bool found = false;
    for (DWORD i = 0; i < info->cAltEntry; i++) {
        const auto& e = info->rgAltEntry[i];
        found |= e.dwAltNameChoice == CERT_ALT_NAME_IP_ADDRESS && e.IPAddress.cbData == 4 &&
                 std::memcmp(e.IPAddress.pbData, &want, 4) == 0;
    }
    LocalFree(info);
    return found;
}

namespace {

// DER-encodes a CryptoAPI structure.
std::vector<BYTE> encode(LPCSTR type, const void* value) {
    DWORD n = 0;
    CryptEncodeObjectEx(X509_ASN_ENCODING, type, value, 0, nullptr, nullptr, &n);
    std::vector<BYTE> out(n);
    CryptEncodeObjectEx(X509_ASN_ENCODING, type, value, 0, nullptr, out.data(), &n);
    out.resize(n);
    return out;
}

// CERT_FIND_SUBJECT_STR matches substrings; only certificates named exactly CN=LocalCam are ours.
bool ours(PCCERT_CONTEXT c) {
    wchar_t cn[32] = L"";
    CertGetNameStringW(c, CERT_NAME_ATTR_TYPE, 0, const_cast<char*>(szOID_COMMON_NAME), cn, 32);
    return !std::wcscmp(cn, L"LocalCam");
}

}  // namespace

PCCERT_CONTEXT loadOrCreateCert(const std::vector<std::string>& ips, std::string* error) {
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"MY");
    if (!store) {
        *error = "Can't open the certificate store.";
        return nullptr;
    }
    const wchar_t* kSubject = L"LocalCam";
    PCCERT_CONTEXT found = nullptr;
    for (PCCERT_CONTEXT c = nullptr;
         (c = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W, kSubject, c));) {
        if (ours(c) && certUsable(c, ips.empty() ? "" : ips[0])) {
            found = c;
            break;
        }
    }
    if (found) {
        CertCloseStore(store, 0);
        return found;
    }
    for (PCCERT_CONTEXT c = nullptr;
         (c = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W, kSubject, c));)
        if (ours(c)) CertDeleteCertificateFromStore(CertDuplicateCertificateContext(c));

    // Key: RSA 2048 in the user's key store, reused by name across certificates.
    const wchar_t* kKeyName = L"LocalCam TLS";
    NCRYPT_PROV_HANDLE prov = 0;
    NCRYPT_KEY_HANDLE key = 0;
    DWORD bits = 2048;
    if (NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) ||
        NCryptCreatePersistedKey(prov, &key, BCRYPT_RSA_ALGORITHM, kKeyName, 0, NCRYPT_OVERWRITE_KEY_FLAG) ||
        NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&bits), sizeof bits, 0) ||
        NCryptFinalizeKey(key, 0)) {
        *error = "Can't create the TLS key.";
        if (key) NCryptFreeObject(key);
        if (prov) NCryptFreeObject(prov);
        CertCloseStore(store, 0);
        return nullptr;
    }

    // What phones require of a TLS certificate (iOS is strictest): IPs in the SAN, serverAuth EKU,
    // SHA-256, at most 825 days.
    std::vector<IN_ADDR> addrs(ips.size() + 1);
    inet_pton(AF_INET, "127.0.0.1", &addrs[0]);
    std::vector<CERT_ALT_NAME_ENTRY> alt(1);
    alt[0].dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
    alt[0].pwszDNSName = const_cast<wchar_t*>(L"localhost");
    for (size_t i = 0; i < addrs.size(); i++) {
        if (i) inet_pton(AF_INET, ips[i - 1].c_str(), &addrs[i]);
        CERT_ALT_NAME_ENTRY e{};
        e.dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
        e.IPAddress = {4, reinterpret_cast<BYTE*>(&addrs[i])};
        alt.push_back(e);
    }
    CERT_ALT_NAME_INFO altInfo{DWORD(alt.size()), alt.data()};
    std::vector<BYTE> san = encode(X509_ALTERNATE_NAME, &altInfo);
    LPSTR serverAuth = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
    CERT_ENHKEY_USAGE usage{1, &serverAuth};
    std::vector<BYTE> eku = encode(X509_ENHANCED_KEY_USAGE, &usage);
    CERT_EXTENSION ext[2] = {{const_cast<LPSTR>(szOID_SUBJECT_ALT_NAME2), FALSE, {DWORD(san.size()), san.data()}},
                             {const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE), FALSE, {DWORD(eku.size()), eku.data()}}};
    CERT_EXTENSIONS exts{2, ext};

    BYTE name[128];
    DWORD nameLen = sizeof name;
    CertStrToNameW(X509_ASN_ENCODING, L"CN=LocalCam", CERT_X500_NAME_STR, nullptr, name, &nameLen, nullptr);
    CERT_NAME_BLOB subject{nameLen, name};
    CRYPT_ALGORITHM_IDENTIFIER alg{const_cast<LPSTR>(szOID_RSA_SHA256RSA), {}};
    CRYPT_KEY_PROV_INFO keyInfo{const_cast<wchar_t*>(kKeyName), const_cast<wchar_t*>(MS_KEY_STORAGE_PROVIDER), 0, 0, 0, nullptr, 0};

    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const uint64_t day = 24ull * 3600 * 10000000, t = uint64_t(now.dwHighDateTime) << 32 | now.dwLowDateTime;
    const uint64_t from = t - day, to = t + 824 * day;
    FILETIME fromFt{DWORD(from), DWORD(from >> 32)}, toFt{DWORD(to), DWORD(to >> 32)};
    SYSTEMTIME start, end;
    FileTimeToSystemTime(&fromFt, &start);
    FileTimeToSystemTime(&toFt, &end);

    PCCERT_CONTEXT cert = CertCreateSelfSignCertificate(key, &subject, 0, &keyInfo, &alg, &start, &end, &exts);
    PCCERT_CONTEXT stored = nullptr;
    if (cert) {
        CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_REPLACE_EXISTING, &stored);
        CertFreeCertificateContext(cert);
    }
    NCryptFreeObject(key);
    NCryptFreeObject(prov);
    CertCloseStore(store, 0);
    if (!stored) *error = "Can't create the TLS certificate.";
    return stored;
}
