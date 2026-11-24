/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/File.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Extensions.h>

namespace WebView {

String strip_json_comments(StringView json)
{
    StringBuilder builder;
    bool in_string = false;

    for (size_t i = 0; i < json.length(); ++i) {
        auto c = json[i];

        if (in_string) {
            builder.append(c);
            if (c == '\\' && i + 1 < json.length()) {
                builder.append(json[++i]);
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            builder.append(c);
        } else if (c == '/' && i + 1 < json.length() && json[i + 1] == '/') {
            while (i < json.length() && json[i] != '\n')
                ++i;
            builder.append('\n');
        } else if (c == '/' && i + 1 < json.length() && json[i + 1] == '*') {
            i += 2;
            while (i + 1 < json.length() && !(json[i] == '*' && json[i + 1] == '/'))
                ++i;
            ++i;
            builder.append(' ');
        } else {
            builder.append(c);
        }
    }

    return MUST(builder.to_string());
}

// https://chromium.googlesource.com/chromium/src/+/main/components/crx_file/id_util.cc
// An ID is the first 16 bytes of a SHA-256 digest, written with the letters a to p for the hex digits 0 to f.
static String extension_id_for_directory(StringView directory)
{
    auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, directory.bytes());
    StringBuilder builder;
    for (auto byte : digest.bytes().slice(0, 16)) {
        builder.append(static_cast<char>('a' + (byte >> 4)));
        builder.append(static_cast<char>('a' + (byte & 0xf)));
    }
    return MUST(builder.to_string());
}

static ErrorOr<String> read_text_file(StringView directory, StringView relative_path)
{
    // A path in a manifest is relative to the extension's directory, which it may not leave.
    auto path = LexicalPath::canonicalized_path(LexicalPath::join(directory, relative_path).string());
    if (!path.starts_with(directory) || (path.length() > directory.length() && path[directory.length()] != '/'))
        return Error::from_string_literal("Path leaves the extension's directory");

    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
    auto contents = TRY(file->read_until_eof());
    return String::from_utf8(contents);
}

static Vector<String> strings_of(JsonObject const& object, StringView key)
{
    Vector<String> strings;
    if (auto array = object.get_array(key); array.has_value()) {
        for (auto const& value : array->values()) {
            if (value.is_string())
                strings.append(value.as_string());
        }
    }
    return strings;
}

static JsonArray strings_as_json(Vector<String> const& strings)
{
    JsonArray array;
    for (auto const& string : strings)
        array.must_append(string);
    return array;
}

ErrorOr<LoadedExtension> load_unpacked_extension(StringView directory_path)
{
    auto directory = TRY(FileSystem::real_path(directory_path));

    auto manifest_source = TRY(read_text_file(directory, "manifest.json"sv));
    auto manifest_value = TRY(JsonParser::parse(strip_json_comments(manifest_source)));
    if (!manifest_value.is_object())
        return Error::from_string_literal("manifest.json is not a JSON object");
    auto const& manifest = manifest_value.as_object();

    auto manifest_version = manifest.get_integer<i64>("manifest_version"sv);
    if (!manifest_version.has_value() || (*manifest_version != 2 && *manifest_version != 3))
        return Error::from_string_literal("manifest.json has no supported manifest_version");

    LoadedExtension extension;
    extension.id = extension_id_for_directory(directory);
    extension.name = manifest.get_string("name"sv).value_or(extension.id);
    extension.version = manifest.get_string("version"sv).value_or({});
    extension.directory = directory;
    extension.manifest = manifest;

    if (auto content_scripts = manifest.get_array("content_scripts"sv); content_scripts.has_value()) {
        for (auto const& content_script_value : content_scripts->values()) {
            if (!content_script_value.is_object())
                continue;
            auto const& content_script = content_script_value.as_object();

            JsonObject serialized;
            serialized.set("matches"sv, strings_as_json(strings_of(content_script, "matches"sv)));
            serialized.set("exclude_matches"sv, strings_as_json(strings_of(content_script, "exclude_matches"sv)));
            serialized.set("run_at"sv, content_script.get_string("run_at"sv).value_or("document_idle"_string));
            serialized.set("all_frames"sv, content_script.get_bool("all_frames"sv).value_or(false));
            serialized.set("match_about_blank"sv, content_script.get_bool("match_about_blank"sv).value_or(false));

            JsonArray js;
            for (auto const& path : strings_of(content_script, "js"sv)) {
                JsonObject file;
                file.set("path"sv, path);
                file.set("source"sv, TRY(read_text_file(directory, path)));
                js.must_append(move(file));
            }
            serialized.set("js"sv, move(js));

            StringBuilder css;
            for (auto const& path : strings_of(content_script, "css"sv)) {
                css.append(TRY(read_text_file(directory, path)));
                css.append('\n');
            }
            serialized.set("css"sv, TRY(css.to_string()));

            extension.content_scripts.must_append(move(serialized));
        }
    }

    return extension;
}

String serialize_extensions_for_web_content(Vector<LoadedExtension> const& extensions)
{
    JsonArray array;
    for (auto const& extension : extensions) {
        JsonObject object;
        object.set("id"sv, extension.id);
        object.set("manifest"sv, extension.manifest.serialized());
        object.set("content_scripts"sv, extension.content_scripts);
        array.must_append(move(object));
    }
    return array.serialized();
}

}
