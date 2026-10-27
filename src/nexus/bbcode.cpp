#include "nexus/bbcode.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace emf::nexus {

namespace {

constexpr size_t kMaxDepth = 32;
constexpr size_t kMaxRawScan = 4096;  // how far [url] [img] [youtube] look for their closing tag
constexpr size_t kMaxArg = 512;
constexpr size_t kMaxInput = 256 * 1024;  // longer descriptions are cut

struct Node {
    enum class Type { Text, Break, Elem } type = Type::Text;
    std::string text;  // Text: decoded; code: the code; img/youtube: the checked address or id; url: its own tag
    std::string tag;   // Elem: b i u s url img size color font center right left quote code spoiler list li youtube hr
    std::string arg;   // Elem: the value after '=' (decoded); img: "inline" or "link"
    bool block = false;   // shown as a block (a break next to it is dropped)
    bool closed = false;  // url: closed by its own [/url] (an unclosed one is text)
    std::vector<std::unique_ptr<Node>> kids;
};

char lower_c(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lower_c(c);
    return out;
}

void put_utf8(std::string& out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// HTML entities Nexus leaves in descriptions (&amp; &lt; &gt; &quot; &apos; &nbsp; &#N; &#xH;); others stay as text.
std::string decode_entities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const size_t semi = s.substr(0, i + 12).find(';', i + 1);  // entity names are short
        if (semi == std::string_view::npos) {
            out += '&';
            continue;
        }
        const std::string_view name = s.substr(i + 1, semi - i - 1);
        bool ok = true;
        if (name == "amp") out += '&';
        else if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (name == "nbsp") out += "\xC2\xA0";
        else if (name == "rsquo") out += "\xE2\x80\x99";
        else if (name == "lsquo") out += "\xE2\x80\x98";
        else if (name == "rdquo") out += "\xE2\x80\x9D";
        else if (name == "ldquo") out += "\xE2\x80\x9C";
        else if (name == "hellip") out += "\xE2\x80\xA6";
        else if (name == "ndash") out += "\xE2\x80\x93";
        else if (name == "mdash") out += "\xE2\x80\x94";
        else if (name == "copy") out += "\xC2\xA9";
        else if (name == "reg") out += "\xC2\xAE";
        else if (name == "trade") out += "\xE2\x84\xA2";
        else if (name.size() >= 2 && name[0] == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const std::string_view digits = name.substr(hex ? 2 : 1);
            uint32_t cp = 0;
            ok = !digits.empty();
            for (char c : digits) {
                int d = -1;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
                if (d < 0) {
                    ok = false;
                    break;
                }
                if (cp <= 0x10FFFF) cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(d);
            }
            if (ok) put_utf8(out, cp);
        } else {
            ok = false;
        }
        if (ok) i = semi;
        else out += '&';
    }
    return out;
}

void escape_into(std::string& out, std::string_view s, bool keep_newlines = false) {
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default:
                if (c == '\n' && keep_newlines) out += c;
                else if (c == '\t') out += c;
                else if (u >= 0x20 && u != 0x7F) out += c;
        }
    }
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

// An http(s) address (entities already decoded) safe to put in an attribute, or "".
std::string safe_url(std::string_view decoded) {
    std::string url(trim(decoded));
    if (url.size() > 2048) return "";
    for (char c : url) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' || c == '`') return "";
    }
    const std::string low = lower(url);
    if (low.rfind("www.", 0) == 0) return "https://" + url;
    if (low.rfind("https://", 0) == 0 && url.size() > 8) return url;
    if (low.rfind("http://", 0) == 0 && url.size() > 7) return url;
    return "";
}

// An https address on one of `hosts` (no user name in it; a port is allowed).
bool image_host_ok(const std::string& url, const std::vector<std::string>& hosts) {
    if (lower(url).rfind("https://", 0) != 0) return false;
    const size_t end = url.find_first_of("/?#", 8);
    const std::string authority = url.substr(8, end == std::string::npos ? std::string::npos : end - 8);
    if (authority.find('@') != std::string::npos) return false;
    const std::string host = lower(authority.substr(0, authority.find(':')));
    for (const auto& h : hosts)
        if (host == lower(h)) return true;
    return false;
}

bool youtube_id_ok(std::string_view id) {
    if (id.size() != 11) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    return true;
}

std::string youtube_id(std::string_view raw) {
    const std::string s(trim(decode_entities(raw)));
    if (youtube_id_ok(s)) return s;
    for (const char* key : {"youtube.com/watch?v=", "youtu.be/", "youtube.com/embed/"}) {
        const size_t at = s.find(key);
        if (at == std::string::npos) continue;
        const std::string id = s.substr(at + std::char_traits<char>::length(key), 11);
        if (youtube_id_ok(id)) return id;
    }
    return "";
}

const char* size_px(const std::string& arg) {
    static const char* px[] = {"10px", "13px", "16px", "18px", "24px", "32px", "48px"};
    if (arg.size() == 1 && arg[0] >= '1' && arg[0] <= '7') return px[arg[0] - '1'];
    return nullptr;
}

bool color_ok(const std::string& c) {
    if (c.empty() || c.size() > 20) return false;
    if (c[0] == '#') {
        if (c.size() != 4 && c.size() != 7) return false;
        for (size_t i = 1; i < c.size(); ++i)
            if (!((c[i] >= '0' && c[i] <= '9') || (lower_c(c[i]) >= 'a' && lower_c(c[i]) <= 'f'))) return false;
        return true;
    }
    for (char ch : c)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) return false;
    return true;
}

bool font_ok(const std::string& f) {
    if (f.empty() || f.size() > 40) return false;
    for (char c : f)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-'))
            return false;
    return true;
}

bool is_known(const std::string& t) {
    static const char* tags[] = {"b",     "i",    "u",      "s",     "url",  "img",     "size",  "color", "font",
                                 "center", "right", "left", "quote", "code", "spoiler", "list",  "*",     "youtube",
                                 "line"};
    for (const char* k : tags)
        if (t == k) return true;
    return false;
}

class Parser {
public:
    Parser(std::string_view s, const std::vector<std::string>& image_hosts)
        : s_(s), low_(lower(s)), image_hosts_(image_hosts) {
        stack_.push_back(&root_);
    }

    Node& run() {
        size_t i = 0;
        while (i < s_.size()) {
            const char c = s_[i];
            if (c == '\r') {
                ++i;
            } else if (c == '\n') {
                line_break(1);
                ++i;
            } else if (c == '<' && br_at(i)) {
                line_break(2);
                i = s_.find('>', i) + 1;
            } else if (c == '[' && tag_at(i)) {
                // tag_at moved i_ past what it read
                i = next_;
            } else {
                text_ += c;
                last_break_ = 0;
                ++i;
            }
        }
        flush();
        return root_;
    }

private:
    Node& top() { return *stack_.back(); }

    void flush() {
        if (text_.empty()) return;
        auto n = std::make_unique<Node>();
        n->type = Node::Type::Text;
        n->text = decode_entities(text_);
        text_.clear();
        top().kids.push_back(std::move(n));
    }
    Node& add(Node::Type type, std::string tag = {}) {
        flush();
        auto n = std::make_unique<Node>();
        n->type = type;
        n->tag = std::move(tag);
        Node& ref = *n;
        top().kids.push_back(std::move(n));
        return ref;
    }
    // Nexus writes "\n<br />" for one break: a newline and a <br> next to each other count once.
    void line_break(int source) {
        if (last_break_ != 0 && last_break_ != source) {
            last_break_ = 0;
            return;
        }
        add(Node::Type::Break);
        last_break_ = source;
    }
    // <br>, <br/>, <br /> (any case, spaces before '>')
    bool br_at(size_t i) const {
        if (low_.compare(i, 3, "<br") != 0) return false;
        size_t j = i + 3;
        while (j < s_.size() && s_[j] == ' ') ++j;
        if (j < s_.size() && s_[j] == '/') ++j;
        return j < s_.size() && s_[j] == '>';
    }

    // The raw text between position `from` and the closing tag `[/name]`, within `max` characters; sets next_.
    bool raw_until(size_t from, const std::string& name, size_t max, std::string_view& raw) {
        const std::string close = "[/" + name + "]";
        const std::string_view window =
            std::string_view(low_).substr(0, max == std::string::npos ? low_.size() : from + max + close.size());
        const size_t at = window.find(close, from);
        if (at == std::string::npos) return false;
        raw = s_.substr(from, at - from);
        next_ = at + close.size();
        return true;
    }

    void literal(size_t from, size_t to) {
        text_.append(s_.substr(from, to - from));
        last_break_ = 0;
    }

    bool tag_at(size_t i) {
        size_t j = i + 1;
        const bool closing = j < s_.size() && s_[j] == '/';
        if (closing) ++j;
        const size_t name_start = j;
        while (j < s_.size() && j - name_start < 10 &&
               ((low_[j] >= 'a' && low_[j] <= 'z') || (s_[j] >= '0' && s_[j] <= '9') || s_[j] == '*'))
            ++j;
        const std::string name = low_.substr(name_start, j - name_start);
        if (name.empty() || !is_known(name)) return false;
        std::string arg;
        bool has_arg = false;
        if (!closing && j < s_.size() && s_[j] == '=') {
            const size_t end = s_.substr(0, j + kMaxArg + 2).find(']', j);
            if (end == std::string::npos) return false;
            const std::string_view raw = s_.substr(j + 1, end - j - 1);
            if (raw.find('\n') != std::string_view::npos) return false;
            std::string_view v = trim(raw);
            if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
                v = v.substr(1, v.size() - 2);
            arg = decode_entities(v);
            has_arg = true;
            j = end;
        }
        if (j >= s_.size() || s_[j] != ']') return false;
        next_ = j + 1;
        last_break_ = 0;
        return closing ? close_tag(i, name) : open_tag(i, name, arg, has_arg);
    }

    bool open_tag(size_t at, const std::string& name, const std::string& arg, bool has_arg) {
        const size_t after = next_;
        flush();  // text so far belongs to the element that is open now
        if (name == "line") {
            add(Node::Type::Elem, "hr").block = true;
            return true;
        }
        if (name == "code") {
            std::string_view raw;
            if (!raw_until(after, "code", std::string::npos, raw)) {
                raw = s_.substr(after);
                next_ = s_.size();
            }
            // Nexus's "\n<br />" inside code: one newline
            std::string code;
            int last = 0;  // 1 = a newline was last, 2 = a <br>
            for (size_t k = 0; k < raw.size(); ++k) {
                if (raw[k] == '\r') continue;
                const size_t abs = static_cast<size_t>(raw.data() - s_.data()) + k;
                const bool br = raw[k] == '<' && br_at(abs);
                if (br || raw[k] == '\n') {
                    const int source = br ? 2 : 1;
                    if (last != 0 && last != source) {
                        last = 0;
                    } else {
                        code += '\n';
                        last = source;
                    }
                    if (br) k = raw.find('>', k);
                    continue;
                }
                code += raw[k];
                last = 0;
            }
            Node& n = add(Node::Type::Elem, "code");
            n.text = decode_entities(code);
            n.block = n.text.find('\n') != std::string::npos;
            return true;
        }
        if (name == "url" && !has_arg) {
            std::string_view raw;
            if (raw_until(after, name, kMaxRawScan, raw)) {
                const std::string url = safe_url(decode_entities(raw));
                if (!url.empty()) {
                    Node& a = add(Node::Type::Elem, "url");
                    a.arg = url;
                    a.closed = true;
                    auto t = std::make_unique<Node>();
                    t->text = decode_entities(raw);
                    a.kids.push_back(std::move(t));
                    return true;
                }
            }
            next_ = after;  // not an address: an element without one, its content read as usual (below)
        } else if (name == "img" || name == "youtube") {
            std::string_view raw;
            if (!raw_until(after, name, kMaxRawScan, raw)) {
                next_ = after;
                literal(at, after);
                return true;
            }
            if (name == "img") {
                const std::string url = safe_url(decode_entities(raw));
                if (!url.empty()) {
                    Node& img = add(Node::Type::Elem, "img");
                    img.text = url;
                    img.arg = image_host_ok(url, image_hosts_) ? "inline" : "link";
                }
            } else {
                const std::string id = youtube_id(raw);
                if (!id.empty()) add(Node::Type::Elem, "youtube").text = id;
            }
            return true;
        }
        if (name == "*") {
            if (top().tag == "li") stack_.pop_back();
            if (top().tag != "list") {
                text_ += "\xE2\x80\xA2 ";
                return true;
            }
        }
        if (stack_.size() > kMaxDepth) {
            literal(at, after);
            return true;
        }
        Node& n = add(Node::Type::Elem, name == "*" ? "li" : name);
        n.arg = arg;
        if (name == "url") n.text = std::string(s_.substr(at, after - at));  // shown as text if never closed
        n.block = name == "center" || name == "right" || name == "left" || name == "quote" || name == "spoiler" ||
                  name == "list";
        stack_.push_back(&n);
        return true;
    }

    bool close_tag(size_t at, const std::string& name) {
        flush();
        if (name == "*") {  // [/*]: optional end of a list item
            if (top().tag == "li") stack_.pop_back();
            return true;
        }
        for (size_t k = stack_.size(); k-- > 1;) {
            if (stack_[k]->tag == name) {
                stack_[k]->closed = true;  // the ones above it end here without their own closing tag
                stack_.resize(k);
                return true;
            }
        }
        literal(at, next_);
        return true;
    }

    std::string_view s_;
    std::string low_;
    const std::vector<std::string>& image_hosts_;
    Node root_;
    std::vector<Node*> stack_;
    std::string text_;
    int last_break_ = 0;  // 1 = the last thing was a newline, 2 = a <br>
    size_t next_ = 0;
};

bool is_break(const Node& n) { return n.type == Node::Type::Break; }
bool is_blank(const Node& n) {
    if (n.type != Node::Type::Text) return false;
    for (char c : n.text)
        if (c != ' ' && c != '\t') return false;
    return true;
}

// Drops the breaks Nexus puts around blocks and list items, which would show as empty lines.
void tidy(std::vector<std::unique_ptr<Node>>& kids, const std::string& parent) {
    std::vector<bool> drop(kids.size(), false);
    for (size_t k = 0; k < kids.size(); ++k) {
        if (parent == "list" && (is_break(*kids[k]) || is_blank(*kids[k]))) drop[k] = true;
        if (kids[k]->block) {
            if (k > 0 && is_break(*kids[k - 1])) drop[k - 1] = true;
            if (k + 1 < kids.size() && is_break(*kids[k + 1])) drop[k + 1] = true;
        }
    }
    const bool trim_ends = parent == "li" || parent == "center" || parent == "right" || parent == "left" ||
                           parent == "quote" || parent == "spoiler";
    if (trim_ends) {
        for (size_t k = 0; k < kids.size() && (is_break(*kids[k]) || is_blank(*kids[k])); ++k)
            if (is_break(*kids[k])) drop[k] = true;
        for (size_t k = kids.size(); k-- > 0 && (is_break(*kids[k]) || is_blank(*kids[k]));)
            if (is_break(*kids[k])) drop[k] = true;
    }
    std::vector<std::unique_ptr<Node>> kept;
    for (size_t k = 0; k < kids.size(); ++k)
        if (!drop[k]) kept.push_back(std::move(kids[k]));
    kids = std::move(kept);
}

const char* kLinkTail = "\" target=\"_blank\" rel=\"noopener noreferrer nofollow\">";

// `in_link`: inside an <a>, where another link would nest; it shows its text instead.
void render(Node& n, std::string& out, bool in_link);

void render_kids(Node& n, std::string& out, bool in_link) {
    tidy(n.kids, n.tag);
    for (auto& k : n.kids) render(*k, out, in_link);
}

void wrap(Node& n, std::string& out, const std::string& open, const char* close, bool in_link) {
    out += open;
    render_kids(n, out, in_link);
    out += close;
}

void link(std::string& out, const std::string& url, const char* text) {
    out += "<a href=\"";
    escape_into(out, url);
    out += kLinkTail;
    out += text;
    out += "</a>";
}

void render(Node& n, std::string& out, bool in_link) {
    if (n.type == Node::Type::Text) {
        escape_into(out, n.text);
        return;
    }
    if (n.type == Node::Type::Break) {
        out += "<br>";
        return;
    }
    const std::string& t = n.tag;
    if (t == "b" || t == "i" || t == "u" || t == "s") {
        wrap(n, out, "<" + t + ">", ("</" + t + ">").c_str(), in_link);
    } else if (t == "url") {
        if (!n.closed) {  // never closed: its tag is text, and it links nothing
            escape_into(out, n.text);
            return render_kids(n, out, in_link);
        }
        const std::string url = safe_url(n.arg);
        if (url.empty() || in_link) return render_kids(n, out, in_link);
        std::string open = "<a href=\"";
        escape_into(open, url);
        wrap(n, out, open + kLinkTail, "</a>", true);
    } else if (t == "img") {
        if (n.arg == "inline") {
            out += "<img src=\"";
            escape_into(out, n.text);
            out += "\" alt=\"\" loading=\"lazy\" referrerpolicy=\"no-referrer\">";
        } else if (in_link) {
            out += "image";
        } else {
            link(out, n.text, "image");  // from a host the page does not load images from
        }
    } else if (t == "youtube") {
        if (in_link) out += "YouTube video";
        else link(out, "https://www.youtube.com/watch?v=" + n.text, "YouTube video");
    } else if (t == "size") {
        const char* px = size_px(n.arg);
        if (!px) return render_kids(n, out, in_link);
        wrap(n, out, std::string("<span style=\"font-size:") + px + "\">", "</span>", in_link);
    } else if (t == "color") {
        if (!color_ok(n.arg)) return render_kids(n, out, in_link);
        wrap(n, out, "<span style=\"color:" + n.arg + "\">", "</span>", in_link);
    } else if (t == "font") {
        if (!font_ok(n.arg)) return render_kids(n, out, in_link);
        wrap(n, out, "<span style=\"font-family:'" + n.arg + "'\">", "</span>", in_link);
    } else if (t == "center" || t == "right" || t == "left") {
        wrap(n, out, "<div style=\"text-align:" + t + "\">", "</div>", in_link);
    } else if (t == "quote") {
        std::string open = "<blockquote class=\"bb-quote\">";
        if (!n.arg.empty()) {
            open += "<cite>";
            escape_into(open, n.arg);
            open += "</cite>";
        }
        wrap(n, out, open, "</blockquote>", in_link);
    } else if (t == "code") {
        if (n.block) {
            out += "<pre class=\"bb-code\"><code>";
            escape_into(out, n.text, true);
            out += "</code></pre>";
        } else {
            out += "<code class=\"bb-code\">";
            escape_into(out, n.text);
            out += "</code>";
        }
    } else if (t == "spoiler") {
        std::string open = "<details class=\"bb-spoiler\"><summary>";
        escape_into(open, n.arg.empty() ? std::string("Spoiler") : n.arg);
        wrap(n, out, open + "</summary>", "</details>", in_link);
    } else if (t == "list") {
        const std::string& a = n.arg;
        if (a.empty()) wrap(n, out, "<ul>", "</ul>", in_link);
        else if (a == "a" || a == "A" || a == "i" || a == "I") wrap(n, out, "<ol type=\"" + a + "\">", "</ol>", in_link);
        else wrap(n, out, "<ol>", "</ol>", in_link);
    } else if (t == "li") {
        wrap(n, out, "<li>", "</li>", in_link);
    } else if (t == "hr") {
        out += "<hr>";
    } else {
        render_kids(n, out, in_link);
    }
}

}  // namespace

const std::vector<std::string>& default_image_hosts() {
    static const std::vector<std::string> hosts = {"staticdelivery.nexusmods.com", "raw.githubusercontent.com",
                                                   "user-images.githubusercontent.com", "i.imgur.com"};
    return hosts;
}

std::string bbcode_to_html(std::string_view bbcode, const std::vector<std::string>& image_hosts) {
    const bool cut = bbcode.size() > kMaxInput;
    if (cut) {
        size_t n = kMaxInput;
        while (n > 0 && (static_cast<unsigned char>(bbcode[n]) & 0xC0) == 0x80) --n;  // not inside a character
        bbcode = bbcode.substr(0, n);
    }
    Parser p(bbcode, image_hosts);
    Node& root = p.run();
    std::string out;
    out.reserve(bbcode.size() + bbcode.size() / 4);
    render_kids(root, out, false);
    if (cut) out += "\xE2\x80\xA6";
    return out;
}

}  // namespace emf::nexus
