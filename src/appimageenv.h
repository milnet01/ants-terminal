#pragma once

// ANTS-5574 — packaging/appimage/AppRun puts a private directory first on
// LD_LIBRARY_PATH, where the unversioned libssl.so and libcrypto.so link to
// the host's OpenSSL 3, and names that directory in ANTS_OPENSSL3_SHIM. The
// bundled Qt 6.2 loads OpenSSL by the unversioned name, so without it a host
// whose libssl.so is LibreSSL or OpenSSL 1.x gets no TLS at all.
//
// The dynamic loader reads LD_LIBRARY_PATH once, at process start, so
// removing the entry from the environment afterwards keeps Qt's lookup and
// keeps the directory out of every process the terminal starts, such as the
// shells in its tabs.

#include <QByteArray>

namespace AppImageEnv {

// `pathList` (colon-separated) without every entry equal to `entry`; every
// other entry, empty ones included, keeps its place.
QByteArray withoutEntry(const QByteArray &pathList, const QByteArray &entry);

// Removes ANTS_OPENSSL3_SHIM's directory from this process's LD_LIBRARY_PATH
// (unsetting it when nothing is left) and unsets ANTS_OPENSSL3_SHIM. Does
// nothing when ANTS_OPENSSL3_SHIM is unset. Call first thing in main().
void dropOpenSsl3Shim();

}  // namespace AppImageEnv
