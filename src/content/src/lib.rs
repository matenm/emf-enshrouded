//! emf_content: EMF's C ABI around EML's mod loader.
//!
//! One call builds the content of a game folder's EML mods, the same two library calls `emm.exe run --patch` makes:
//! `ModEnvironment::load` and `mod_loader::lua::run`. The request and the reply are UTF-8 JSON; log lines go to a
//! callback. The ABI is in `emf_content.h`; README.md has the rules for loading the DLL.
//!
//! Nothing outlives a call: no panic leaves an export, no thread is started (and EML's image crate is built without
//! rayon, patch P3), the log subscriber and a panic hook are set only for the length of the call, and the reply goes
//! into the caller's buffer, so no memory crosses. Calls are one at a time.

use std::ffi::{CStr, c_char, c_int, c_void};
use std::io;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::Path;
use std::sync::{Arc, Mutex, TryLockError};
use std::time::Instant;

pub mod portraits;

use mod_loader::ModEnvironment;
use mod_loader::lua::{RunArgs, RunOptions};
use serde::{Deserialize, Serialize};

/// The version of this ABI (`emf_content_abi()`).
pub const ABI: u32 = 1;

pub const OK: i32 = 0;
pub const MODS_FAILED: i32 = 1;
/// emf_content_portraits: some items failed, the others were drawn or current (the same number as MODS_FAILED).
pub const ITEMS_FAILED: i32 = 1;
pub const RUN_FAILED: i32 = 2;
pub const BAD_REQUEST: i32 = 3;
pub const REPLY_TOO_SMALL: i32 = 4;
pub const PANIC: i32 = 5;

/// `void (*)(void* ctx, int level, const char* line)`: level 0 error, 1 warning, 2 info, 3 debug; one UTF-8 line
/// without a newline.
pub type LogFn = Option<unsafe extern "C" fn(ctx: *mut c_void, level: c_int, line: *const c_char)>;

/// The caller's log callback and its context.
#[derive(Clone, Copy)]
pub struct Logger {
    callback: LogFn,
    ctx: *mut c_void,
}

// The callback is only ever called on the thread that called emf_content_build, while that call runs; tracing's
// writer traits and the panic hook want Send + Sync all the same.
unsafe impl Send for Logger {}
unsafe impl Sync for Logger {}

impl Logger {
    pub fn new(callback: LogFn, ctx: *mut c_void) -> Self {
        Logger { callback, ctx }
    }

    pub fn none() -> Self {
        Logger { callback: None, ctx: std::ptr::null_mut() }
    }

    /// Sends `text` line by line, without control characters (tab stays): a mod's text cannot move a console's
    /// cursor or colour the log it lands in.
    fn send(&self, level: c_int, text: &str) {
        let Some(callback) = self.callback else { return };
        for line in text.split('\n') {
            let line: String = line.chars().filter(|&c| c == '\t' || (c as u32) >= 0x20).collect();
            if line.is_empty() {
                continue;
            }
            let mut bytes = line.into_bytes();
            bytes.push(0);
            // SAFETY: the caller promises the callback and its context are valid for the whole call.
            unsafe { callback(self.ctx, level, bytes.as_ptr() as *const c_char) };
        }
    }
}

fn level_number(level: &tracing::Level) -> c_int {
    match *level {
        tracing::Level::ERROR => 0,
        tracing::Level::WARN => 1,
        tracing::Level::INFO => 2,
        _ => 3,
    }
}

/// tracing's fmt layer makes one writer per event; it collects the formatted event and hands it to the callback,
/// line by line, when it is dropped.
struct LineWriter {
    logger: Logger,
    level: c_int,
    buf: Vec<u8>,
}

impl io::Write for LineWriter {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        self.buf.extend_from_slice(data);
        Ok(data.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl Drop for LineWriter {
    fn drop(&mut self) {
        let text = String::from_utf8_lossy(&self.buf);
        self.logger.send(self.level, &text);
    }
}

struct CallbackWriter(Logger);

impl<'a> tracing_subscriber::fmt::MakeWriter<'a> for CallbackWriter {
    type Writer = LineWriter;

    fn make_writer(&'a self) -> Self::Writer {
        LineWriter { logger: self.0, level: 2, buf: Vec::new() }
    }

    fn make_writer_for(&'a self, meta: &tracing::Metadata<'_>) -> Self::Writer {
        LineWriter { logger: self.0, level: level_number(meta.level()), buf: Vec::new() }
    }
}

#[derive(Deserialize)]
struct Request {
    game_dir: String,
    #[serde(default)]
    file_name: Option<String>,
    #[serde(default)]
    force: bool,
    #[serde(default)]
    server: Option<bool>,
    /// For tests only: the call panics with this message, so the PANIC path can be tested through the built DLL.
    #[serde(default)]
    test_panic: Option<String>,
}

#[derive(Serialize)]
struct ModInfo {
    id: String,
    version: String,
}

#[derive(Serialize)]
struct ModErrorInfo {
    id: Option<String>,
    path: String,
    error: String,
}

/// A warning or error a mod's Lua gave while its content was built (EML's `warn`, or an error logged in its span): the
/// build still worked, but the mod says something of it is missing (a creature pack that caught its own failure).
#[derive(Serialize, Clone, Debug)]
struct ModWarningInfo {
    id: String,
    warning: String,
}

/// At most this many warnings go into a reply; every one still goes to the log.
const MAX_MOD_WARNINGS: usize = 20;
/// A warning longer than this many characters is cut (the log has it whole).
const MAX_WARNING_CHARS: usize = 300;

#[derive(Serialize, Deserialize, Default)]
struct Reply {
    code: i32,
    message: String,
    /// Only in a reply cut to fit the caller's buffer.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    truncated: bool,
    #[serde(skip_deserializing)]
    mods: Vec<ModInfo>,
    #[serde(skip_deserializing)]
    mod_errors: Vec<ModErrorInfo>,
    /// Only when a mod gave one (a reply without warnings stays as before).
    #[serde(skip_deserializing, skip_serializing_if = "Vec::is_empty")]
    mod_warnings: Vec<ModWarningInfo>,
    #[serde(skip_deserializing)]
    dlls: Vec<String>,
    ms: u64,
}

impl Reply {
    fn failed(code: i32, message: impl Into<String>) -> Self {
        Reply { code, message: message.into(), ..Default::default() }
    }

    fn to_json(&self) -> String {
        // Plain strings and numbers only: serialising cannot fail.
        serde_json::to_string(self).unwrap_or_else(|_| format!("{{\"code\":{}}}", self.code))
    }
}

/// Which game files to build: the request's `file_name`, else `enshrouded` or `enshrouded_server` when its .kfc or
/// .exe is there, else the first .kfc or .exe in the folder, else `enshrouded`.
fn resolve_file_name(game_dir: &Path, requested: Option<String>) -> String {
    if let Some(name) = requested.filter(|n| !n.is_empty()) {
        return name;
    }
    let has = |stem: &str| ["kfc", "exe"].iter().any(|ext| game_dir.join(format!("{stem}.{ext}")).is_file());
    if let Some(stem) = ["enshrouded", "enshrouded_server"].into_iter().find(|s| has(s)) {
        return stem.to_string();
    }
    let first = std::fs::read_dir(game_dir).ok().and_then(|entries| {
        entries.flatten().map(|e| e.path()).find_map(|p| {
            let ext = p.extension()?.to_str()?.to_ascii_lowercase();
            if ext == "kfc" || ext == "exe" { p.file_stem()?.to_str().map(str::to_string) } else { None }
        })
    });
    first.unwrap_or_else(|| "enshrouded".to_string())
}

#[repr(C)]
#[derive(Default)]
struct ByHandleFileInformation {
    attributes: u32,
    creation_time: [u32; 2],
    last_access_time: [u32; 2],
    last_write_time: [u32; 2],
    volume_serial_number: u32,
    size_high: u32,
    size_low: u32,
    number_of_links: u32,
    index_high: u32,
    index_low: u32,
}

#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetFileInformationByHandle(file: *mut c_void, info: *mut ByHandleFileInformation) -> i32;
}

/// How many names the file at `path` has (more than 1: a hard link), or None when it cannot be opened.
fn link_count(path: &Path) -> Option<u32> {
    use std::os::windows::io::AsRawHandle;
    let file = std::fs::File::open(path).ok()?;
    let mut info = ByHandleFileInformation::default();
    // SAFETY: a valid open handle and a struct laid out as BY_HANDLE_FILE_INFORMATION.
    let ok = unsafe { GetFileInformationByHandle(file.as_raw_handle() as *mut c_void, &mut info) };
    (ok != 0).then_some(info.number_of_links)
}

fn sorted_mods(env: &ModEnvironment) -> Vec<ModInfo> {
    let mut mods: Vec<ModInfo> = env
        .mod_registry()
        .values()
        .map(|m| ModInfo { id: m.info().id.clone(), version: m.info().version.to_string() })
        .collect();
    mods.sort_by(|a, b| a.id.cmp(&b.id));
    mods
}

/// The build itself, inside the call's log subscriber.
fn build(request: Request) -> Reply {
    if let Some(message) = request.test_panic {
        panic!("{message}");
    }
    let game_dir = Path::new(&request.game_dir);
    tracing::info!(target: "emf::content", "Building the content of the EML mods in {}", request.game_dir);

    if !game_dir.is_dir() {
        let message = format!("The game folder does not exist or is not a folder: {}", request.game_dir);
        tracing::error!(target: "emf::content", "{message}");
        return Reply::failed(RUN_FAILED, message);
    }
    let file_name = resolve_file_name(game_dir, request.file_name);
    if !game_dir.join(format!("{file_name}.kfc")).is_file() && !game_dir.join(format!("{file_name}.exe")).is_file() {
        let message = format!("No {file_name}.kfc or {file_name}.exe in {}", request.game_dir);
        tracing::error!(target: "emf::content", "{message}");
        return Reply::failed(RUN_FAILED, message);
    }
    // EML writes these three in place: through a hard link it would change the linked file (the game's own) too.
    for ext in ["kfc", "kfc_resources", "kfc.bak"] {
        let name = format!("{file_name}.{ext}");
        if let Some(links) = link_count(&game_dir.join(&name)).filter(|&n| n > 1) {
            let message = format!(
                "{name} in {} is a hard link ({links} names): EML writes it in place and would change the linked \
                 file too. It must be a copy of its own.",
                request.game_dir
            );
            tracing::error!(target: "emf::content", "{message}");
            return Reply::failed(RUN_FAILED, message);
        }
    }

    let env = match ModEnvironment::load(request.game_dir.as_str()) {
        Ok(env) => env,
        Err(report) => {
            if let Some(error) = report.error {
                let message = format!("Could not read the mods: {error}");
                tracing::error!(target: "emf::content", "{message}");
                return Reply::failed(RUN_FAILED, message);
            }
            let mut reply = Reply::failed(MODS_FAILED, "Some EML mods could not be loaded");
            for e in report.mods {
                tracing::error!(target: "emf::content", "In mod {} at {}: {}",
                    e.id.as_deref().unwrap_or("(no id)"), e.path, e.error);
                reply.mod_errors.push(ModErrorInfo { id: e.id, path: e.path.to_string(), error: e.error.to_string() });
            }
            return reply;
        }
    };

    let mods = sorted_mods(&env);
    let args = RunArgs {
        file_name,
        options: RunOptions {
            skip_cache: request.force,
            is_server: request.server,
            patch: true,
            export: false,
            runtime: false,
            export_dir: None,
        },
    };
    match mod_loader::lua::run(&env, args) {
        Ok(result) => Reply {
            code: OK,
            message: format!("The content of {} EML mods is up to date", mods.len()),
            mods,
            dlls: result.dlls.iter().map(|p| p.to_string_lossy().into_owned()).collect(),
            ..Default::default()
        },
        Err(e) => {
            let message = format!("{e:#}");
            tracing::error!(target: "emf::content", "The build failed: {message}");
            Reply { code: RUN_FAILED, message, mods, ..Default::default() }
        }
    }
}

/// The `mod_id` a span was opened with (EML opens a `lua` span with the mod's id around each line a mod's Lua logs).
struct ModId(String);

/// One field's text from a span's or an event's fields.
struct FieldText {
    name: &'static str,
    value: Option<String>,
}

impl tracing::field::Visit for FieldText {
    fn record_str(&mut self, field: &tracing::field::Field, value: &str) {
        if field.name() == self.name {
            self.value = Some(value.to_string());
        }
    }

    fn record_debug(&mut self, field: &tracing::field::Field, value: &dyn std::fmt::Debug) {
        if field.name() == self.name && self.value.is_none() {
            self.value = Some(format!("{value:?}"));
        }
    }
}

/// A warning's text for the reply: tabs and line breaks become single spaces, cut to MAX_WARNING_CHARS characters.
fn warning_text(raw: &str) -> String {
    let mut out = String::new();
    let mut space = false;
    for c in raw.trim().chars() {
        if c == '\t' || c == '\r' || c == '\n' {
            if !space {
                out.push(' ');
            }
            space = true;
        } else {
            out.push(c);
            space = false;
        }
    }
    match out.char_indices().nth(MAX_WARNING_CHARS) {
        Some((at, _)) => out[..at].to_string(),
        None => out,
    }
}

/// Collects the warnings and errors logged inside a span that carries a `mod_id`, for the reply.
struct ModWarningLayer(Arc<Mutex<Vec<ModWarningInfo>>>);

impl<S> tracing_subscriber::Layer<S> for ModWarningLayer
where
    S: tracing::Subscriber + for<'a> tracing_subscriber::registry::LookupSpan<'a>,
{
    fn on_new_span(
        &self,
        attrs: &tracing::span::Attributes<'_>,
        id: &tracing::span::Id,
        ctx: tracing_subscriber::layer::Context<'_, S>,
    ) {
        let mut mod_id = FieldText { name: "mod_id", value: None };
        attrs.record(&mut mod_id);
        if let (Some(mod_id), Some(span)) = (mod_id.value, ctx.span(id)) {
            span.extensions_mut().insert(ModId(mod_id));
        }
    }

    fn on_event(&self, event: &tracing::Event<'_>, ctx: tracing_subscriber::layer::Context<'_, S>) {
        if *event.metadata().level() > tracing::Level::WARN {
            return;  // information and below: not a warning
        }
        let Some(scope) = ctx.event_scope(event) else { return };
        let Some(id) = scope.into_iter().find_map(|span| span.extensions().get::<ModId>().map(|m| m.0.clone())) else {
            return;  // not inside a mod's span: EML's or EMF's own line
        };
        let mut message = FieldText { name: "message", value: None };
        event.record(&mut message);
        let mut list = self.0.lock().unwrap_or_else(|e| e.into_inner());
        if list.len() < MAX_MOD_WARNINGS {
            let mut warning = warning_text(&message.value.unwrap_or_default());
            // a mod's own "<id>:" in front of its line (creature packs log so): the id is in `id` already
            if let Some(rest) = warning.strip_prefix(&format!("{id}:")) {
                warning = rest.trim_start().to_string();
            }
            list.push(ModWarningInfo { id, warning });
        }
    }
}

/// Runs `f` with the caller's log callback as the tracing subscriber (none when there is no callback), and gives the
/// warnings mods logged meanwhile.
fn with_log_collecting<R>(logger: Logger, f: impl FnOnce() -> R) -> (R, Vec<ModWarningInfo>) {
    use tracing_subscriber::layer::SubscriberExt;
    let warnings = Arc::new(Mutex::new(Vec::new()));
    let layer = ModWarningLayer(warnings.clone());
    let result = if logger.callback.is_some() {
        let subscriber = tracing_subscriber::fmt()
            .with_ansi(false)
            .without_time()
            .with_level(false)
            .with_target(true)
            .with_max_level(tracing::Level::DEBUG)
            .with_writer(CallbackWriter(logger))
            .finish()
            .with(layer);
        tracing::subscriber::with_default(subscriber, f)
    } else {
        tracing::subscriber::with_default(tracing_subscriber::registry().with(layer), f)
    };
    let list = std::mem::take(&mut *warnings.lock().unwrap_or_else(|e| e.into_inner()));
    (result, list)
}

/// Runs `f` with the caller's log callback as the tracing subscriber (none when there is no callback).
fn with_log<R>(logger: Logger, f: impl FnOnce() -> R) -> R {
    with_log_collecting(logger, f).0
}

/// Parses the request, runs the build with the caller's log callback, and gives the code and the reply JSON.
pub fn build_json(request_json: &str, logger: Logger) -> (i32, String) {
    let started = Instant::now();
    let mut reply = match serde_json::from_str::<Request>(request_json) {
        Err(e) => Reply::failed(BAD_REQUEST, format!("The request is not valid JSON for emf_content: {e}")),
        Ok(request) if request.game_dir.is_empty() => Reply::failed(BAD_REQUEST, "The request's game_dir is empty"),
        Ok(request) => {
            let (mut reply, warnings) = with_log_collecting(logger, || build(request));
            reply.mod_warnings = warnings;
            reply
        }
    };
    reply.ms = started.elapsed().as_millis() as u64;
    (reply.code, reply.to_json())
}

/// Parses a portraits request, draws what is not current with the caller's log callback, and gives the code and the
/// reply JSON (portraits/mod.rs).
pub fn portraits_json(request_json: &str, logger: Logger) -> (i32, String) {
    let started = Instant::now();
    let mut reply = match serde_json::from_str::<portraits::Request>(request_json) {
        Err(e) => portraits::Reply::failed(
            BAD_REQUEST,
            format!("The request is not valid JSON for emf_content_portraits: {e}"),
        ),
        Ok(request) => with_log(logger, || {
            let file_name = resolve_file_name(Path::new(&request.game_dir), request.file_name.clone());
            portraits::run(request, &file_name, portraits::game::Archive::open)
        }),
    };
    reply.ms = started.elapsed().as_millis() as u64;
    (reply.code, reply.to_json())
}

fn panic_text(payload: &(dyn std::any::Any + Send)) -> String {
    payload
        .downcast_ref::<&str>()
        .map(|s| s.to_string())
        .or_else(|| payload.downcast_ref::<String>().cloned())
        .unwrap_or_else(|| "(no message)".to_string())
}

/// Runs `f` and turns a panic into a PANIC reply that carries the panic's message.
pub fn guarded<F: FnOnce() -> (i32, String)>(f: F) -> (i32, String) {
    guarded_with(f, BUILD.failed)
}

/// Runs `f` and turns a panic into a PANIC reply made by `failed`, carrying the panic's message.
fn guarded_with<F: FnOnce() -> (i32, String)>(f: F, failed: fn(i32, String) -> String) -> (i32, String) {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(result) => result,
        Err(payload) => {
            let text = panic_text(payload.as_ref());
            (PANIC, failed(PANIC, format!("emf_content panicked: {text}")))
        }
    }
}

/// The panic hook for the length of one call: the message and its place go to the caller's log (not to stderr, and
/// with no backtrace to symbolise).
fn log_panic(logger: Logger, info: &std::panic::PanicHookInfo<'_>) {
    let place = info.location().map(|l| format!(" at {}:{}:{}", l.file(), l.line(), l.column())).unwrap_or_default();
    logger.send(0, &format!("emf::content: panicked{place}: {}", panic_text(info.payload())));
}

/// The longest reply `render(message cut)` that fits `cap` bytes with its NUL, cutting the message at a character
/// boundary. None when even an empty message does not fit.
fn cut_to_fit(mut message: String, cap: usize, render: impl Fn(&str) -> String) -> Option<String> {
    loop {
        let text = render(&message);
        if text.len() < cap {
            return Some(text);
        }
        if message.is_empty() {
            return None;
        }
        let over = text.len() + 1 - cap;
        let mut keep = message.len().saturating_sub(over);
        while !message.is_char_boundary(keep) {
            keep -= 1;
        }
        message.truncate(keep);
    }
}

/// A build reply that fits `cap` bytes with its NUL: the real code and ms, `"truncated": true`, no lists, and as
/// much of the message as fits. None when even an empty message does not fit.
fn cut_reply(json: &str, cap: usize) -> Option<String> {
    let full: Reply = serde_json::from_str(json).ok()?;
    cut_to_fit(full.message, cap, |message| {
        Reply { code: full.code, message: message.to_string(), truncated: true, ms: full.ms, ..Default::default() }
            .to_json()
    })
}

/// A portraits reply cut the same way: the real code and ms, `"truncated": true`, no items.
fn cut_portraits_reply(json: &str, cap: usize) -> Option<String> {
    let full: portraits::Reply = serde_json::from_str(json).ok()?;
    cut_to_fit(full.message, cap, |message| {
        let cut = portraits::Reply { code: full.code, message: message.to_string(), truncated: true, ms: full.ms,
            ..Default::default() };
        cut.to_json()
    })
}

/// Copies the reply and a NUL into the caller's buffer and gives the call's result. `*reply_len` always gets the
/// full reply's length with the NUL. When the full reply does not fit, a cut one (`cut`) goes in and the call
/// still gives `code`; when not even that fits (or `reply` is NULL), nothing is written: REPLY_TOO_SMALL.
///
/// # Safety
/// `reply` is NULL or points to `reply_cap` writable bytes; `reply_len` points to a writable size_t.
unsafe fn write_reply(
    json: &str,
    code: i32,
    reply: *mut c_char,
    reply_cap: usize,
    reply_len: *mut usize,
    cut: fn(&str, usize) -> Option<String>,
) -> i32 {
    unsafe { *reply_len = json.len() + 1 };
    if reply.is_null() {
        return REPLY_TOO_SMALL;
    }
    let text = if json.len() < reply_cap {
        json.to_string()
    } else {
        match cut(json, reply_cap) {
            Some(cut) => cut,
            None => return REPLY_TOO_SMALL,
        }
    };
    unsafe {
        std::ptr::copy_nonoverlapping(text.as_ptr(), reply as *mut u8, text.len());
        *reply.add(text.len()) = 0;
    }
    code
}

/// One call at a time, over both exports: a call swaps the process's panic hook, and EML's state is not made for two
/// runs at once.
static CALL: Mutex<()> = Mutex::new(());

/// What differs between the exports: the work, and the shape of a failed or cut reply.
struct Export {
    run: fn(&str, Logger) -> (i32, String),
    failed: fn(i32, String) -> String,
    cut: fn(&str, usize) -> Option<String>,
}

const BUILD: Export = Export {
    run: build_json,
    failed: |code, message| Reply::failed(code, message).to_json(),
    cut: cut_reply,
};

const PORTRAITS: Export = Export {
    run: portraits_json,
    failed: |code, message| portraits::Reply::failed(code, message).to_json(),
    cut: cut_portraits_reply,
};

/// The body of both exports: one call at a time, the caller's log and a panic hook for the length of the call, no
/// panic out, the reply into the caller's buffer.
///
/// # Safety
/// As for the exports.
unsafe fn export_call(
    export: &Export,
    request_json: *const c_char,
    log: LogFn,
    log_ctx: *mut c_void,
    reply: *mut c_char,
    reply_cap: usize,
    reply_len: *mut usize,
) -> i32 {
    if reply_len.is_null() {
        return BAD_REQUEST;
    }
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        let turn = match CALL.try_lock() {
            Ok(guard) => Some(guard),
            Err(TryLockError::Poisoned(poisoned)) => Some(poisoned.into_inner()),
            Err(TryLockError::WouldBlock) => None,
        };
        let (code, json) = match turn {
            None => {
                let message = "emf_content is already running a call (emf_content_build or emf_content_portraits): \
                               one call at a time, never from the log callback";
                (BAD_REQUEST, (export.failed)(BAD_REQUEST, message.to_string()))
            }
            Some(_turn) => {
                let logger = Logger::new(log, log_ctx);
                let previous = std::panic::take_hook();
                std::panic::set_hook(Box::new(move |info| log_panic(logger, info)));
                let run = || {
                    if request_json.is_null() {
                        return (BAD_REQUEST, (export.failed)(BAD_REQUEST, "The request is NULL".to_string()));
                    }
                    match unsafe { CStr::from_ptr(request_json) }.to_str() {
                        Ok(text) => (export.run)(text, logger),
                        Err(e) => {
                            let message = format!("The request is not UTF-8: {e}");
                            (BAD_REQUEST, (export.failed)(BAD_REQUEST, message))
                        }
                    }
                };
                let result = guarded_with(run, export.failed);
                drop(std::panic::take_hook());
                std::panic::set_hook(previous);
                result
            }
        };
        unsafe { write_reply(&json, code, reply, reply_cap, reply_len, export.cut) }
    }));
    outcome.unwrap_or(PANIC)
}

/// The ABI version: 1.
#[unsafe(no_mangle)]
pub extern "C" fn emf_content_abi() -> u32 {
    ABI
}

/// Builds the content of a game folder's EML mods. See emf_content.h.
///
/// # Safety
/// `request_json` is NULL or a NUL-terminated string; `log` is NULL or a function that may be called with `log_ctx`
/// during the call; `reply` is NULL or points to `reply_cap` writable bytes; `reply_len` points to a size_t.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emf_content_build(
    request_json: *const c_char,
    log: LogFn,
    log_ctx: *mut c_void,
    reply: *mut c_char,
    reply_cap: usize,
    reply_len: *mut usize,
) -> i32 {
    unsafe { export_call(&BUILD, request_json, log, log_ctx, reply, reply_cap, reply_len) }
}

/// Draws portraits of game models into a cache folder, from the game's own files. See emf_content.h.
///
/// # Safety
/// As for emf_content_build.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn emf_content_portraits(
    request_json: *const c_char,
    log: LogFn,
    log_ctx: *mut c_void,
    reply: *mut c_char,
    reply_cap: usize,
    reply_len: *mut usize,
) -> i32 {
    unsafe { export_call(&PORTRAITS, request_json, log, log_ctx, reply, reply_cap, reply_len) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::{CStr, c_char, c_int, c_void};
    use std::path::PathBuf;

    /// A folder in %TEMP% that is removed again when the test ends, also when it fails.
    struct TempDir(PathBuf);

    impl TempDir {
        fn new(name: &str) -> Self {
            let unique = format!(
                "emf_content_{name}_{}_{}",
                std::process::id(),
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap()
                    .as_nanos()
            );
            let path = std::env::temp_dir().join(unique);
            std::fs::create_dir_all(&path).unwrap();
            TempDir(path)
        }

        fn utf8(&self) -> &str {
            self.0.to_str().unwrap()
        }
    }

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn request_for(dir: &str) -> String {
        serde_json::json!({ "game_dir": dir }).to_string()
    }

    fn parse(reply: &str) -> serde_json::Value {
        serde_json::from_str(reply).expect("the reply is JSON")
    }

    unsafe extern "C" fn collect(ctx: *mut c_void, level: c_int, line: *const c_char) {
        let lines = unsafe { &mut *(ctx as *mut Vec<(i32, String)>) };
        let line = unsafe { CStr::from_ptr(line) }.to_string_lossy().into_owned();
        lines.push((level, line));
    }

    /// Calls the exported function the way C does, with a reply buffer of `cap` bytes.
    /// The export takes one call at a time and swaps the process's panic hook for its length, so the tests that call
    /// it, or panic, take turns.
    static EXPORT_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    fn export_turn() -> std::sync::MutexGuard<'static, ()> {
        EXPORT_LOCK.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn call_export_logged(
        request: &str,
        cap: usize,
        fill: u8,
        log: Option<&mut Vec<(i32, String)>>,
    ) -> (i32, Vec<u8>, usize) {
        let request = std::ffi::CString::new(request).unwrap();
        let mut buf = vec![fill; cap];
        let mut len = 0usize;
        let ptr = if cap == 0 { std::ptr::null_mut() } else { buf.as_mut_ptr() as *mut c_char };
        let (callback, ctx): (LogFn, *mut c_void) = match log {
            Some(lines) => (Some(collect), lines as *mut _ as *mut c_void),
            None => (None, std::ptr::null_mut()),
        };
        let code = unsafe { emf_content_build(request.as_ptr(), callback, ctx, ptr, cap, &mut len) };
        (code, buf, len)
    }

    fn call_export(request: &str, cap: usize, fill: u8) -> (i32, Vec<u8>, usize) {
        let _turn = export_turn();
        call_export_logged(request, cap, fill, None)
    }

    fn reply_text(buf: &[u8], len: usize) -> &str {
        assert_eq!(buf[len - 1], 0, "the reply ends with a NUL");
        std::str::from_utf8(&buf[..len - 1]).unwrap()
    }

    #[test]
    fn bad_json_is_bad_request() {
        let (code, reply) = build_json("{", Logger::none());
        assert_eq!(code, BAD_REQUEST);
        let reply = parse(&reply);
        assert_eq!(reply["code"], BAD_REQUEST);
        let message = reply["message"].as_str().unwrap();
        assert!(message.contains("JSON"), "{message}");
        assert!(message.contains("EOF"), "the message names serde's error: {message}");
    }

    #[test]
    fn missing_game_dir_is_bad_request() {
        let (code, reply) = build_json("{}", Logger::none());
        assert_eq!(code, BAD_REQUEST);
        assert!(parse(&reply)["message"].as_str().unwrap().contains("game_dir"));
    }

    #[test]
    fn folder_without_kfc_is_run_failed() {
        let dir = TempDir::new("empty");
        let (code, reply) = build_json(&request_for(dir.utf8()), Logger::none());
        assert_ne!(code, OK);
        assert!(code == MODS_FAILED || code == RUN_FAILED, "code {code}");
        let reply = parse(&reply);
        assert_eq!(reply["code"], code);
        let message = reply["message"].as_str().unwrap();
        assert!(message.contains(dir.utf8()), "the message names the folder: {message}");
        assert_eq!(reply["mods"].as_array().unwrap().len(), 0);
        assert!(reply["mod_errors"].as_array().unwrap().is_empty());
        assert!(reply["dlls"].as_array().unwrap().is_empty());
        assert!(reply["ms"].is_u64());
    }

    #[test]
    fn reply_too_small_reports_needed_length() {
        let dir = TempDir::new("small");
        let (code, buf, len) = call_export(&request_for(dir.utf8()), 4, b'#');
        assert_eq!(code, REPLY_TOO_SMALL);
        assert!(len > 4, "needed length {len}");
        assert_eq!(buf, b"####".to_vec(), "nothing is written into too small a buffer");
    }

    #[test]
    fn retry_with_needed_length_succeeds() {
        let dir = TempDir::new("retry");
        let request = request_for(dir.utf8());
        let (big_code, _, _) = call_export(&request, 64 * 1024, 0);
        let (code, _, needed) = call_export(&request, 0, 0);
        assert_eq!(code, REPLY_TOO_SMALL);
        let (code, buf, len) = call_export(&request, needed, b'#');
        assert_eq!(code, big_code);
        assert_eq!(len, needed);
        assert_eq!(buf[needed - 1], 0, "the reply ends with a NUL");
        let text = std::str::from_utf8(&buf[..needed - 1]).unwrap();
        assert_eq!(parse(text)["code"], big_code);
    }

    #[test]
    fn panic_is_caught() {
        let _turn = export_turn();
        let (code, reply) = guarded(|| panic!("x marks the panic"));
        assert_eq!(code, PANIC);
        let reply = parse(&reply);
        assert_eq!(reply["code"], PANIC);
        assert!(reply["message"].as_str().unwrap().contains("x marks the panic"));
        let (code, _) = build_json("{}", Logger::none());
        assert_eq!(code, BAD_REQUEST, "the next call works");
    }

    #[test]
    fn log_lines_reach_the_callback() {
        let dir = TempDir::new("log");
        let mut lines: Vec<(i32, String)> = Vec::new();
        let logger = Logger::new(Some(collect), &mut lines as *mut _ as *mut c_void);
        let _ = build_json(&request_for(dir.utf8()), logger);
        assert!(!lines.is_empty());
        let info = lines.iter().find(|(level, _)| *level == 2).expect("an info line");
        assert!(info.1.contains(dir.utf8()), "{}", info.1);
        for (_, line) in &lines {
            assert!(!line.contains('\n') && !line.contains('\r'), "{line:?}");
        }
    }

    #[test]
    fn eml_runs_and_its_errors_reach_the_callback() {
        // Not a game: an exe EML cannot read its types from. EML's own code runs (it makes mods\ and .cache\)
        // and fails.
        let dir = TempDir::new("fake_game");
        std::fs::write(dir.0.join("enshrouded.exe"), b"MZ but not a game").unwrap();
        let mut lines: Vec<(i32, String)> = Vec::new();
        let logger = Logger::new(Some(collect), &mut lines as *mut _ as *mut c_void);
        let (code, reply) = build_json(&request_for(dir.utf8()), logger);
        assert_eq!(code, RUN_FAILED);
        assert!(dir.0.join("mods").is_dir(), "EML's ModEnvironment::load ran");
        let reply = parse(&reply);
        assert!(!reply["message"].as_str().unwrap().is_empty());
        assert!(reply["mods"].as_array().unwrap().is_empty());
        let eml_error = lines.iter().find(|(level, line)| *level == 0 && line.starts_with("eml::"));
        assert!(eml_error.is_some(), "an error line from EML itself: {lines:?}");
        assert!(eml_error.unwrap().1.contains("enshrouded.exe"), "{lines:?}");
    }

    // A creature pack that caught its own failure (pcall) and only said so in a print: the build answered "up to
    // date" and the game started without that creature, with nothing on screen.
    #[test]
    fn a_mods_warnings_are_collected_with_its_id_and_still_logged() {
        let mut lines: Vec<(i32, String)> = Vec::new();
        let logger = Logger::new(Some(collect), &mut lines as *mut _ as *mut c_void);
        let ((), warnings) = with_log_collecting(logger, || {
            let mod_id = String::from("emf_beastmaster_bear");  // as EML has it
            tracing::warn_span!(target: "eml::lua", "lua", mod_id).in_scope(|| {
                tracing::warn!(target: "eml::lua", "emf_beastmaster_bear:\tBear: not added: out of references");
            });
            let quiet = "quiet";
            tracing::info_span!(target: "eml::lua", "lua", mod_id = quiet).in_scope(|| {
                tracing::info!(target: "eml::lua", "only information");
            });
            let failing = "failing";
            tracing::info_span!(target: "eml::lua", "lua", mod_id = failing).in_scope(|| {
                tracing::error!(target: "eml::lua", "an error counts too");
            });
            tracing::warn!(target: "eml::content", "a warning outside any mod is not one of a mod's");
        });
        assert_eq!(warnings.len(), 2, "{:?}", warnings.iter().map(|w| &w.warning).collect::<Vec<_>>());
        assert_eq!(warnings[0].id, "emf_beastmaster_bear");
        assert_eq!(warnings[0].warning, "Bear: not added: out of references");  // its own "<id>:" prefix goes: the id is in id
        assert_eq!(warnings[1].id, "failing");
        assert!(lines.iter().any(|(level, line)| *level == 1 && line.contains("not added")), "{lines:?}");
    }

    #[test]
    fn a_mods_warnings_are_collected_without_a_log_callback() {
        let ((), warnings) = with_log_collecting(Logger::none(), || {
            let mod_id = "m";
            tracing::warn_span!(target: "eml::lua", "lua", mod_id).in_scope(|| {
                tracing::warn!(target: "eml::lua", "w");
            });
        });
        assert_eq!(warnings.len(), 1);
        assert_eq!(warnings[0].id, "m");
    }

    #[test]
    fn a_mods_warnings_are_capped_and_cut_between_letters() {
        let ((), warnings) = with_log_collecting(Logger::none(), || {
            let mod_id = "m";
            tracing::warn_span!(target: "eml::lua", "lua", mod_id).in_scope(|| {
                for _ in 0..30 {
                    tracing::warn!(target: "eml::lua", "{}", "\u{e9}".repeat(400));
                }
            });
        });
        assert_eq!(warnings.len(), MAX_MOD_WARNINGS);
        assert!(warnings[0].warning.chars().count() <= MAX_WARNING_CHARS);
        assert!(warnings[0].warning.ends_with('\u{e9}'));
    }

    #[test]
    fn a_reply_lists_the_mods_warnings() {
        let reply = Reply {
            code: OK,
            mod_warnings: vec![ModWarningInfo { id: "a".into(), warning: "b".into() }],
            ..Default::default()
        };
        let v = parse(&reply.to_json());
        assert_eq!(v["mod_warnings"][0]["id"], "a");
        assert_eq!(v["mod_warnings"][0]["warning"], "b");
    }

    #[test]
    fn hard_linked_kfc_files_are_refused() {
        // EML writes the .kfc, .kfc_resources and .kfc.bak in place: through a link it would change another file.
        for name in ["enshrouded.kfc", "enshrouded.kfc_resources", "enshrouded.kfc.bak"] {
            let dir = TempDir::new("linked");
            std::fs::write(dir.0.join("enshrouded.exe"), b"MZ but not a game").unwrap();
            let original = dir.0.join("original.bin");
            std::fs::write(&original, b"GAME DATA").unwrap();
            std::fs::hard_link(&original, dir.0.join(name)).unwrap();
            let (code, reply) = build_json(&request_for(dir.utf8()), Logger::none());
            assert_eq!(code, RUN_FAILED, "{name}");
            let message = parse(&reply)["message"].as_str().unwrap().to_string();
            assert!(message.contains(name) && message.contains("link"), "{name}: {message}");
            assert!(!dir.0.join("mods").exists(), "{name}: EML must not run");
            assert_eq!(std::fs::read(&original).unwrap(), b"GAME DATA");
        }
    }

    #[test]
    fn a_panic_reaches_the_log_with_its_place() {
        let _turn = export_turn();
        let mut lines: Vec<(i32, String)> = Vec::new();
        let request = serde_json::json!({ "game_dir": "unused", "test_panic": "boom for the test" }).to_string();
        let (code, buf, len) = call_export_logged(&request, 64 * 1024, 0, Some(&mut lines));
        assert_eq!(code, PANIC);
        let reply = parse(reply_text(&buf, len));
        assert!(reply["message"].as_str().unwrap().contains("boom for the test"));
        let line = lines.iter().find(|(level, line)| *level == 0 && line.contains("boom for the test"));
        assert!(line.is_some_and(|(_, l)| l.contains("lib.rs:")), "the log names the place: {lines:?}");
    }

    #[test]
    fn the_callers_panic_hook_is_back_after_a_call() {
        let _turn = export_turn();
        static OURS: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let previous = std::panic::take_hook();
        std::panic::set_hook(Box::new(|_| {
            OURS.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        }));
        let request = serde_json::json!({ "game_dir": "unused", "test_panic": "inside" }).to_string();
        let (code, _, _) = call_export_logged(&request, 64 * 1024, 0, None);
        let during = OURS.load(std::sync::atomic::Ordering::SeqCst);
        let _ = std::panic::catch_unwind(|| panic!("after the call"));
        let after = OURS.load(std::sync::atomic::Ordering::SeqCst);
        std::panic::set_hook(previous);
        assert_eq!(code, PANIC);
        assert_eq!(during, 0, "the call's own hook took its panic");
        assert_eq!(after, 1, "the caller's hook is back");
    }

    #[test]
    fn a_call_from_inside_the_log_callback_is_refused() {
        let _turn = export_turn();
        unsafe extern "C" fn call_again(ctx: *mut c_void, _level: c_int, _line: *const c_char) {
            let inner = unsafe { &mut *(ctx as *mut Vec<(i32, String)>) };
            if !inner.is_empty() {
                return;
            }
            let request = std::ffi::CString::new("{\"game_dir\": \"unused\"}").unwrap();
            let mut buf = vec![0u8; 4096];
            let mut len = 0usize;
            let code = unsafe {
                emf_content_build(request.as_ptr(), None, std::ptr::null_mut(), buf.as_mut_ptr() as *mut c_char,
                    buf.len(), &mut len)
            };
            let text = if len > 0 && len <= buf.len() {
                String::from_utf8_lossy(&buf[..len - 1]).into_owned()
            } else {
                String::new()
            };
            inner.push((code, text));
        }
        let dir = TempDir::new("reentry");
        let mut inner: Vec<(i32, String)> = Vec::new();
        let request = std::ffi::CString::new(request_for(dir.utf8())).unwrap();
        let mut buf = vec![0u8; 64 * 1024];
        let mut len = 0usize;
        let code = unsafe {
            emf_content_build(request.as_ptr(), Some(call_again), &mut inner as *mut _ as *mut c_void,
                buf.as_mut_ptr() as *mut c_char, buf.len(), &mut len)
        };
        assert_eq!(code, RUN_FAILED, "the outer call goes on");
        assert_eq!(inner.len(), 1);
        assert_eq!(inner[0].0, BAD_REQUEST, "{:?}", inner[0].1);
        assert!(inner[0].1.contains("already"), "{:?}", inner[0].1);
    }

    #[test]
    fn a_reply_cut_to_fit_keeps_the_real_code() {
        let dir = TempDir::new("cut");
        let request = request_for(dir.utf8());
        let (code, buf, len) = call_export(&request, 120, b'#');
        assert_eq!(code, RUN_FAILED);
        assert!(len > 120, "the full length: {len}");
        let end = buf.iter().position(|&b| b == 0).expect("a NUL inside the buffer");
        let reply = parse(std::str::from_utf8(&buf[..end]).unwrap());
        assert_eq!(reply["code"], RUN_FAILED);
        assert_eq!(reply["truncated"], true);
        assert!(reply["message"].as_str().unwrap().starts_with("No enshrouded.kfc"));
        assert!(reply["mods"].as_array().unwrap().is_empty());
    }

    #[test]
    fn control_characters_are_dropped_from_log_lines() {
        let mut lines: Vec<(i32, String)> = Vec::new();
        let logger = Logger::new(Some(collect), &mut lines as *mut _ as *mut c_void);
        logger.send(2, "a\u{1b}[31mb\u{7}\tc\0d\r\nnext");
        assert_eq!(lines, vec![(2, "a[31mb\tcd".to_string()), (2, "next".to_string())]);
    }

    #[test]
    fn abi_is_one() {
        assert_eq!(emf_content_abi(), 1);
    }

    /// Calls emf_content_portraits the way C does.
    fn call_portraits_logged(
        request: &str,
        cap: usize,
        fill: u8,
        log: Option<&mut Vec<(i32, String)>>,
    ) -> (i32, Vec<u8>, usize) {
        let request = std::ffi::CString::new(request).unwrap();
        let mut buf = vec![fill; cap];
        let mut len = 0usize;
        let ptr = if cap == 0 { std::ptr::null_mut() } else { buf.as_mut_ptr() as *mut c_char };
        let (callback, ctx): (LogFn, *mut c_void) = match log {
            Some(lines) => (Some(collect), lines as *mut _ as *mut c_void),
            None => (None, std::ptr::null_mut()),
        };
        let code = unsafe { emf_content_portraits(request.as_ptr(), callback, ctx, ptr, cap, &mut len) };
        (code, buf, len)
    }

    fn call_portraits(request: &str, cap: usize) -> (i32, serde_json::Value) {
        let _turn = export_turn();
        let (code, buf, len) = call_portraits_logged(request, cap, 0, None);
        (code, parse(reply_text(&buf, len)))
    }

    fn portraits_request(game: &str, cache: &str, items: serde_json::Value) -> String {
        serde_json::json!({ "game_dir": game, "cache_dir": cache, "items": items }).to_string()
    }

    #[test]
    fn portraits_bad_json_is_bad_request_with_items() {
        let (code, reply) = call_portraits("{", 64 * 1024);
        assert_eq!(code, BAD_REQUEST);
        assert_eq!(reply["code"], BAD_REQUEST);
        assert!(reply["message"].as_str().unwrap().contains("emf_content_portraits"));
        assert!(reply["items"].as_array().unwrap().is_empty(), "the portraits reply's shape: {reply}");
        assert!(reply.get("mods").is_none());
        let (code, reply) = call_portraits("{\"game_dir\": \"C:\\\\Game\", \"items\": []}", 64 * 1024);
        assert_eq!(code, BAD_REQUEST);
        assert!(reply["message"].as_str().unwrap().contains("cache_dir"));
    }

    #[test]
    fn portraits_with_nothing_to_draw_are_ok_without_a_game() {
        let cache = TempDir::new("portraits_none");
        let (code, reply) = call_portraits(&portraits_request("C:\\no\\game", cache.utf8(), serde_json::json!([])), 4096);
        assert_eq!(code, OK, "{reply}");
        assert!(reply["ms"].is_u64());
    }

    #[test]
    fn portraits_without_an_archive_fail_and_name_the_folder() {
        let game = TempDir::new("portraits_nogame");
        let cache = TempDir::new("portraits_cache");
        let items = serde_json::json!([
            {"key": "wolf", "template": "478381c5-585f-4264-b373-dbb1ea596eca"},
            {"key": "bad key", "model": "91e2d4cc-01c9-4203-a92e-b08769364522"}
        ]);
        let (code, reply) = call_portraits(&portraits_request(game.utf8(), cache.utf8(), items), 64 * 1024);
        assert_eq!(code, RUN_FAILED, "{reply}");
        assert!(reply["message"].as_str().unwrap().contains(game.utf8()), "{reply}");
        assert_eq!(reply["items"][0]["status"], "failed");
        assert!(reply["items"][0]["error"].as_str().unwrap().contains("enshrouded.kfc"), "{reply}");
        assert_eq!(reply["items"][1]["key"], "bad key");
        assert!(reply["items"][1]["error"].as_str().unwrap().contains("key"), "{reply}");
    }

    #[test]
    fn portraits_too_small_and_cut_replies() {
        let game = TempDir::new("portraits_small");
        let cache = TempDir::new("portraits_small_cache");
        let items = serde_json::json!([{"key": "wolf", "model": "91e2d4cc-01c9-4203-a92e-b08769364522"}]);
        let request = portraits_request(game.utf8(), cache.utf8(), items);
        let _turn = export_turn();
        let (code, buf, len) = call_portraits_logged(&request, 4, b'#', None);
        assert_eq!(code, REPLY_TOO_SMALL);
        assert!(len > 4);
        assert_eq!(buf, b"####".to_vec());
        let (code, buf, len) = call_portraits_logged(&request, 120, b'#', None);
        assert_eq!(code, RUN_FAILED, "the real code");
        assert!(len > 120);
        let end = buf.iter().position(|&b| b == 0).expect("a NUL inside the buffer");
        let reply = parse(std::str::from_utf8(&buf[..end]).unwrap());
        assert_eq!(reply["code"], RUN_FAILED);
        assert_eq!(reply["truncated"], true);
        assert!(reply["items"].as_array().unwrap().is_empty());
    }

    #[test]
    fn portraits_panic_is_caught_and_logged() {
        let _turn = export_turn();
        let mut lines: Vec<(i32, String)> = Vec::new();
        let request = serde_json::json!({ "game_dir": "unused", "cache_dir": "C:\\x", "test_panic": "portrait boom" });
        let (code, buf, len) = call_portraits_logged(&request.to_string(), 64 * 1024, 0, Some(&mut lines));
        assert_eq!(code, PANIC);
        let reply = parse(reply_text(&buf, len));
        assert!(reply["message"].as_str().unwrap().contains("portrait boom"));
        assert!(reply["items"].as_array().unwrap().is_empty(), "the portraits reply's shape: {reply}");
        assert!(lines.iter().any(|(level, line)| *level == 0 && line.contains("portrait boom")), "{lines:?}");
    }

    #[test]
    fn portraits_log_lines_reach_the_callback() {
        let game = TempDir::new("portraits_log");
        let cache = TempDir::new("portraits_log_cache");
        let items = serde_json::json!([{"key": "wolf", "model": "91e2d4cc-01c9-4203-a92e-b08769364522"}]);
        let _turn = export_turn();
        let mut lines: Vec<(i32, String)> = Vec::new();
        let _ = call_portraits_logged(&portraits_request(game.utf8(), cache.utf8(), items), 64 * 1024, 0,
            Some(&mut lines));
        let line = lines.iter().find(|(level, line)| *level == 0 && line.starts_with("emf::portraits"));
        assert!(line.is_some_and(|(_, l)| l.contains(game.utf8())), "{lines:?}");
    }

    #[test]
    fn a_portraits_call_from_inside_a_builds_log_callback_is_refused() {
        let _turn = export_turn();
        unsafe extern "C" fn call_portraits_again(ctx: *mut c_void, _level: c_int, _line: *const c_char) {
            let inner = unsafe { &mut *(ctx as *mut Vec<(i32, String)>) };
            if !inner.is_empty() {
                return;
            }
            let request = std::ffi::CString::new("{\"game_dir\": \"unused\", \"cache_dir\": \"C:\\\\x\"}").unwrap();
            let mut buf = vec![0u8; 4096];
            let mut len = 0usize;
            let code = unsafe {
                emf_content_portraits(request.as_ptr(), None, std::ptr::null_mut(), buf.as_mut_ptr() as *mut c_char,
                    buf.len(), &mut len)
            };
            let text = if len > 0 && len <= buf.len() {
                String::from_utf8_lossy(&buf[..len - 1]).into_owned()
            } else {
                String::new()
            };
            inner.push((code, text));
        }
        let dir = TempDir::new("reentry_portraits");
        let mut inner: Vec<(i32, String)> = Vec::new();
        let request = std::ffi::CString::new(request_for(dir.utf8())).unwrap();
        let mut buf = vec![0u8; 64 * 1024];
        let mut len = 0usize;
        let code = unsafe {
            emf_content_build(request.as_ptr(), Some(call_portraits_again), &mut inner as *mut _ as *mut c_void,
                buf.as_mut_ptr() as *mut c_char, buf.len(), &mut len)
        };
        assert_eq!(code, RUN_FAILED, "the outer build goes on");
        assert_eq!(inner.len(), 1);
        assert_eq!(inner[0].0, BAD_REQUEST, "{:?}", inner[0].1);
        assert!(inner[0].1.contains("already"), "{:?}", inner[0].1);
        assert!(parse(&inner[0].1)["items"].is_array(), "the portraits reply's shape: {:?}", inner[0].1);
    }

    #[test]
    fn portraits_need_a_reply_len() {
        let request = std::ffi::CString::new("{}").unwrap();
        let mut buf = vec![0u8; 64];
        let code = unsafe {
            emf_content_portraits(request.as_ptr(), None, std::ptr::null_mut(), buf.as_mut_ptr() as *mut c_char, 64,
                std::ptr::null_mut())
        };
        assert_eq!(code, BAD_REQUEST);
    }
}
