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
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebView/Extensions.h>
#include <LibWebView/LocalFileRoot.h>
#include <LibWebView/Utilities.h>

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

static ErrorOr<ByteBuffer> read_file(StringView directory, StringView relative_path)
{
    // A path in a manifest is relative to the extension's directory, which it may not leave.
    auto path = LexicalPath::canonicalized_path(LexicalPath::join(directory, relative_path).string());
    if (!path.starts_with(directory) || (path.length() > directory.length() && path[directory.length()] != '/'))
        return Error::from_string_literal("Path leaves the extension's directory");

    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
    return file->read_until_eof();
}

static ErrorOr<String> read_text_file(StringView directory, StringView relative_path)
{
    auto contents = TRY(read_file(directory, relative_path));
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

ErrorOr<String> read_extension_text_file(LoadedExtension const& extension, StringView relative_path)
{
    return read_text_file(extension.directory, relative_path.trim("/"sv, TrimMode::Left));
}

ErrorOr<ByteBuffer> read_extension_file(LoadedExtension const& extension, StringView relative_path)
{
    return read_file(extension.directory, relative_path.trim("/"sv, TrimMode::Left));
}

// https://developer.chrome.com/docs/extensions/reference/manifest/icons
Optional<String> icon_path(LoadedExtension const& extension, int size)
{
    auto best_of = [&](JsonObject const& icons) -> Optional<String> {
        // The smallest icon at least as large as asked for, or else the largest there is.
        Optional<String> best_path;
        int best_size = 0;
        icons.for_each_member([&](String const& key, JsonValue const& value) {
            auto icon_size = key.to_number<int>();
            if (!icon_size.has_value() || *icon_size <= 0 || !value.is_string())
                return;
            bool is_better = !best_path.has_value()
                || (best_size < size && *icon_size > best_size)
                || (*icon_size >= size && *icon_size < best_size);
            if (is_better) {
                best_path = value.as_string();
                best_size = *icon_size;
            }
        });
        return best_path;
    };

    if (auto icons = extension.manifest.get_object("icons"sv); icons.has_value()) {
        if (auto path = best_of(*icons); path.has_value())
            return path;
    }
    // Without icons of its own, an extension is shown with its toolbar button's.
    for (auto key : { "action"sv, "browser_action"sv }) {
        auto action = extension.manifest.get_object(key);
        if (!action.has_value())
            continue;
        if (auto icon = action->get_string("default_icon"sv); icon.has_value())
            return icon.copy();
        if (auto icons = action->get_object("default_icon"sv); icons.has_value())
            return best_of(*icons);
    }
    return {};
}

// https://developer.chrome.com/docs/extensions/develop/ui/options-page
Optional<String> options_page_path(LoadedExtension const& extension)
{
    if (auto options_ui = extension.manifest.get_object("options_ui"sv); options_ui.has_value()) {
        if (auto page = options_ui->get_string("page"sv); page.has_value())
            return page.copy();
    }
    return extension.manifest.get_string("options_page"sv).copy();
}

Optional<String> background_page_path(LoadedExtension const& extension)
{
    auto background = extension.manifest.get_object("background"sv);
    if (!background.has_value())
        return {};
    if (auto page = background->get_string("page"sv); page.has_value())
        return *page;
    if (background->has("service_worker"sv) || background->has("scripts"sv))
        return String::from_utf8_without_validation(generated_background_page_path.bytes());
    return {};
}

static String escape_html_attribute(StringView value)
{
    StringBuilder builder;
    for (auto c : value) {
        if (c == '"')
            builder.append("&quot;"sv);
        else if (c == '&')
            builder.append("&amp;"sv);
        else if (c == '<')
            builder.append("&lt;"sv);
        else
            builder.append(c);
    }
    return MUST(builder.to_string());
}

// Like Firefox, this runs a Manifest V3 service worker as the scripts of a background page rather than as a service
// worker. An extension's background code mostly uses the extension APIs, which both offer alike.
static String generated_background_page(LoadedExtension const& extension)
{
    StringBuilder builder;
    builder.append("<!DOCTYPE html>\n<meta charset=\"utf-8\">\n"sv);

    auto background = extension.manifest.get_object("background"sv);
    if (background.has_value()) {
        auto is_module = background->get_string("type"sv).value_or({}) == "module"sv;
        auto append_script = [&](StringView path) {
            builder.appendff("<script{} src=\"/{}\"></script>\n", is_module ? " type=\"module\""sv : ""sv, escape_html_attribute(path.trim("/"sv, TrimMode::Left)));
        };

        if (auto service_worker = background->get_string("service_worker"sv); service_worker.has_value()) {
            append_script(*service_worker);
        } else {
            for (auto const& script : strings_of(*background, "scripts"sv))
                append_script(script);
        }
    }

    return MUST(builder.to_string());
}

ErrorOr<IPC::File> file_with_contents(ReadonlyBytes contents)
{
    // Contents larger than the buffer of a pipe go to a file that nothing else can open, so that writing them doesn't
    // wait for a reader.
    static constexpr size_t smallest_pipe_buffer_size = 16 * KiB;
    if (contents.size() > smallest_pipe_buffer_size) {
        auto pattern = ByteString::formatted("{}/ladybird-extension-XXXXXX", Core::StandardPaths::tempfile_directory());
        Vector<char> path;
        path.append(pattern.characters(), pattern.length());
        path.append('\0');
        auto fd = TRY(Core::System::mkstemp(path.span()));
        (void)Core::System::unlink(StringView { path.data(), pattern.length() });
        auto file = TRY(Core::File::adopt_fd(fd, Core::File::OpenMode::ReadWrite, Core::File::ShouldCloseFileDescriptor::No));
        auto write_and_rewind = [&]() -> ErrorOr<void> {
            TRY(file->write_until_depleted(contents));
            TRY(file->seek(0, SeekMode::SetPosition));
            return {};
        };
        if (auto result = write_and_rewind(); result.is_error()) {
            (void)Core::System::close(fd);
            return result.release_error();
        }
        return IPC::File::adopt_fd(fd);
    }

    // Otherwise a pipe holds the contents, so that nothing has to be written to disk.
    auto fds = TRY(Core::System::pipe2(O_CLOEXEC));
    auto write_end = TRY(Core::File::adopt_fd(fds[1], Core::File::OpenMode::Write));
    if (auto result = write_end->write_until_depleted(contents); result.is_error()) {
        (void)Core::System::close(fds[0]);
        return result.release_error();
    }
    return IPC::File::adopt_fd(fds[0]);
}

bool extension_has_permission(LoadedExtension const& extension, StringView permission)
{
    return strings_of(extension.manifest, "permissions"sv).contains_slow(permission);
}

static bool is_match_pattern(StringView string)
{
    return string == "<all_urls>"sv || string.contains("://"sv);
}

bool extension_has_host_access(LoadedExtension const& extension, URL::URL const& url, HostAccess access)
{
    if (url.scheme() == "chrome-extension"sv && url.serialized_host() == extension.id)
        return true;

    // Manifest V3 names hosts in host_permissions, and Manifest V2 among its permissions.
    Vector<String> patterns = strings_of(extension.manifest, "host_permissions"sv);
    for (auto& permission : strings_of(extension.manifest, "permissions"sv)) {
        if (is_match_pattern(permission))
            patterns.append(move(permission));
    }
    auto matches = [&](String const& pattern) { return Web::Extensions::match_pattern_matches(pattern, url); };
    if (any_of(patterns, matches))
        return true;

    if (access == HostAccess::IncludingContentScripts) {
        for (auto const& content_script : extension.content_scripts.values()) {
            if (content_script.is_object() && any_of(strings_of(content_script.as_object(), "matches"sv), matches))
                return true;
        }
    }
    return false;
}

bool extension_may_see_tab_with_url(LoadedExtension const& extension, URL::URL const& url)
{
    return extension_has_permission(extension, "tabs"sv) || extension_has_host_access(extension, url, HostAccess::IncludingContentScripts);
}

// https://developer.chrome.com/docs/extensions/reference/manifest/web-accessible-resources
bool is_web_accessible(LoadedExtension const& extension, StringView path)
{
    auto resources = extension.manifest.get_array("web_accessible_resources"sv);
    if (!resources.has_value())
        return false;

    auto matches = [&](StringView glob) {
        return path.matches(glob.trim("/"sv, TrimMode::Left), CaseSensitivity::CaseSensitive);
    };

    for (auto const& entry : resources->values()) {
        // Manifest V2 lists the resources themselves.
        if (entry.is_string() && matches(entry.as_string()))
            return true;
        // Manifest V3 lists them with the sites that may use them.
        // FIXME: Check the site of the requesting page against the entry's matches.
        if (entry.is_object() && any_of(strings_of(entry.as_object(), "resources"sv), [&](auto const& glob) { return matches(glob); }))
            return true;
    }
    return false;
}

ErrorOr<IPC::File> open_extension_file(LoadedExtension const& extension, StringView url_path, ExtensionFileRequester requester)
{
    auto path = URL::percent_decode(url_path);
    auto relative_path = path.view().trim("/"sv, TrimMode::Left);

    if (requester == ExtensionFileRequester::WebPage && !is_web_accessible(extension, relative_path))
        return Error::from_errno(EACCES);

    if (relative_path == generated_background_page_path) {
        if (requester != ExtensionFileRequester::ExtensionPage)
            return Error::from_errno(EACCES);
        auto page = generated_background_page(extension);
        return file_with_contents(page.bytes());
    }

    // The roots keep the path, once its symbolic links are resolved, in the extension's directory.
    LocalFileRoot root { .path = extension.directory, .scope = LocalFileRoot::Scope::Directory };
    auto opened = TRY(open_local_file_for_renderer(LexicalPath::join(extension.directory, relative_path).string(), { &root, 1 }));
    return move(opened.file);
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
