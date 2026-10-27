#pragma once
// Nexus mod descriptions (BBCode) to HTML for the app's page. Safe on its own, whatever the page does with it:
// - all text is escaped, so no HTML from the description gets through;
// - links must be http(s) (a bare "www." link gets https://); anything else (javascript:, data:, ...) loses its link
//   and keeps its text;
// - images are shown only from https addresses on `image_hosts`; any other image becomes a link with the text
//   "image", so the page never loads from a host it does not know;
// - tags EMF does not know stay as text ("[Chain]" in an ini example); an [url=...] that is never closed is text.
//
// Tags: [b] [i] [u] [s] [url] [url=...] [img] [size=1..7] [color=#hex or name] [font=name] [center] [right] [left]
// [quote] [quote=name] [code] [spoiler] [spoiler=title] [list] [list=1] [list=a] [*] [/*] [youtube] [line].
// Nexus stores descriptions with "\n<br />" for each line break and with HTML entities (&amp; &#92; &rsquo;); both
// are read. Only the first 256 KiB of a description is converted.
// Output: <b> <i> <u> <s> <a> <img> <span style> <div style> <blockquote class="bb-quote"> <cite> <code>/<pre>
// class="bb-code" <details class="bb-spoiler"> <summary> <ul> <ol> <li> <hr> <br>. Colours, sizes, fonts and
// alignment are inline style attributes with checked values (the page allows them with
// style-src-attr 'unsafe-inline'); links open in a new window with rel="noopener noreferrer nofollow"; YouTube videos
// are links, not embedded players. Nesting deeper than 32 tags stays text.
#include <string>
#include <string_view>
#include <vector>

namespace emf::nexus {

// staticdelivery.nexusmods.com, raw.githubusercontent.com, user-images.githubusercontent.com, i.imgur.com
const std::vector<std::string>& default_image_hosts();

// `image_hosts`: lower-case host names, matched exactly.
std::string bbcode_to_html(std::string_view bbcode,
                           const std::vector<std::string>& image_hosts = default_image_hosts());

}  // namespace emf::nexus
