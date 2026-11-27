/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebCommon/Extensions/WebAccessibleResources.h>
#include <LibWebView/ExtensionPackage.h>
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
// An ID is the first 16 bytes of a SHA-256 digest, written with the letters a to p for the hex digits 0 to f. It is
// the digest of the manifest's "key" (a developer's public key, in base64) if it has one, else of the directory's path,
// unless the caller knows the ID another way.
static ErrorOr<String> extension_id_for(JsonObject const& manifest, StringView directory, Optional<String> const& id_without_key)
{
    if (auto key = manifest.get("key"sv); key.has_value()) {
        if (!key->is_string())
            return Error::from_string_literal("manifest.json has a \"key\" that is not a string");
        auto public_key = decode_base64(key->as_string());
        if (public_key.is_error() || public_key.value().is_empty())
            return Error::from_string_literal("manifest.json has a \"key\" that is not valid base64");
        return extension_id_for_public_key(public_key.value());
    }
    if (id_without_key.has_value())
        return *id_without_key;
    return extension_id_for_public_key(directory.bytes());
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

ErrorOr<LoadedExtension> load_unpacked_extension(StringView directory_path, Optional<String> const& id_without_key)
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
    extension.id = TRY(extension_id_for(manifest, directory, id_without_key));
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

Optional<String> best_icon_path(JsonObject const& icons, int size)
{
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
}

Optional<JsonObject const&> action_of(LoadedExtension const& extension)
{
    if (auto action = extension.manifest.get_object("action"sv); action.has_value())
        return action;
    return extension.manifest.get_object("browser_action"sv);
}

static Optional<String> action_default_icon_path(LoadedExtension const& extension, int size)
{
    auto action = action_of(extension);
    if (!action.has_value())
        return {};
    if (auto icon = action->get_string("default_icon"sv); icon.has_value())
        return icon.copy();
    if (auto icons = action->get_object("default_icon"sv); icons.has_value())
        return best_icon_path(*icons, size);
    return {};
}

// https://developer.chrome.com/docs/extensions/reference/manifest/icons
Optional<String> icon_path(LoadedExtension const& extension, int size)
{
    if (auto icons = extension.manifest.get_object("icons"sv); icons.has_value()) {
        if (auto path = best_icon_path(*icons, size); path.has_value())
            return path;
    }
    // Without icons of its own, an extension is shown with its toolbar button's.
    return action_default_icon_path(extension, size);
}

Optional<String> action_icon_path(LoadedExtension const& extension, int size)
{
    if (auto path = action_default_icon_path(extension, size); path.has_value())
        return path;
    if (auto icons = extension.manifest.get_object("icons"sv); icons.has_value())
        return best_icon_path(*icons, size);
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

static bool is_match_pattern(StringView string)
{
    return string == "<all_urls>"sv || string.contains("://"sv);
}

bool extension_has_permission(LoadedExtension const& extension, StringView permission)
{
    return strings_of(extension.manifest, "permissions"sv).contains_slow(permission) || extension.granted_permissions.contains_slow(permission);
}

static ExtensionPermissions permissions_in(LoadedExtension const& extension, StringView permissions_key, StringView host_permissions_key)
{
    ExtensionPermissions result;
    for (auto& permission : strings_of(extension.manifest, permissions_key))
        (is_match_pattern(permission) ? result.origins : result.permissions).append(move(permission));
    result.origins.extend(strings_of(extension.manifest, host_permissions_key));
    return result;
}

bool match_pattern_covers(StringView held, StringView wanted)
{
    if (held == wanted || held == "<all_urls>"sv)
        return true;
    if (held == "*://*/*"sv)
        return wanted.starts_with("http://"sv) || wanted.starts_with("https://"sv) || wanted.starts_with("*://"sv);
    auto separator = wanted.find("://"sv);
    if (!separator.has_value() || wanted.substring_view(0, *separator).contains('*'))
        return false;
    auto host_and_path = wanted.substring_view(*separator + 3);
    if (host_and_path.starts_with('*'))
        return false;
    auto url = URL::Parser::basic_parse(MUST(MUST(String::from_utf8(wanted)).replace("*"sv, ""sv, ReplaceMode::All)));
    return url.has_value() && Web::Extensions::match_pattern_matches(MUST(String::from_utf8(held)), *url);
}

ExtensionPermissions required_permissions_of(LoadedExtension const& extension)
{
    return permissions_in(extension, "permissions"sv, "host_permissions"sv);
}

ExtensionPermissions optional_permissions_of(LoadedExtension const& extension)
{
    return permissions_in(extension, "optional_permissions"sv, "optional_host_permissions"sv);
}

// What the user is told an extension asks for, much as Chromium words its warnings.
// https://developer.chrome.com/docs/extensions/reference/permissions-list
String describe_extension_permission(StringView permission)
{
    if (permission == "history"sv)
        return "Read and change your browsing history"_string;
    if (permission == "bookmarks"sv)
        return "Read and change your bookmarks"_string;
    if (permission == "tabs"sv)
        return "Read your browsing history"_string;
    if (permission == "sessions"sv)
        return "Read your recently closed tabs"_string;
    if (permission == "webNavigation"sv)
        return "Read your browsing history"_string;
    if (permission == "notifications"sv)
        return "Display notifications"_string;
    if (permission == "clipboardRead"sv)
        return "Read data you copy and paste"_string;
    if (permission == "clipboardWrite"sv)
        return "Modify data you copy and paste"_string;
    return MUST(String::formatted("Use \"{}\"", permission));
}

String describe_extension_origin(StringView origin)
{
    if (origin == "<all_urls>"sv || origin == "*://*/*"sv || origin == "http://*/*"sv || origin == "https://*/*"sv)
        return "Read and change all your data on all websites"_string;
    auto host = origin;
    if (auto separator = host.find("://"sv); separator.has_value())
        host = host.substring_view(*separator + 3);
    host = host.substring_view(0, host.find('/').value_or(host.length()));
    if (host.starts_with("*."sv))
        return MUST(String::formatted("Read and change your data on all {} sites", host.substring_view(2)));
    return MUST(String::formatted("Read and change your data on {}", host));
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
    patterns.extend(extension.granted_origins);
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

bool is_web_accessible(LoadedExtension const& extension, StringView path, Optional<URL::Origin> const& requesting_origin)
{
    return Web::Extensions::is_web_accessible_resource(extension.manifest, path, requesting_origin);
}

ErrorOr<IPC::File> open_extension_file(LoadedExtension const& extension, StringView url_path, ExtensionFileRequester requester, Optional<URL::Origin> const& requesting_origin)
{
    // NB: A URL's path keeps the dot segments it spells with percent-encoded slashes, like "sub/..%2fsecret.html", so
    //     the path is resolved once decoded, before it is checked against the web-accessible resources or opened. It
    //     cannot rise above the extension's root.
    auto path = LexicalPath::canonicalized_path(ByteString::formatted("/{}", URL::percent_decode(url_path)));
    auto relative_path = path.view().trim("/"sv, TrimMode::Left);

    if (requester == ExtensionFileRequester::WebPage && !is_web_accessible(extension, relative_path, requesting_origin))
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

// https://developer.chrome.com/docs/extensions/reference/api/tabs#event-onMoved
Vector<TabMove> tab_moves_between(ReadonlySpan<u64> before, ReadonlySpan<u64> after, HashTable<u64> const& requested_moves)
{
    VERIFY(before.size() == after.size());
    Vector<u64> order { before };

    // The tabs that stayed put are the most that kept their order. The tabs an extension just asked to move are
    // taken to have moved, which also settles which of two neighbours moved when they swap places.
    Vector<u64> candidates;
    Vector<size_t> old_indices;
    for (auto view_id : after) {
        if (!requested_moves.contains(view_id)) {
            candidates.append(view_id);
            old_indices.append(order.find_first_index(view_id).value());
        }
    }
    // The longest run of candidates whose old places increase, by dynamic programming (there are only so many tabs).
    Vector<size_t> run_length;
    Vector<Optional<size_t>> previous_in_run;
    Optional<size_t> run_end;
    for (size_t i = 0; i < candidates.size(); ++i) {
        run_length.append(1);
        previous_in_run.append({});
        for (size_t j = 0; j < i; ++j) {
            if (old_indices[j] < old_indices[i] && run_length[j] + 1 > run_length[i]) {
                run_length[i] = run_length[j] + 1;
                previous_in_run[i] = j;
            }
        }
        if (!run_end.has_value() || run_length[i] > run_length[*run_end])
            run_end = i;
    }
    HashTable<u64> stayed;
    for (auto i = run_end; i.has_value(); i = previous_in_run[*i])
        stayed.set(candidates[*i]);

    // Each moved tab, from the first place on, goes right after the tab before it in the new order, as Chromium tells
    // of the moves of tabs.move with several tabs.
    Vector<TabMove> moves;
    for (size_t to_index = 0; to_index < after.size(); ++to_index) {
        auto view_id = after[to_index];
        if (stayed.contains(view_id))
            continue;
        auto from_index = order.find_first_index(view_id).value();
        order.remove(from_index);
        auto new_index = to_index == 0 ? 0 : order.find_first_index(after[to_index - 1]).value() + 1;
        order.insert(new_index, view_id);
        if (from_index != new_index)
            moves.append({ view_id, from_index, new_index });
    }
    return moves;
}

}
