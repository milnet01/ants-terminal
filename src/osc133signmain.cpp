// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

// ANTS-5428 — sign an OSC 133 marker without putting the key on a command
// line. `openssl dgst -hmac KEY` takes the key as an argument, where `ps`
// shows it to every local user. This reads it from stdin instead:
//
//     printf '%s' "$ANTS_OSC133_KEY" | ants-osc133-sign "$msg"
//
// and prints the hex HMAC-SHA256 of MESSAGE under the key's raw bytes, as
// TerminalGrid's verifier computes it. One trailing newline on the key is
// dropped. Contract: tests/features/osc133_sign_helper/spec.md.

#include <QByteArray>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

constexpr qsizetype kMaxKeyBytes = 4096;

int refuse(const char *why)
{
    std::fprintf(stderr, "ants-osc133-sign: %s\n", why);
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
        return refuse("usage: printf '%s' KEY | ants-osc133-sign MESSAGE");

    QByteArray key;
    char buf[1024];
    for (;;) {
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return refuse("cannot read the key from stdin");
        }
        if (n == 0) break;
        key.append(buf, n);
        // One byte of slack for the trailing newline dropped below.
        if (key.size() > kMaxKeyBytes + 1) return refuse("key over 4 KiB");
    }
    if (key.endsWith('\n')) key.chop(1);
    if (key.isEmpty()) return refuse("empty key");
    if (key.size() > kMaxKeyBytes) return refuse("key over 4 KiB");

    QMessageAuthenticationCode mac(QCryptographicHash::Sha256, key);
    key.fill('\0');
    mac.addData(argv[1], static_cast<qsizetype>(std::strlen(argv[1])));
    std::puts(mac.result().toHex().constData());
    return 0;
}
