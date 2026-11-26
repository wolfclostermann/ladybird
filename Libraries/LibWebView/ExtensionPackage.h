/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibWebView/Forward.h>

// Packed extensions: .crx files (CRX3, as Chromium makes them) and plain .zip archives of an extension's folder.
namespace WebView {

struct CrxContents {
    // The developer's public key (a DER SubjectPublicKeyInfo), from which the extension's ID derives.
    ByteBuffer public_key;
    // The zip archive within the file.
    ReadonlyBytes archive;
};

// Reads a CRX3 file and checks its signatures: https://chromium.googlesource.com/chromium/src/+/main/components/crx_file/
WEBVIEW_API ErrorOr<CrxContents> read_crx(ReadonlyBytes);

// Writes the files of a zip archive into an empty directory. Only stored and deflated files are supported, and no
// file may land outside the directory. If the extension's files all sit in one folder (as in the archives made by
// zipping that folder), that folder's contents are written instead.
WEBVIEW_API ErrorOr<void> extract_zip(ReadonlyBytes, StringView directory);

// Like Chromium, an extension packed with a key has the ID that derives from the key.
WEBVIEW_API String extension_id_for_public_key(ReadonlyBytes);

// Unpacks a .crx or .zip file into a folder of its own within the given directory, and returns that folder. A .crx
// file's key is written into its manifest as "key", so that the unpacked extension keeps the ID of the packed one. The
// folder is named after that ID, or for a .zip archive after its path, and the browser takes a .zip's folder name as
// its ID.
// The folder replaces what was unpacked there before only once the new files load as an extension.
WEBVIEW_API ErrorOr<ByteString> unpack_extension_package(StringView package_path, StringView packages_directory);

}
