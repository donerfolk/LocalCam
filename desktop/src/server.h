#pragma once
// HTTPS + secure WebSocket server on Windows' own TLS (SChannel). Serves the phone page and accepts
// one phone stream at a time; a newer phone connection replaces the current one.
#include <windows.h>
#include <wincrypt.h>

#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

inline void logf(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stderr, fmt, a);
    va_end(a);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

struct Conn;

// Runs on the connection's thread once a phone opened /ws with the right token; return to drop it.
using PhoneHandler = std::function<void(Conn*)>;

bool startServer(int port, const std::string& token, const std::string& page, PCCERT_CONTEXT cert,
                 PhoneHandler handler, std::string* error);

// Reads the next WebSocket message: 1 = text, 2 = binary, 0 = disconnected.
int readMessage(Conn* c, std::string& msg);

// Sends a text message to the connected phone, if any. Safe from any thread.
void sendToPhone(const std::string& text);

// Replaces the pairing token: links with the old one are refused, and the connected phone is told and dropped.
void setToken(const std::string& token);

// Self-signed certificate in the user's certificate store, valid for the given IPv4 addresses.
// Reused while it covers ips[0] and has 30+ days left.
PCCERT_CONTEXT loadOrCreateCert(const std::vector<std::string>& ips, std::string* error);

// True when the certificate has 30+ days left and covers this IPv4 address (any address when empty).
bool certUsable(PCCERT_CONTEXT c, const std::string& ip);

std::string wsAcceptKey(const std::string& clientKey);

// Value of a request header (case-insensitive name), trimmed.
std::string header(const std::string& head, const char* name);

// DNS rebinding guard: the certificate names only IP addresses, so a request for any other host isn't for us.
bool hostAllowed(const std::string& host);
