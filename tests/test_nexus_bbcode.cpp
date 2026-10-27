// Nexus mod descriptions (BBCode) to HTML: every tag the descriptions use, escaping, link and image checks, and the
// five descriptions in tests/fixtures/nexus/descriptions.json. Their text is invented (it is not ours to publish); their
// BBCode follows what Nexus sent for those mods: tags closed on a later line, a link over a line break,
// lists with and without [/*], code blocks, spoilers, ini section names, entities and emoji.
#include <doctest.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "core/json.h"
#include "nexus/bbcode.h"

using emf::nexus::bbcode_to_html;

namespace {
const char* kLink = "\" target=\"_blank\" rel=\"noopener noreferrer nofollow\">";

size_t count(const std::string& s, const std::string& what) {
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size())) ++n;
    return n;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Every tag in `html` is one the converter may write, with allowed attributes and safe values; returns "" or why not.
std::string check_safe(const std::string& html) {
    static const std::set<std::string> tags = {"b",          "i",    "u",    "s",       "a",    "img", "span",
                                               "div",        "cite", "pre",  "code",    "ul",   "ol",  "li",
                                               "blockquote", "hr",   "br",   "details", "summary"};
    static const std::set<std::string> attrs = {"href", "src",  "style",   "class",          "target",
                                                "rel",  "alt",  "loading", "referrerpolicy", "type"};
    for (size_t i = html.find('<'); i != std::string::npos; i = html.find('<', i + 1)) {
        const size_t end = html.find('>', i);
        if (end == std::string::npos) return "an unclosed tag";
        std::string t = html.substr(i + 1, end - i - 1);
        if (!t.empty() && t[0] == '/') t.erase(0, 1);
        const size_t sp = t.find(' ');
        const std::string name = t.substr(0, sp);
        if (!tags.count(name)) return "tag <" + name + ">";
        if (sp == std::string::npos) continue;
        std::string rest = t.substr(sp + 1);
        while (!rest.empty()) {
            const size_t eq = rest.find("=\"");
            if (eq == std::string::npos) return "attribute without a quoted value in <" + t + ">";
            const std::string key = rest.substr(0, eq);
            const size_t close = rest.find('"', eq + 2);
            if (close == std::string::npos) return "unclosed attribute in <" + t + ">";
            const std::string value = lower(rest.substr(eq + 2, close - eq - 2));
            if (!attrs.count(key)) return "attribute " + key;
            if ((key == "href" || key == "src") && value.rfind("https://", 0) != 0 && value.rfind("http://", 0) != 0)
                return "link " + value;
            static const std::regex style(
                "font-size:[0-9]+px|color:(#[0-9a-f]{3}|#[0-9a-f]{6}|[a-z]+)|font-family:'[a-z0-9 -]+'|"
                "text-align:(center|left|right)");
            if (key == "style" && !std::regex_match(value, style)) return "style " + value;
            if (value.find('<') != std::string::npos || value.find('>') != std::string::npos) return "value " + value;
            rest = close + 1 < rest.size() ? rest.substr(close + 2) : "";
        }
    }
    return "";
}
}  // namespace

TEST_CASE("bbcode: bold, italic, underline, strike; tags ignore letter case") {
    CHECK(bbcode_to_html("[b]bold[/b] [i]it[/i] [u]u[/u] [s]s[/s]") == "<b>bold</b> <i>it</i> <u>u</u> <s>s</s>");
    CHECK(bbcode_to_html("[B]x[/b]") == "<b>x</b>");
}

TEST_CASE("bbcode: all text is escaped; no HTML gets through") {
    CHECK(bbcode_to_html("<script>alert(1)</script> & \"x\" 'y'") ==
          "&lt;script&gt;alert(1)&lt;/script&gt; &amp; &quot;x&quot; &#39;y&#39;");
    CHECK(bbcode_to_html("<img src=x onerror=alert(1)>") == "&lt;img src=x onerror=alert(1)&gt;");
    CHECK(bbcode_to_html("a\x01" "b\x7f" "c") == "abc");  // control characters are dropped
}

TEST_CASE("bbcode: line breaks, and Nexus's newline + <br /> pairs count once") {
    CHECK(bbcode_to_html("a\nb\r\nc") == "a<br>b<br>c");
    CHECK(bbcode_to_html("a\n<br />b") == "a<br>b");
    CHECK(bbcode_to_html("a<br />\nb") == "a<br>b");
    CHECK(bbcode_to_html("a\n<br />\n<br />b") == "a<br><br>b");
    CHECK(bbcode_to_html("a<br/><BR>b<br >c") == "a<br><br>b<br>c");
    CHECK(bbcode_to_html("a<brx>b") == "a&lt;brx&gt;b");
}

TEST_CASE("bbcode: HTML entities in Nexus's text are decoded, then escaped again") {
    CHECK(bbcode_to_html("&amp; &lt;b&gt; &#92; &#x41; &quot;") == "&amp; &lt;b&gt; \\ A &quot;");
    CHECK(bbcode_to_html("a&nbsp;b") == "a\xC2\xA0" "b");
    CHECK(bbcode_to_html("&bogus; &#0; &#xD800; &#99999999; &") == "&amp;bogus; \xEF\xBF\xBD \xEF\xBF\xBD \xEF\xBF\xBD &amp;");
    // the named entities descriptions use
    CHECK(bbcode_to_html("&rsquo;&lsquo;&rdquo;&ldquo;&hellip;&ndash;&mdash;&copy;&reg;&trade;") ==
          "\xE2\x80\x99\xE2\x80\x98\xE2\x80\x9D\xE2\x80\x9C\xE2\x80\xA6\xE2\x80\x93\xE2\x80\x94"
          "\xC2\xA9\xC2\xAE\xE2\x84\xA2");
    CHECK(bbcode_to_html("&#91;b&#93;x") == "[b]x");  // an escaped bracket is text, not a tag
    CHECK(bbcode_to_html("&lt;script&gt;") == "&lt;script&gt;");
}

TEST_CASE("bbcode: links") {
    CHECK(bbcode_to_html("[url=https://x.com/a?b=1&amp;c=2]link[/url]") ==
          std::string("<a href=\"https://x.com/a?b=1&amp;c=2") + kLink + "link</a>");
    CHECK(bbcode_to_html("[url]https://x.com[/url]") == std::string("<a href=\"https://x.com") + kLink +
                                                            "https://x.com</a>");
    CHECK(bbcode_to_html("[url=\"http://x.com\"][b]bold[/b][/url]") ==
          std::string("<a href=\"http://x.com") + kLink + "<b>bold</b></a>");
    CHECK(bbcode_to_html("[url=www.nexusmods.com/enshrouded]x[/url]") ==
          std::string("<a href=\"https://www.nexusmods.com/enshrouded") + kLink + "x</a>");
    // anything but http(s) loses the link and keeps the text
    for (const char* bad : {"[url=javascript:alert(1)]x[/url]", "[url=JaVaScRiPt:alert(1)]x[/url]",
                            "[url= javascript:alert(1)]x[/url]", "[url=data:text/html,hi]x[/url]",
                            "[url=//evil.com]x[/url]", "[url=vbscript:x]x[/url]", "[url=java&#115;cript:x]x[/url]",
                            "[url=https://x.com/a b]x[/url]", "[url=https://]x[/url]"})
        CHECK_MESSAGE(bbcode_to_html(bad) == "x", bad);
    CHECK(bbcode_to_html("[url]javascript:alert(1)[/url]") == "javascript:alert(1)");
    // a quote cannot end the attribute
    const std::string q = bbcode_to_html("[url=https://x.com/\"onmouseover=\"alert(1)]x[/url]");
    CHECK(q.find("\"onmouseover") == std::string::npos);
    CHECK(check_safe(q).empty());
    // an unclosed [url] is text
    CHECK(bbcode_to_html("[url]https://x.com") == "[url]https://x.com");
}

TEST_CASE("bbcode: images") {
    CHECK(bbcode_to_html("[img]https://i.imgur.com/a.png[/img]") ==
          "<img src=\"https://i.imgur.com/a.png\" alt=\"\" loading=\"lazy\" referrerpolicy=\"no-referrer\">");
    CHECK(bbcode_to_html("a[img]javascript:alert(1)[/img]b") == "ab");
    CHECK(bbcode_to_html("[img]data:image/png;base64,AAAA[/img]") == "");
    CHECK(bbcode_to_html("[img] https://i.imgur.com/a.png [/img]").find("src=\"https://i.imgur.com/a.png\"") !=
          std::string::npos);
}

TEST_CASE("bbcode: images only from known hosts over https; others become a link") {
    const std::string img = "<img src=\"https://staticdelivery.nexusmods.com/m/1.png\" alt=\"\" loading=\"lazy\" "
                            "referrerpolicy=\"no-referrer\">";
    CHECK(bbcode_to_html("[img]https://staticdelivery.nexusmods.com/m/1.png[/img]") == img);
    CHECK(bbcode_to_html("[img]https://STATICDELIVERY.nexusmods.com/m/1.png[/img]").find("<img ") == 0);
    CHECK(bbcode_to_html("[img]https://i.imgur.com:443/a.png[/img]").find("<img ") == 0);
    for (const char* other : {"[img]https://x.com/a.png[/img]", "[img]http://i.imgur.com/a.png[/img]",
                              "[img]https://i.imgur.com@evil.com/a.png[/img]",
                              "[img]https://i.imgur.com.evil.com/a.png[/img]", "[img]www.imgur.com/a.png[/img]"}) {
        const std::string out = bbcode_to_html(other);
        CHECK_MESSAGE(out.find("<img") == std::string::npos, other);
        CHECK_MESSAGE(out.rfind("<a href=\"http", 0) == 0, other);
        CHECK_MESSAGE(out.find(std::string(kLink) + "image</a>") != std::string::npos, other);
    }
    // the hosts are a parameter
    CHECK(bbcode_to_html("[img]https://x.com/a.png[/img]", {"x.com"}).find("<img src=\"https://x.com/a.png\"") == 0);
    CHECK(bbcode_to_html("[img]https://i.imgur.com/a.png[/img]", {}).find("<img") == std::string::npos);
    // inside a link an image link is just its text (no link inside a link)
    CHECK(bbcode_to_html("[url=https://n.com][img]https://x.com/a.png[/img][/url]") ==
          std::string("<a href=\"https://n.com") + kLink + "image</a>");
    CHECK(bbcode_to_html("[url=https://n.com][img]https://i.imgur.com/a.png[/img][/url]") ==
          std::string("<a href=\"https://n.com") + kLink +
              "<img src=\"https://i.imgur.com/a.png\" alt=\"\" loading=\"lazy\" referrerpolicy=\"no-referrer\"></a>");
    CHECK(bbcode_to_html("[url=https://n.com][youtube]V57iZcn-hfo[/youtube][/url]") ==
          std::string("<a href=\"https://n.com") + kLink + "YouTube video</a>");
}

TEST_CASE("bbcode: [url] without an address, and an [url=...] that is never closed") {
    // not an address: its content is read as usual (line breaks, images)
    CHECK(bbcode_to_html("[url]not a link\n<br />second line[/url]") == "not a link<br>second line");
    CHECK(bbcode_to_html("[url][img]https://i.imgur.com/a.png[/img][/url]") ==
          "<img src=\"https://i.imgur.com/a.png\" alt=\"\" loading=\"lazy\" referrerpolicy=\"no-referrer\">");
    // never closed: text, not a link over the rest of the description
    CHECK(bbcode_to_html("[url=https://x.com]text and the rest") == "[url=https://x.com]text and the rest");
    CHECK(bbcode_to_html("[url=https://x.com]a[b]b[/b]\nrest") == "[url=https://x.com]a<b>b</b><br>rest");
    CHECK(bbcode_to_html("[b][url=https://x.com]a[/b] rest") == "<b>[url=https://x.com]a</b> rest");
}

TEST_CASE("bbcode: only the first 256 KiB of a description is converted") {
    const std::string big(300 * 1024, 'a');
    const std::string out = bbcode_to_html(big);
    CHECK(out.size() <= 256 * 1024 + 8);
    CHECK(out.size() >= 256 * 1024 - 8);
    CHECK(out.substr(out.size() - 3) == "\xE2\x80\xA6");
}

TEST_CASE("bbcode: the test's safety check itself rejects unsafe output") {
    CHECK_FALSE(check_safe("<span style=\"color:red;background:url(x)\">x</span>").empty());
    CHECK_FALSE(check_safe("<span style=\"font-family:'a';x:y'\">x</span>").empty());
    CHECK_FALSE(check_safe("<a href=\"javascript:x\">x</a>").empty());
    CHECK_FALSE(check_safe("<script>").empty());
    CHECK(check_safe("<span style=\"color:#ff7700\">x</span><div style=\"text-align:center\"></div>").empty());
}

TEST_CASE("bbcode: size, colour, font and alignment, with checked values") {
    CHECK(bbcode_to_html("[size=4]x[/size]") == "<span style=\"font-size:18px\">x</span>");
    CHECK(bbcode_to_html("[size=1]x[/size][size=7]y[/size]") ==
          "<span style=\"font-size:10px\">x</span><span style=\"font-size:48px\">y</span>");
    CHECK(bbcode_to_html("[size=99]x[/size]") == "x");
    CHECK(bbcode_to_html("[color=#ff7700]x[/color]") == "<span style=\"color:#ff7700\">x</span>");
    CHECK(bbcode_to_html("[color=red]x[/color]") == "<span style=\"color:red\">x</span>");
    CHECK(bbcode_to_html("[color=red;background:url(x)]x[/color]") == "x");
    CHECK(bbcode_to_html("[color=#ff77001122]x[/color]") == "x");
    CHECK(bbcode_to_html("[font=Courier New]x[/font]") == "<span style=\"font-family:'Courier New'\">x</span>");
    CHECK(bbcode_to_html("[font=x'y]x[/font]") == "x");
    CHECK(bbcode_to_html("[center]x[/center][right]y[/right][left]z[/left]") ==
          "<div style=\"text-align:center\">x</div><div style=\"text-align:right\">y</div>"
          "<div style=\"text-align:left\">z</div>");
}

TEST_CASE("bbcode: quotes, code and spoilers") {
    CHECK(bbcode_to_html("[quote]x[/quote]") == "<blockquote class=\"bb-quote\">x</blockquote>");
    CHECK(bbcode_to_html("[quote=Bob <3]x[/quote]") ==
          "<blockquote class=\"bb-quote\"><cite>Bob &lt;3</cite>x</blockquote>");
    // no tags inside code; one line is inline code, more lines a block
    CHECK(bbcode_to_html("[code]a [b]b[/b] <i>[/code]") == "<code class=\"bb-code\">a [b]b[/b] &lt;i&gt;</code>");
    CHECK(bbcode_to_html("[code]a\n<br />b &#92; c[/code]") == "<pre class=\"bb-code\"><code>a\nb \\ c</code></pre>");
    CHECK(bbcode_to_html("[code]open to the end") == "<code class=\"bb-code\">open to the end</code>");
    CHECK(bbcode_to_html("[spoiler]x[/spoiler]") ==
          "<details class=\"bb-spoiler\"><summary>Spoiler</summary>x</details>");
    CHECK(bbcode_to_html("[spoiler=Ending]x[/spoiler]") ==
          "<details class=\"bb-spoiler\"><summary>Ending</summary>x</details>");
}

TEST_CASE("bbcode: lists, with Nexus's line breaks between items dropped") {
    CHECK(bbcode_to_html("[list]\n<br />[*]one[/*]\n<br />[*]two\n<br />[/list]") ==
          "<ul><li>one</li><li>two</li></ul>");
    CHECK(bbcode_to_html("[list=1][*]a[*]b[/list]") == "<ol><li>a</li><li>b</li></ol>");
    CHECK(bbcode_to_html("[list=a][*]a[/list]") == "<ol type=\"a\"><li>a</li></ol>");
    CHECK(bbcode_to_html("[list][*][b]a[/list]after") == "<ul><li><b>a</b></li></ul>after");
    CHECK(bbcode_to_html("[*]outside") == "\xE2\x80\xA2 outside");
    CHECK(bbcode_to_html("text\n<br />[list][*]a[/list]\n<br />more") == "text<ul><li>a</li></ul>more");
}

TEST_CASE("bbcode: YouTube videos are links; [line] is a rule") {
    const std::string link = std::string("<a href=\"https://www.youtube.com/watch?v=V57iZcn-hfo") + kLink +
                             "YouTube video</a>";
    CHECK(bbcode_to_html("[youtube]V57iZcn-hfo[/youtube]") == link);
    CHECK(bbcode_to_html("[youtube]https://www.youtube.com/watch?v=V57iZcn-hfo[/youtube]") == link);
    CHECK(bbcode_to_html("[youtube]https://youtu.be/V57iZcn-hfo[/youtube]") == link);
    CHECK(bbcode_to_html("[youtube]\"><script>[/youtube]") == "");
    CHECK(bbcode_to_html("a\n<br />[line]\n<br />b") == "a<hr>b");
}

TEST_CASE("bbcode: unknown tags and stray closing tags stay as text; open tags are closed at the end") {
    CHECK(bbcode_to_html("[Chain]\ncreative.dll") == "[Chain]<br>creative.dll");
    CHECK(bbcode_to_html("[heading]x[/heading]") == "[heading]x[/heading]");
    CHECK(bbcode_to_html("[b]x") == "<b>x</b>");
    CHECK(bbcode_to_html("x[/b]") == "x[/b]");
    CHECK(bbcode_to_html("[b][i]x[/b]y[/i]") == "<b><i>x</i></b>y[/i]");
    CHECK(bbcode_to_html("[b") == "[b");
    CHECK(bbcode_to_html("[") == "[");
    CHECK(bbcode_to_html("a [ b ] c [/] [=x]") == "a [ b ] c [/] [=x]");
    CHECK(bbcode_to_html("") == "");
}

TEST_CASE("bbcode: deep nesting and many unclosed tags stay cheap and bounded") {
    std::string deep;
    for (int i = 0; i < 1000; ++i) deep += "[b]";
    const std::string out = bbcode_to_html(deep + "x");
    CHECK(count(out, "<b>") == 32);
    CHECK(count(out, "</b>") == 32);
    CHECK(count(out, "[b]") == 1000 - 32);

    std::string many;
    for (int i = 0; i < 20000; ++i) many += "[img]";
    CHECK(bbcode_to_html(many).size() == many.size());  // all text, and quick
}

TEST_CASE("bbcode: descriptions shaped like the real ones convert to safe, balanced HTML") {
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/tests/fixtures/nexus/descriptions.json", std::ios::binary);
    REQUIRE(f.good());
    std::stringstream s;
    s << f.rdbuf();
    const auto doc = emf::core::json::parse(s.str());
    REQUIRE(doc.value.has_value());
    const auto& nodes = doc.value->get("data")->get("legacyModsByDomain")->get("nodes")->arr;
    REQUIRE(nodes.size() == 5);
    for (const auto& n : nodes) {
        const std::string name = n.get("name")->s;
        const std::string html = bbcode_to_html(n.get("description")->s);
        CHECK_MESSAGE(check_safe(html).empty(), name << ": " << check_safe(html));
        CHECK_MESSAGE(html.find("&lt;br") == std::string::npos, name);  // every <br /> became a break
        CHECK_MESSAGE(html.find("[b]") == std::string::npos, name);
        CHECK_MESSAGE(html.find("[/list]") == std::string::npos, name);
        for (const char* t : {"b", "i", "u", "a", "span", "div", "ul", "ol", "li", "blockquote", "pre", "code", "details"})
            CHECK_MESSAGE(count(html, std::string("<") + t + ">") + count(html, std::string("<") + t + " ") ==
                              count(html, std::string("</") + t + ">"),
                          name << " <" << t << ">");
        const int id = static_cast<int>(n.get("modId")->n);
        if (id == 147)  // ShroudForge: links to its GitHub page
            CHECK(html.find("<a href=\"https://github.com/bonsaibauer/shroudforge") != std::string::npos);
        if (id == 162)  // ModLoader Proxy: its ini example keeps "[Chain]"-style section names as text
            CHECK(html.find("[Load]") != std::string::npos);
        if (id == 170) {  // Auto Loot Plus: a code block with the folder layout, &#92; as backslashes
            CHECK(html.find("<pre class=\"bb-code\"><code>") != std::string::npos);
            CHECK(html.find("mods\\auto-loot-plus") != std::string::npos);
        }
        if (id == 86)  // Shroudtopia Manager: centred header image
            CHECK(html.find("<div style=\"text-align:center\"><img src=\"https://raw.githubusercontent.com/") !=
                  std::string::npos);
    }
}
