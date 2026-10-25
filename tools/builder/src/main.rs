//! xboxrecomp-build: makes a self-building release's game from the player's
//! own copy of the disc.
//!
//!     xboxrecomp-build build  [--source <iso or folder>] [--release <dir>]
//!     xboxrecomp-build relink --source <iso or folder> [--release <dir>]
//!     xboxrecomp-build status [--release <dir>]
//!
//! A release (tools/release/make_release.py) carries no game code. It carries
//! this program, the pipeline and a Python to run it, an engine package, a
//! compiler, CMake and Ninja, and the title's recipe; release.json says which
//! title it is and which images of its default.xbe it was made for. `build`
//! does everything between the player choosing a disc and the game being
//! ready to start:
//!
//!   1. the game folder: an ISO is extracted once; a folder the player
//!      already has is mirrored with links, never copied, and never written
//!      to (the mirror's own UDATA and TDATA take the saves);
//!   2. the check: default.xbe's SHA-1 must be one release.json lists, or
//!      the build stops and says which image it got;
//!   3. the pipeline, lifting the XBE to C, after XbSymbolDatabase has named
//!      the XDK functions in this player's image (the names stay here: they
//!      are addresses in the player's own game);
//!   4. CMake and Ninja, building that C against the engine package;
//!   5. the executable put beside the game folder, and a stamp saying what
//!      it was built from, so a release update can tell it is stale.
//!
//! A folder's mirror is links, so moving or deleting the folder breaks the
//! game without touching what was built: `status` then says `moved`, and
//! `relink` lays out the game folder again from wherever the same copy is
//! now, rebuilding nothing.
//!
//! Everything happens in the player's folder for this title
//! ($XDG_DATA_HOME/xboxrecomp/builds/<TITLEID>; on a Mac
//! ~/Library/Application Support/xboxrecomp/builds/<TITLEID>; on Windows
//! %LOCALAPPDATA%\xboxrecomp\builds\<TITLEID>; or XBOXRECOMP_BUILD_DIR), so
//! the release itself can be read-only. Progress goes to stdout as one JSON
//! object a line, for the launcher to draw; everything the tools print goes
//! to build.log there.

use serde::{Deserialize, Serialize};
use serde_json::json;
use std::collections::VecDeque;
use std::fs::{self, File};
use std::io::{self, BufRead, BufReader, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode, Stdio};
use std::sync::{Arc, Mutex};
use std::time::UNIX_EPOCH;

// ── release.json ────────────────────────────────────────────────────────────

/// A default.xbe this release was made for.
#[derive(Deserialize, Clone)]
struct Image {
    sha1: String,
    /// What the player knows it as: "PAL", "USA".
    #[serde(default)]
    name: String,
    /// Where in the recipe this image's seeds.json, xdk_symbols.json and
    /// xbe_patches.json are, when it has its own (one folder per region).
    #[serde(default)]
    config: Option<String>,
}

#[derive(Deserialize)]
struct Release {
    title_id: String,
    title_name: String,
    version: String,
    /// The executable the recipe's CMakeLists builds (its target name).
    exe: String,
    #[serde(default)]
    images: Vec<Image>,
    /// The images as SHA-1s alone (releases made before images had names).
    #[serde(default)]
    xbe_sha1: Vec<String>,
    /// A title's own lift, in place of this program's: a script in the
    /// recipe and its arguments, run with the release's Python from the
    /// title's project folder ({xbe_dir} is the folder holding the XBE to
    /// lift). For a title whose lift does more than the pipeline -- one
    /// folder of addresses per region, a header naming which -- as
    /// split2-recomp's scripts/build.py does.
    #[serde(default)]
    lift: Option<Vec<String>>,
    /// The XBE the game boots, when it is not default.xbe: a disc whose
    /// default.xbe only picks a language (Gladius: gladius_eng.xbe). It is
    /// the one checked against `images` and lifted.
    #[serde(default)]
    xbe: Option<String>,
    #[serde(skip)]
    dir: PathBuf,
}

impl Release {
    fn boot_xbe(&self) -> &str {
        self.xbe.as_deref().unwrap_or("default.xbe")
    }

    fn load(dir: &Path) -> Result<Release, String> {
        let path = dir.join("release.json");
        let text = fs::read_to_string(&path)
            .map_err(|e| format!("cannot read {}: {e}", path.display()))?;
        let mut r: Release = serde_json::from_str(&text)
            .map_err(|e| format!("{} is not a valid release file: {e}", path.display()))?;
        r.dir = dir.to_path_buf();
        r.title_id = r.title_id.to_uppercase();
        for sha in std::mem::take(&mut r.xbe_sha1) {
            if !r.images.iter().any(|i| i.sha1.eq_ignore_ascii_case(&sha)) {
                r.images.push(Image { sha1: sha, name: String::new(), config: None });
            }
        }
        for i in r.images.iter_mut() {
            i.sha1 = i.sha1.to_lowercase();
        }
        if r.images.is_empty() {
            return Err(format!("{} names no {} images", path.display(), r.boot_xbe()));
        }
        if r.boot_xbe().contains(['/', '\\']) || r.boot_xbe().starts_with('.') {
            return Err(format!("{}: xbe must be a file name in the game folder", path.display()));
        }
        Ok(r)
    }
}

/// What was chosen, and what was built from it.
#[derive(Serialize, Deserialize, Default)]
struct SourceRecord {
    path: String,
    /// For an ISO: its size and modification time when it was extracted, so
    /// the same file is not extracted again.
    size: u64,
    mtime: u64,
}

#[derive(Serialize, Deserialize)]
struct Stamp {
    release_version: String,
    title_id: String,
    xbe_sha1: String,
    exe: String,
}

// ── progress ────────────────────────────────────────────────────────────────

/// The share of the whole build each stage is drawn as. Measured on
/// TimeSplitters 2: extraction is disk-bound, the pipeline about 19 s and the
/// compile about 30 s on a 6-core machine.
const STAGES: &[(&str, &str, f64)] = &[
    ("game", "Preparing the game files", 0.12),
    ("check", "Checking the disc", 0.01),
    ("lift", "Translating the game to C", 0.35),
    ("configure", "Setting up the build", 0.02),
    ("compile", "Compiling", 0.48),
    ("install", "Finishing", 0.02),
];

fn stage_index(name: &str) -> usize {
    STAGES.iter().position(|s| s.0 == name).expect("known stage")
}

fn emit(v: serde_json::Value) {
    let mut out = io::stdout().lock();
    let _ = writeln!(out, "{v}");
    let _ = out.flush();
}

/// Progress through `stage`, as a fraction of it and of the whole build.
fn progress(stage: &str, done: u64, total: u64, detail: &str) {
    let i = stage_index(stage);
    let before: f64 = STAGES[..i].iter().map(|s| s.2).sum();
    let part = if total > 0 { (done as f64 / total as f64).min(1.0) } else { 0.0 };
    emit(json!({
        "event": "progress", "stage": stage, "label": STAGES[i].1,
        "done": done, "total": total, "detail": detail,
        "overall": before + STAGES[i].2 * part,
    }));
}

#[derive(Debug)]
struct Failure {
    message: String,
    /// The process exit code: 2 for a disc this release does not support.
    code: u8,
}

fn fail<T>(message: impl Into<String>) -> Result<T, Failure> {
    Err(Failure { message: message.into(), code: 1 })
}

// ── the player's folder ─────────────────────────────────────────────────────

fn user_dir(title_id: &str) -> Result<PathBuf, Failure> {
    if let Some(d) = std::env::var_os("XBOXRECOMP_BUILD_DIR") {
        return Ok(PathBuf::from(d).join(title_id));
    }
    // Where the runtime keeps its saves on Windows (src/config/recomp_config.h).
    if cfg!(windows) {
        return match std::env::var_os("LOCALAPPDATA") {
            Some(d) if !d.is_empty() => Ok(PathBuf::from(d).join("xboxrecomp").join("builds")
                .join(title_id)),
            _ => fail("no folder to build in (LOCALAPPDATA is not set)"),
        };
    }
    // Where the runtime keeps a Mac's settings and saves (src/config/recomp_config.h).
    if cfg!(target_os = "macos") {
        return match std::env::var_os("HOME") {
            Some(h) => Ok(PathBuf::from(h).join("Library/Application Support/xboxrecomp/builds")
                .join(title_id)),
            None => fail("no home folder to build in (HOME is not set)"),
        };
    }
    let data = match std::env::var_os("XDG_DATA_HOME") {
        Some(d) if !d.is_empty() => PathBuf::from(d),
        _ => match std::env::var_os("HOME") {
            Some(h) => PathBuf::from(h).join(".local/share"),
            None => return fail("no home folder to build in (HOME is not set)"),
        },
    };
    Ok(data.join("xboxrecomp").join("builds").join(title_id))
}

/// The executable a target name builds: name.exe on Windows.
fn exe_name(name: &str) -> String {
    if cfg!(windows) { format!("{name}.exe") } else { name.to_string() }
}

/// A tool in the release, by its name without the extension.
fn tool(dir: &Path, rel: &str) -> PathBuf {
    dir.join(exe_name(rel))
}

/// The release's Python: python/bin/python3, or python\python.exe on Windows.
fn python_exe(release_dir: &Path) -> PathBuf {
    if cfg!(windows) { release_dir.join("python").join("python.exe") }
    else { release_dir.join("python/bin/python3") }
}

/// A path as the tools want it. Windows: canonicalize's \\?\ prefix off
/// (CMake and Python's tools do not all take it), and for CMake forward
/// slashes, which it never reads as escapes.
fn plain(p: PathBuf) -> PathBuf {
    let s = p.to_string_lossy();
    if let Some(rest) = s.strip_prefix(r"\\?\UNC\") {
        return PathBuf::from(format!(r"\\{rest}"));
    }
    if let Some(rest) = s.strip_prefix(r"\\?\") {
        return PathBuf::from(rest);
    }
    p
}

fn cmake_path(p: &Path) -> String {
    let s = p.display().to_string();
    if cfg!(windows) { s.replace('\\', "/") } else { s }
}

fn mtime_secs(m: &fs::Metadata) -> u64 {
    m.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_secs()).unwrap_or(0)
}

fn read_json<T: for<'a> Deserialize<'a>>(path: &Path) -> Option<T> {
    serde_json::from_str(&fs::read_to_string(path).ok()?).ok()
}

fn write_json<T: Serialize>(path: &Path, v: &T) -> Result<(), Failure> {
    let tmp = path.with_extension("json.part");
    fs::write(&tmp, serde_json::to_string_pretty(v).unwrap())
        .and_then(|_| fs::rename(&tmp, path))
        .or_else(|e| fail(format!("cannot write {}: {e}", path.display())))
}

/// Remove a folder this program made, and nothing it does not own: symlinks
/// in a mirror are removed as links, never followed.
fn remove_owned(path: &Path) -> Result<(), Failure> {
    match fs::symlink_metadata(path) {
        // A junction or directory link (Windows) is removed as a directory.
        Ok(m) if m.file_type().is_symlink() && is_dir_link(&m) => fs::remove_dir(path),
        Ok(m) if m.file_type().is_symlink() || m.is_file() => fs::remove_file(path),
        Ok(_) => fs::remove_dir_all(path),
        Err(_) => Ok(()),
    }
    .or_else(|e| fail(format!("cannot remove {}: {e}", path.display())))
}

// ── 1. the game folder ──────────────────────────────────────────────────────

/// A file read as a block device for xdvdfs.
struct FileDev(File);

impl xdvdfs::blockdev::BlockDeviceRead<io::Error> for FileDev {
    fn read(&mut self, offset: u64, buffer: &mut [u8]) -> Result<(), io::Error> {
        self.0.seek(SeekFrom::Start(offset))?;
        self.0.read_exact(buffer)
    }
}

fn xiso_error<E: std::fmt::Debug>(iso: &Path, e: E) -> Failure {
    Failure { message: format!("{} is not an Xbox disc image ({e:?})", iso.display()), code: 1 }
}

/// Every file of the image into `out`.
fn extract_iso(iso: &Path, out: &Path) -> Result<(), Failure> {
    let file = File::open(iso).or_else(|e| fail(format!("cannot open {}: {e}", iso.display())))?;
    // The game partition starts partway into a full ("redump") image.
    let mut dev = xdvdfs::blockdev::OffsetWrapper::new(FileDev(file))
        .map_err(|e| xiso_error(iso, e))?;
    let volume = xdvdfs::read::read_volume(&mut dev).map_err(|e| xiso_error(iso, e))?;
    let tree = volume.root_table.file_tree(&mut dev).map_err(|e| xiso_error(iso, e))?;

    let total: u64 = tree.iter().map(|(_, d)| { let r = d.node.dirent.data; r.size as u64 }).sum();
    let mut done = 0u64;
    let mut last = 0u64;
    let mut buf = vec![0u8; 4 << 20];
    fs::create_dir_all(out).or_else(|e| fail(format!("cannot create {}: {e}", out.display())))?;
    for (parent, dirent) in &tree {
        let name = dirent.name_str::<io::Error>().map_err(|e| xiso_error(iso, e))?;
        let rel = format!("{}/{}", parent.trim_start_matches('/'), name);
        let rel = rel.trim_start_matches('/');
        if rel.split('/').any(|c| c == ".." || c.is_empty()) {
            return fail(format!("{} has an unsafe file name: {rel}", iso.display()));
        }
        let dest = out.join(rel);
        if dirent.node.dirent.is_directory() {
            fs::create_dir_all(&dest)
                .or_else(|e| fail(format!("cannot create {}: {e}", dest.display())))?;
            continue;
        }
        if let Some(p) = dest.parent() {
            fs::create_dir_all(p).or_else(|e| fail(format!("cannot create {}: {e}", p.display())))?;
        }
        let region = dirent.node.dirent.data;
        let size = region.size as u64;
        let mut f = File::create(&dest)
            .or_else(|e| fail(format!("cannot write {}: {e}", dest.display())))?;
        if size > 0 {
            let start = region.offset::<io::Error>(0).map_err(|e| xiso_error(iso, e))?;
            let mut pos = 0u64;
            while pos < size {
                let n = ((size - pos) as usize).min(buf.len());
                xdvdfs::blockdev::BlockDeviceRead::read(&mut dev, start + pos, &mut buf[..n])
                    .or_else(|e| fail(format!("reading {}: {e}", iso.display())))?;
                f.write_all(&buf[..n])
                    .or_else(|e| fail(format!("writing {}: {e}", dest.display())))?;
                pos += n as u64;
                done += n as u64;
                if done - last >= 64 << 20 {
                    last = done;
                    progress("game", done, total, rel);
                }
            }
        }
    }
    progress("game", total, total, "");
    Ok(())
}

/// A folder of links to the player's own copy: folders as symlinks, files as
/// hard links where the file system allows, symlinks where it does not. The
/// game's save folders are left out, so it writes into real, empty ones here
/// (E: is the game folder) and never into the player's copy.
fn mirror_folder(src: &Path, out: &Path) -> Result<(), Failure> {
    fs::create_dir_all(out).or_else(|e| fail(format!("cannot create {}: {e}", out.display())))?;
    let entries = fs::read_dir(src)
        .or_else(|e| fail(format!("cannot read {}: {e}", src.display())))?;
    for entry in entries.flatten() {
        let name = entry.file_name();
        let upper = name.to_string_lossy().to_uppercase();
        if upper == "UDATA" || upper == "TDATA" {
            continue;
        }
        let from = fs::canonicalize(entry.path()).map(plain)
            .or_else(|e| fail(format!("cannot resolve {}: {e}", entry.path().display())))?;
        let to = out.join(&name);
        let linked = if from.is_dir() {
            symlink_dir(&from, &to)
        } else {
            fs::hard_link(&from, &to).or_else(|_| symlink_file(&from, &to))
        };
        linked.or_else(|e| fail(format!("cannot link {}: {e}", to.display())))?;
    }
    Ok(())
}

#[cfg(unix)]
fn symlink_dir(from: &Path, to: &Path) -> io::Result<()> {
    std::os::unix::fs::symlink(from, to)
}
#[cfg(unix)]
fn symlink_file(from: &Path, to: &Path) -> io::Result<()> {
    std::os::unix::fs::symlink(from, to)
}
#[cfg(unix)]
fn is_dir_link(_m: &fs::Metadata) -> bool {
    false
}

/// On Windows a folder is linked with a junction, which needs no privilege
/// (a symbolic link needs Developer Mode or an administrator); a file that
/// cannot be hard-linked (another drive) is copied.
#[cfg(windows)]
fn symlink_dir(from: &Path, to: &Path) -> io::Result<()> {
    junction::create(from, to)
}
#[cfg(windows)]
fn symlink_file(from: &Path, to: &Path) -> io::Result<()> {
    fs::copy(from, to).map(|_| ())
}
#[cfg(windows)]
fn is_dir_link(m: &fs::Metadata) -> bool {
    use std::os::windows::fs::MetadataExt;
    m.file_attributes() & 0x10 != 0         // FILE_ATTRIBUTE_DIRECTORY
}

/// A directory junction (a mount point reparse point), through the API
/// mklink /J uses, so no shell is involved.
#[cfg(windows)]
mod junction {
    use std::ffi::c_void;
    use std::io;
    use std::os::windows::ffi::OsStrExt;
    use std::path::Path;

    #[link(name = "kernel32")]
    extern "system" {
        fn CreateFileW(name: *const u16, access: u32, share: u32, sa: *mut c_void,
                       disposition: u32, flags: u32, template: *mut c_void) -> *mut c_void;
        fn DeviceIoControl(dev: *mut c_void, code: u32, inbuf: *const c_void, insize: u32,
                           outbuf: *mut c_void, outsize: u32, returned: *mut u32,
                           overlapped: *mut c_void) -> i32;
        fn CloseHandle(h: *mut c_void) -> i32;
    }
    const GENERIC_WRITE: u32 = 0x4000_0000;
    const OPEN_EXISTING: u32 = 3;
    const FILE_FLAG_BACKUP_SEMANTICS: u32 = 0x0200_0000;
    const FILE_FLAG_OPEN_REPARSE_POINT: u32 = 0x0020_0000;
    const FSCTL_SET_REPARSE_POINT: u32 = 0x0009_00A4;
    const IO_REPARSE_TAG_MOUNT_POINT: u32 = 0xA000_0003;

    fn wide(s: &std::ffi::OsStr) -> Vec<u16> {
        s.encode_wide().collect()
    }

    pub fn create(target: &Path, link: &Path) -> io::Result<()> {
        std::fs::create_dir(link)?;
        let made = set(target, link);
        if made.is_err() {
            let _ = std::fs::remove_dir(link);
        }
        made
    }

    fn set(target: &Path, link: &Path) -> io::Result<()> {
        // \??\C:\the\target as the substitute name, C:\the\target as the
        // name Explorer shows.
        let print = wide(target.as_os_str());
        let mut subst = wide(std::ffi::OsStr::new(r"\??\"));
        subst.extend_from_slice(&print);
        let (sb, pb) = (subst.len() * 2, print.len() * 2);
        // ReparseTag, ReparseDataLength, Reserved; then the mount point's
        // four offsets and lengths, and both names, each ending in a NUL.
        let path_buf = sb + 2 + pb + 2;
        let data_len = 8 + path_buf;
        let mut buf: Vec<u8> = Vec::with_capacity(8 + data_len);
        buf.extend_from_slice(&IO_REPARSE_TAG_MOUNT_POINT.to_le_bytes());
        buf.extend_from_slice(&(data_len as u16).to_le_bytes());
        buf.extend_from_slice(&0u16.to_le_bytes());
        for v in [0u16, sb as u16, (sb + 2) as u16, pb as u16] {
            buf.extend_from_slice(&v.to_le_bytes());
        }
        for w in subst.iter().chain([0u16].iter()).chain(print.iter()).chain([0u16].iter()) {
            buf.extend_from_slice(&w.to_le_bytes());
        }
        let mut name = wide(link.as_os_str());
        name.push(0);
        unsafe {
            let h = CreateFileW(name.as_ptr(), GENERIC_WRITE, 0, std::ptr::null_mut(),
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                std::ptr::null_mut());
            if h as isize == -1 {
                return Err(io::Error::last_os_error());
            }
            let mut got = 0u32;
            let ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.as_ptr() as *const c_void,
                                     buf.len() as u32, std::ptr::null_mut(), 0, &mut got,
                                     std::ptr::null_mut());
            let err = io::Error::last_os_error();
            CloseHandle(h);
            if ok == 0 {
                return Err(err);
            }
        }
        Ok(())
    }
}

/// A save folder in the game folder: UDATA or TDATA, or one kept beside them
/// under a suffix (UDATA.disc, UDATA.rescued), which must travel with them.
fn is_save_dir(name: &str) -> bool {
    let upper = name.to_uppercase();
    ["UDATA", "TDATA"].iter().any(|s| upper == *s || upper.starts_with(&format!("{s}.")))
}

/// The game's save folders, from the game folder being replaced into the new
/// one. E: is the game folder, so a title that saves there (TimeSplitters 2,
/// in UDATA) keeps its saves in it: a rebuild -- a release update, another
/// image of the same game -- must carry them over, not delete them. Should
/// the new folder have its own (a disc that ships one), it is kept beside
/// them as <name>.disc.
fn carry_saves(old: &Path, new: &Path) -> Result<(), Failure> {
    let Ok(entries) = fs::read_dir(old) else { return Ok(()) };
    for entry in entries.flatten() {
        let name = entry.file_name();
        let real_dir = fs::symlink_metadata(entry.path()).map(|m| m.is_dir()).unwrap_or(false);
        if !is_save_dir(&name.to_string_lossy()) || !real_dir {
            continue;
        }
        let dest = new.join(&name);
        if fs::symlink_metadata(&dest).is_ok() {
            let disc = new.join(format!("{}.disc", name.to_string_lossy()));
            fs::rename(&dest, &disc)
                .or_else(|e| fail(format!("cannot set aside the disc's {}: {e}", dest.display())))?;
        }
        fs::rename(entry.path(), &dest)
            .or_else(|e| fail(format!("cannot keep the saves in {}: {e}", entry.path().display())))?;
    }
    Ok(())
}

/// Saves left in a game.partial by a run that stopped between carrying them
/// over and putting the new folder in place (Cancel, a crash, a failed
/// rename), moved back into the game folder before game.partial is cleared.
/// Should the game folder somehow have its own as well, neither is lost:
/// the rescued one is kept beside it as <name>.rescued.
fn rescue_saves(partial: &Path, game: &Path) -> Result<(), Failure> {
    let Ok(entries) = fs::read_dir(partial) else { return Ok(()) };
    for entry in entries.flatten() {
        let name = entry.file_name();
        let real_dir = fs::symlink_metadata(entry.path()).map(|m| m.is_dir()).unwrap_or(false);
        if !is_save_dir(&name.to_string_lossy()) || !real_dir {
            continue;
        }
        fs::create_dir_all(game)
            .or_else(|e| fail(format!("cannot create {}: {e}", game.display())))?;
        let mut dest = game.join(&name);
        let mut n = 0;
        while fs::symlink_metadata(&dest).is_ok() {
            n += 1;
            let suffix = if n == 1 { String::new() } else { format!("-{n}") };
            dest = game.join(format!("{}.rescued{suffix}", name.to_string_lossy()));
        }
        fs::rename(entry.path(), &dest)
            .or_else(|e| fail(format!("cannot rescue the saves in {}: {e}", entry.path().display())))?;
    }
    Ok(())
}

fn prepare_game(source: &Path, user: &Path) -> Result<(), Failure> {
    let game = user.join("game");
    let record_path = user.join("source.json");
    rescue_saves(&user.join("game.partial"), &game)?;
    let meta = fs::metadata(source)
        .or_else(|e| fail(format!("cannot open {}: {e}", source.display())))?;
    let record = SourceRecord {
        path: source.display().to_string(),
        size: if meta.is_file() { meta.len() } else { 0 },
        mtime: if meta.is_file() { mtime_secs(&meta) } else { 0 },
    };

    if meta.is_file() {
        // Extracted already, from this same file: keep it.
        let old: Option<SourceRecord> = read_json(&record_path);
        if let Some(old) = old {
            if old.path == record.path && old.size == record.size && old.mtime == record.mtime
                && game.join("default.xbe").is_file()
                && fs::symlink_metadata(&game).map(|m| m.is_dir()).unwrap_or(false)
            {
                progress("game", 1, 1, "already extracted");
                return write_json(&record_path, &record);
            }
        }
        let partial = user.join("game.partial");
        remove_owned(&partial)?;
        progress("game", 0, 1, "");
        extract_iso(source, &partial)?;
        if !partial.join("default.xbe").is_file() {
            remove_owned(&partial)?;
            return fail(format!("{} has no default.xbe: it is not a game disc", source.display()));
        }
        carry_saves(&game, &partial)?;
        remove_owned(&game)?;
        fs::rename(&partial, &game)
            .or_else(|e| fail(format!("cannot move the extracted game into place: {e}")))?;
    } else {
        if !source.join("default.xbe").is_file() {
            return fail(format!("{} has no default.xbe: choose the folder the game's \
                                 files are in, or the disc image", source.display()));
        }
        progress("game", 0, 1, "");
        let partial = user.join("game.partial");
        remove_owned(&partial)?;
        mirror_folder(source, &partial)?;
        carry_saves(&game, &partial)?;
        remove_owned(&game)?;
        fs::rename(&partial, &game)
            .or_else(|e| fail(format!("cannot move the game folder into place: {e}")))?;
        progress("game", 1, 1, "");
    }
    write_json(&record_path, &record)
}

// ── 2. the check ────────────────────────────────────────────────────────────

fn sha1_file(path: &Path) -> Result<String, Failure> {
    let mut f = File::open(path).or_else(|e| fail(format!("cannot read {}: {e}", path.display())))?;
    let mut h = sha1_smol::Sha1::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf).or_else(|e| fail(format!("reading {}: {e}", path.display())))?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(h.digest().to_string())
}

fn check_disc(release: &Release, user: &Path) -> Result<Image, Failure> {
    progress("check", 0, 1, "");
    let xbe = user.join("game").join(release.boot_xbe());
    if !xbe.is_file() {
        return Err(Failure {
            message: format!("This isn't a copy of {}: it has no {}.", release.title_name,
                             release.boot_xbe()),
            code: 2,
        });
    }
    let sha1 = sha1_file(&xbe)?;
    if let Some(image) = release.images.iter().find(|i| i.sha1 == sha1) {
        progress("check", 1, 1, &image.name);
        return Ok(image.clone());
    }
    let what = fs::read(&xbe).ok().and_then(|d| xbe::parse(&d).ok())
        .map(|i| format!("{} ({}, {})", i.title_name, i.title_id_hex(), i.region_label()))
        .unwrap_or_else(|| "an unreadable XBE".into());
    Err(Failure {
        message: format!(
            "This isn't a copy of {} this release supports. Its {} is {what}, \
             SHA-1 {sha1}. This release was made for: {}.",
            release.title_name, release.boot_xbe(),
            release.images.iter().map(|i| if i.name.is_empty() { i.sha1.clone() }
                                          else { format!("{} ({})", i.name, i.sha1) })
                .collect::<Vec<_>>().join(", ")),
        code: 2,
    })
}

// ── running the tools ───────────────────────────────────────────────────────

/// Run a tool, its output appended to the log and each line handed to
/// `on_line`; on failure, the error carries the last lines it printed.
fn run(cmd: &mut Command, log: &Path, what: &str, mut on_line: impl FnMut(&str))
    -> Result<(), Failure>
{
    let mut logf = fs::OpenOptions::new().create(true).append(true).open(log)
        .or_else(|e| fail(format!("cannot write {}: {e}", log.display())))?;
    let _ = writeln!(logf, "\n$ {cmd:?}");
    let logf = Arc::new(Mutex::new(logf));
    // No console window for each tool: the launcher has none to share.
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000);        // CREATE_NO_WINDOW
    }
    let mut child = cmd.stdout(Stdio::piped()).stderr(Stdio::piped()).stdin(Stdio::null())
        .spawn().or_else(|e| fail(format!("cannot start {what}: {e}")))?;
    let tail = Arc::new(Mutex::new(VecDeque::<String>::new()));

    let err = child.stderr.take().unwrap();
    let (log2, tail2) = (logf.clone(), tail.clone());
    let errs = std::thread::spawn(move || {
        for line in BufReader::new(err).lines().map_while(Result::ok) {
            let _ = writeln!(log2.lock().unwrap(), "{line}");
            let mut t = tail2.lock().unwrap();
            t.push_back(line);
            if t.len() > 25 { t.pop_front(); }
        }
    });
    for line in BufReader::new(child.stdout.take().unwrap()).lines().map_while(Result::ok) {
        let _ = writeln!(logf.lock().unwrap(), "{line}");
        {
            let mut t = tail.lock().unwrap();
            t.push_back(line.clone());
            if t.len() > 25 { t.pop_front(); }
        }
        on_line(&line);
    }
    let _ = errs.join();
    let status = child.wait().or_else(|e| fail(format!("{what}: {e}")))?;
    if status.success() {
        return Ok(());
    }
    let tail: Vec<String> = tail.lock().unwrap().iter().cloned().collect();
    fail(format!("{what} failed ({status}). Its last lines:\n{}\n\nThe whole log is {}",
                 tail.join("\n"), log.display()))
}

/// The recipe's files into the player's project folder, leaving files that
/// have not changed alone so the build does not redo them.
fn sync_tree(from: &Path, to: &Path) -> Result<(), Failure> {
    fs::create_dir_all(to).or_else(|e| fail(format!("cannot create {}: {e}", to.display())))?;
    for entry in fs::read_dir(from).or_else(|e| fail(format!("cannot read {}: {e}", from.display())))?.flatten() {
        let (src, dst) = (entry.path(), to.join(entry.file_name()));
        if src.is_dir() {
            sync_tree(&src, &dst)?;
        } else if fs::read(&dst).ok() != fs::read(&src).ok() {
            fs::copy(&src, &dst).or_else(|e| fail(format!("cannot copy {}: {e}", src.display())))?;
        }
    }
    Ok(())
}

// ── 3-5. lift, build, install ───────────────────────────────────────────────

/// recompile.py's "  2/4  disasm" as each stage starts, as progress.
fn lift_progress(line: &str) {
    let t = line.trim_start();
    if let Some((n, rest)) = t.split_once('/') {
        let mut words = rest.split_whitespace();
        if let (Ok(n), Some("4")) = (n.parse::<u64>(), words.next()) {
            if (1..=4).contains(&n) {
                progress("lift", n, 5, words.next().unwrap_or(""));
            }
        }
    }
}

fn lift(release: &Release, image: &Image, user: &Path, log: &Path) -> Result<(), Failure> {
    let r = &release.dir;
    let project = user.join("project");
    sync_tree(&r.join("recipe"), &project)?;

    // The pipeline writes beside the XBE it is given, so it gets its own
    // copy (a hard link where possible), never the one in the player's folder.
    let xdir = user.join("pipe").join("xbe");
    fs::create_dir_all(&xdir).or_else(|e| fail(format!("cannot create {}: {e}", xdir.display())))?;
    let name = release.boot_xbe();
    let xbe = xdir.join(name);
    let _ = fs::remove_file(&xbe);
    let game_xbe = fs::canonicalize(user.join("game").join(name)).map(plain)
        .or_else(|e| fail(format!("cannot find {name}: {e}")))?;
    fs::hard_link(&game_xbe, &xbe).or_else(|_| fs::copy(&game_xbe, &xbe).map(|_| ()))
        .or_else(|e| fail(format!("cannot copy {name}: {e}")))?;

    let tmp = user.join("tmp");
    fs::create_dir_all(&tmp).ok();
    let python = |c: &mut Command, dir: &Path| {
        c.current_dir(dir)
            .env("PYTHONDONTWRITEBYTECODE", "1").env("PYTHONUNBUFFERED", "1")
            .env("TMPDIR", &tmp);
    };

    // A title's own lift. It finds the pipeline where its toolkit submodule
    // would be, xboxrecomp/ beside its sources, and is given the private
    // copy of the XBE, so whatever it writes beside it stays here.
    if let Some(hook) = &release.lift {
        // Re-pointed when the release has moved since the last build.
        let toolkit_link = project.join("xboxrecomp");
        let toolkit = r.join("toolkit");
        if fs::read_link(&toolkit_link).ok().as_deref() != Some(toolkit.as_path()) {
            let _ = fs::remove_file(&toolkit_link).or_else(|_| fs::remove_dir(&toolkit_link));
            symlink_dir(&toolkit, &toolkit_link)
                .or_else(|e| fail(format!("cannot link the pipeline into the project: {e}")))?;
        }
        let Some((script, args)) = hook.split_first() else {
            return fail("release.json's lift names no script");
        };
        progress("lift", 0, 5, &image.name);
        let mut cmd = Command::new(python_exe(r));
        cmd.arg(project.join(script));
        for a in args {
            cmd.arg(a.replace("{xbe_dir}", &xdir.display().to_string()));
        }
        python(&mut cmd, &project);
        run(&mut cmd, log, "the translation to C", lift_progress)?;
        progress("lift", 5, 5, "");
        return Ok(());
    }

    // This image's addresses: its own folder when the recipe has one per
    // image, else the recipe's config/.
    let config = project.join(image.config.as_deref().unwrap_or("config"));

    // Which XDK functions sit where in this image, for the replacements the
    // lifter links in by name. A recipe may carry them; otherwise they are
    // found here, by signature, as tools.xdk_symbols does at a desk.
    progress("lift", 0, 5, "naming the XDK functions");
    // Beside the XBE, named after it, where recompile.py looks.
    let stem = Path::new(name).file_stem().map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_else(|| "default".into());
    let symbols = xdir.join(format!("{stem}_xdk_symbols.json"));
    let carried = config.join("xdk_symbols.json");
    if carried.is_file() {
        fs::copy(&carried, &symbols)
            .or_else(|e| fail(format!("cannot copy the XDK symbols: {e}")))?;
    } else {
        let mut c = Command::new(python_exe(r));
        c.args(["-m", "tools.xdk_symbols"]).arg(&xbe)
            .arg("--cli").arg(tool(r, "bin/XbSymbolDatabaseCLI"))
            .arg("--out").arg(&symbols);
        python(&mut c, &r.join("toolkit"));
        run(&mut c, log, "XbSymbolDatabase", |_| {})?;
    }

    let mut cmd = Command::new(python_exe(r));
    cmd.arg(r.join("toolkit/scripts/recompile.py")).arg(&xbe)
        .arg("--work-dir").arg(user.join("pipe").join("out"))
        .arg("--project").arg(&project);
    python(&mut cmd, &r.join("toolkit"));
    let seeds = config.join("seeds.json");
    if seeds.is_file() {
        cmd.arg("--seeds").arg(&seeds);
    }
    // An image with a folder of its own has its patches there or none: the
    // toolkit's config/xbe_patches/<TITLEID>.json cannot say which image
    // it is for.
    let patches = config.join("xbe_patches.json");
    if patches.is_file() {
        cmd.arg("--xbe-patches").arg(&patches);
    } else if image.config.is_some() {
        cmd.arg("--no-xbe-patches");
    }
    run(&mut cmd, log, "the translation to C", lift_progress)?;
    progress("lift", 5, 5, "");
    Ok(())
}

fn build(release: &Release, user: &Path, log: &Path) -> Result<PathBuf, Failure> {
    let r = &release.dir;
    let tc = r.join("toolchain");
    let bdir = user.join("build");
    let cmake = tool(&tc, "cmake/bin/cmake");
    let zig_cache = user.join("zig-cache");
    // On a Mac the toolchain's own xcrun comes first (fetch_toolchain.py):
    // the system's offers to install Xcode's tools, which zig does not need.
    // And no deployment target from the player's environment: zig's target
    // names the macOS a release is built for.
    let path = std::env::var_os("PATH").unwrap_or_default();
    let mut dirs = vec![tc.join("bin")];
    dirs.extend(std::env::split_paths(&path));
    let mac_path = std::env::join_paths(dirs).unwrap_or(path);
    let with_env = |c: &mut Command| {
        // From the player's folder, not wherever the launcher was started
        // (a folder CMake may not be able to read).
        c.current_dir(user)
            .env("ZIG_GLOBAL_CACHE_DIR", &zig_cache).env("ZIG_LOCAL_CACHE_DIR", &zig_cache);
        if cfg!(target_os = "macos") {
            c.env("PATH", &mac_path).env_remove("MACOSX_DEPLOYMENT_TARGET");
        }
    };

    progress("configure", 0, 1, "");
    // A build folder made by another release (another engine, another
    // compiler) starts again; one from this release is built incrementally.
    let marker = bdir.join("xboxrecomp-release.txt");
    let want = format!("{}\n{}\n", release.version, r.display());
    if fs::read_to_string(&marker).ok().as_deref() != Some(want.as_str()) {
        remove_owned(&bdir)?;
    }
    if !bdir.join("CMakeCache.txt").is_file() {
        let mut c = Command::new(&cmake);
        c.arg("-S").arg(user.join("project")).arg("-B").arg(&bdir)
            .arg("-G").arg("Ninja")
            .arg(format!("-DCMAKE_MAKE_PROGRAM={}", cmake_path(&tool(&tc, "ninja/bin/ninja"))))
            .arg("-DCMAKE_BUILD_TYPE=Release")
            .arg(format!("-DCMAKE_C_COMPILER={}", cmake_path(&tool(&tc, "bin/zcc"))))
            .arg(format!("-DCMAKE_CXX_COMPILER={}", cmake_path(&tool(&tc, "bin/zc++"))))
            .arg(format!("-DCMAKE_AR={}", cmake_path(&tool(&tc, "bin/zar"))))
            .arg(format!("-DCMAKE_RANLIB={}", cmake_path(&tool(&tc, "bin/zranlib"))))
            .arg(format!("-DXBOXRECOMP_DIR={}", cmake_path(&r.join("engine"))));
        // zig rc, for the game's icon (cmake/title_icon.cmake).
        if cfg!(windows) {
            c.arg(format!("-DCMAKE_RC_COMPILER={}", cmake_path(&tool(&tc, "bin/zrc"))));
        }
        with_env(&mut c);
        run(&mut c, log, "CMake", |_| {})?;
        fs::write(&marker, &want).ok();
    }
    progress("configure", 1, 1, "");

    progress("compile", 0, 1, "");
    let mut c = Command::new(&cmake);
    c.arg("--build").arg(&bdir).arg("--target").arg(&release.exe);
    with_env(&mut c);
    run(&mut c, log, "the compile", |line| {
        // Ninja: "[12/49] Building C object ..."
        if let Some(rest) = line.strip_prefix('[') {
            if let Some((counts, what)) = rest.split_once(']') {
                if let Some((d, t)) = counts.split_once('/') {
                    if let (Ok(d), Ok(t)) = (d.trim().parse::<u64>(), t.trim().parse::<u64>()) {
                        let file = what.rsplit('/').next().unwrap_or("").trim();
                        progress("compile", d, t,
                                 file.trim_end_matches(".obj").trim_end_matches(".o"));
                    }
                }
            }
        }
    })?;
    let exe = bdir.join(exe_name(&release.exe));
    if !exe.is_file() {
        return fail(format!("the compile finished but made no {}", exe.display()));
    }
    Ok(exe)
}

fn install(release: &Release, user: &Path, built: &Path, sha1: &str) -> Result<PathBuf, Failure> {
    progress("install", 0, 1, "");
    // Beside the game folder, which is where the game looks for it.
    let dest = user.join(exe_name(&release.exe));
    let tmp = user.join(format!("{}.part", exe_name(&release.exe)));
    fs::copy(built, &tmp).and_then(|_| fs::rename(&tmp, &dest))
        .or_else(|e| fail(format!("cannot install {}: {e}", dest.display())))?;
    // What the release keeps for beside the game (on a Mac: the SDL dylib the
    // game links, the Vulkan loader, MoltenVK and its manifest, DXC), copied
    // rather than linked, so the game still starts after the release is
    // moved; on APFS a copy is a clone, and costs nothing.
    let runtime = release.dir.join("runtime");
    if runtime.is_dir() {
        sync_tree(&runtime, user)?;
    }
    // The renderer looks for the shader compiler beside the executable first.
    let dxc = release.dir.join("dxc/libdxcompiler.so");
    if dxc.is_file() {
        let link = user.join("libdxcompiler.so");
        let _ = fs::remove_file(&link);
        symlink_file(&dxc, &link).or_else(|e| fail(format!("cannot link the shader compiler: {e}")))?;
    }
    write_json(&user.join("build.json"), &Stamp {
        release_version: release.version.clone(),
        title_id: release.title_id.clone(),
        xbe_sha1: sha1.to_string(),
        exe: dest.display().to_string(),
    })?;
    progress("install", 1, 1, "");
    Ok(dest)
}

// ── one build at a time ─────────────────────────────────────────────────────

/// Held for the length of a build: two builds in one folder would extract,
/// lift and compile over each other. A lock whose process has gone (a
/// crash, a power cut) is taken over.
struct BuildLock(PathBuf);

impl BuildLock {
    fn take(user: &Path) -> Result<BuildLock, Failure> {
        let path = user.join("build.lock");
        for _ in 0..2 {
            match fs::OpenOptions::new().write(true).create_new(true).open(&path) {
                Ok(mut f) => {
                    let _ = write!(f, "{}", std::process::id());
                    return Ok(BuildLock(path));
                }
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => {
                    let holder = fs::read_to_string(&path).ok()
                        .and_then(|s| s.trim().parse::<u32>().ok());
                    if holder.is_some_and(process_alive) {
                        return fail("this game is already being built (another window \
                                     may be building it); wait for that to finish");
                    }
                    let _ = fs::remove_file(&path);         // left by a build that died
                }
                Err(e) => return fail(format!("cannot create {}: {e}", path.display())),
            }
        }
        fail(format!("cannot take {}", path.display()))
    }
}

impl Drop for BuildLock {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

#[cfg(target_os = "linux")]
fn process_alive(pid: u32) -> bool {
    Path::new(&format!("/proc/{pid}")).exists()
}
#[cfg(all(unix, not(target_os = "linux")))]
fn process_alive(pid: u32) -> bool {
    extern "C" {
        fn kill(pid: i32, sig: i32) -> i32;
    }
    let Ok(pid) = i32::try_from(pid) else { return false };
    // Signal 0 checks without sending: EPERM is a process that exists but is
    // not ours, which still holds the lock.
    let alive = unsafe { kill(pid, 0) } == 0;
    alive || io::Error::last_os_error().raw_os_error() == Some(1)
}
#[cfg(windows)]
fn process_alive(pid: u32) -> bool {
    use std::ffi::c_void;
    #[link(name = "kernel32")]
    extern "system" {
        fn OpenProcess(access: u32, inherit: i32, pid: u32) -> *mut c_void;
        fn GetExitCodeProcess(h: *mut c_void, code: *mut u32) -> i32;
        fn CloseHandle(h: *mut c_void) -> i32;
    }
    const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x1000;
    const STILL_ACTIVE: u32 = 259;
    unsafe {
        let h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if h.is_null() {
            // Access denied is a process that exists but is not ours.
            return std::io::Error::last_os_error().raw_os_error() == Some(5);
        }
        let mut code = 0u32;
        let ok = GetExitCodeProcess(h, &mut code);
        CloseHandle(h);
        ok != 0 && code == STILL_ACTIVE
    }
}

// ── commands ────────────────────────────────────────────────────────────────

fn cmd_build(release: &Release, source: Option<PathBuf>) -> Result<(), Failure> {
    let user = user_dir(&release.title_id)?;
    fs::create_dir_all(&user).or_else(|e| fail(format!("cannot create {}: {e}", user.display())))?;
    let _lock = BuildLock::take(&user)?;
    let source = match source {
        Some(s) => s,
        None => match read_json::<SourceRecord>(&user.join("source.json")) {
            Some(r) => PathBuf::from(r.path),
            None => return fail("no disc chosen yet: pass --source <iso or folder>"),
        },
    };
    let source = fs::canonicalize(&source).map(plain)
        .or_else(|e| fail(format!("{}: {e}", source.display())))?;
    let log = user.join("build.log");
    let _ = fs::write(&log, format!("xboxrecomp-build {} for {} {}\n",
                                    env!("CARGO_PKG_VERSION"), release.title_name, release.version));
    emit(json!({"event": "start", "title": release.title_name, "user_dir": user,
                "source": source, "log": log}));

    prepare_game(&source, &user)?;
    let image = check_disc(release, &user)?;
    lift(release, &image, &user, &log)?;
    let built = build(release, &user, &log)?;
    let exe = install(release, &user, &built, &image.sha1)?;
    emit(json!({"event": "done", "exe": exe, "game_dir": user.join("game")}));
    Ok(())
}

/// Whether the game folder still reaches every file: a folder the player
/// picked is linked, not copied, so moving or deleting it breaks the links.
fn game_intact(user: &Path) -> bool {
    let game = user.join("game");
    if !game.join("default.xbe").is_file() {
        return false;
    }
    match fs::read_dir(&game) {
        Ok(entries) => entries.flatten().all(|e| fs::metadata(e.path()).is_ok()),
        Err(_) => false,
    }
}

/// Point a built game at its files again, after the player moved them:
/// the game folder is laid out afresh and nothing is rebuilt. A different
/// copy (another region, another revision) needs the build its addresses
/// came from, so it is refused, and if an image had to be extracted before
/// that could be seen, the stamp goes too, so the launcher offers a build.
fn cmd_relink(release: &Release, source: Option<PathBuf>) -> Result<(), Failure> {
    let user = user_dir(&release.title_id)?;
    let Some(stamp) = read_json::<Stamp>(&user.join("build.json")) else {
        return fail("nothing is built yet: build it first");
    };
    let _lock = BuildLock::take(&user)?;
    let Some(source) = source else {
        return fail("relink needs --source <iso or folder>");
    };
    let source = fs::canonicalize(&source).map(plain)
        .or_else(|e| fail(format!("{}: {e}", source.display())))?;
    let log = user.join("build.log");
    if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(&log) {
        let _ = writeln!(f, "relink from {}", source.display());
    }
    emit(json!({"event": "start", "title": release.title_name, "user_dir": user,
                "source": source, "log": log}));
    let different = |sha1: &str| Failure {
        message: format!("This is a different copy of {} from the one it was built from \
                          (its {} has SHA-1 {sha1}, not {}). Build again to use it.",
                         release.title_name, release.boot_xbe(), stamp.xbe_sha1),
        code: 3,
    };
    if source.is_dir() {
        let sha1 = sha1_file(&source.join(release.boot_xbe()))?;
        if sha1 != stamp.xbe_sha1 {
            return Err(different(&sha1));
        }
    }
    prepare_game(&source, &user)?;
    let sha1 = sha1_file(&user.join("game").join(release.boot_xbe()))?;
    if sha1 != stamp.xbe_sha1 {
        let _ = fs::remove_file(user.join("build.json"));
        return Err(different(&sha1));
    }
    emit(json!({"event": "done", "exe": stamp.exe, "game_dir": user.join("game")}));
    Ok(())
}

fn cmd_status(release: &Release) -> Result<(), Failure> {
    let user = user_dir(&release.title_id)?;
    let source: Option<SourceRecord> = read_json(&user.join("source.json"));
    let stamp: Option<Stamp> = read_json(&user.join("build.json"));
    let state = match &stamp {
        Some(s) if s.release_version == release.version
            && Path::new(&s.exe).is_file() && !game_intact(&user) => "moved",
        Some(s) if s.release_version == release.version
            && Path::new(&s.exe).is_file() => "ready",
        Some(_) => "stale",
        None if source.is_some() => "unbuilt",
        None => "no-source",
    };
    emit(json!({
        "event": "status", "state": state, "user_dir": user,
        "source": source.map(|s| s.path),
        "built_version": stamp.as_ref().map(|s| s.release_version.clone()),
        "exe": stamp.map(|s| s.exe),
    }));
    Ok(())
}

fn usage() -> ExitCode {
    eprintln!("usage: xboxrecomp-build build [--source <iso or folder>] [--release <dir>]\n       \
               xboxrecomp-build relink --source <iso or folder> [--release <dir>]\n       \
               xboxrecomp-build status [--release <dir>]");
    ExitCode::from(64)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(command) = args.first().cloned() else { return usage() };
    let (mut source, mut release_dir) = (None, None);
    let mut it = args[1..].iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--source" => source = it.next().map(PathBuf::from),
            "--release" => release_dir = it.next().map(PathBuf::from),
            _ => return usage(),
        }
    }
    // A release keeps this program in bin/, one level below release.json.
    let release_dir = release_dir.unwrap_or_else(|| {
        std::env::current_exe().ok()
            .and_then(|p| p.parent()?.parent().map(Path::to_path_buf))
            .unwrap_or_else(|| PathBuf::from("."))
    });
    let result = Release::load(&release_dir).map_err(|m| Failure { message: m, code: 1 })
        .and_then(|release| match command.as_str() {
            "build" => cmd_build(&release, source),
            "relink" => cmd_relink(&release, source),
            "status" => cmd_status(&release),
            _ => fail(format!("unknown command {command}")),
        });
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(f) => {
            emit(json!({"event": "error", "message": f.message, "unsupported": f.code == 2,
                        "rebuild": f.code == 3}));
            ExitCode::from(f.code)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("xboxrecomp-build-test-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn a_mirror_links_the_game_and_leaves_its_saves_out() {
        let d = scratch("mirror");
        let (src, out) = (d.join("src"), d.join("out"));
        for sub in ["Data", "UDATA", "tdata"] {
            fs::create_dir_all(src.join(sub)).unwrap();
        }
        fs::write(src.join("default.xbe"), b"xbe").unwrap();
        mirror_folder(&src, &out).unwrap();
        assert!(fs::symlink_metadata(out.join("Data")).unwrap().file_type().is_symlink());
        assert_eq!(fs::read(out.join("default.xbe")).unwrap(), b"xbe");
        assert!(!out.join("UDATA").exists() && !out.join("tdata").exists());
        // Removing the mirror removes links, never what they point at.
        remove_owned(&out).unwrap();
        assert!(src.join("Data").is_dir() && src.join("default.xbe").is_file());
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn a_moved_folder_is_seen_and_relinked() {
        let d = scratch("moved");
        let (src, user) = (d.join("src"), d.join("user"));
        fs::create_dir_all(src.join("Data")).unwrap();
        fs::write(src.join("default.xbe"), b"xbe").unwrap();
        prepare_game(&src, &user).unwrap();
        fs::create_dir_all(user.join("game").join("UDATA")).unwrap();
        fs::write(user.join("game").join("UDATA").join("save"), b"s").unwrap();
        assert!(game_intact(&user));
        // The player moves their folder: the folder links dangle.
        let moved = d.join("moved");
        fs::rename(&src, &moved).unwrap();
        assert!(!game_intact(&user), "a dangling link is a broken game");
        prepare_game(&moved, &user).unwrap();
        assert!(game_intact(&user));
        assert_eq!(fs::read(user.join("game").join("UDATA").join("save")).unwrap(), b"s",
                   "laying the folder out again keeps the saves");
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn saves_left_by_an_interrupted_run_are_rescued() {
        let d = scratch("interrupted");
        let (src, user) = (d.join("src"), d.join("user"));
        fs::create_dir_all(src.join("Data")).unwrap();
        fs::write(src.join("default.xbe"), b"xbe").unwrap();
        // A run stopped after carrying the saves into game.partial and
        // removing the old game folder, before the rename.
        let stranded = user.join("game.partial").join("UDATA");
        fs::create_dir_all(&stranded).unwrap();
        fs::write(stranded.join("save"), b"s").unwrap();
        prepare_game(&src, &user).unwrap();
        assert_eq!(fs::read(user.join("game").join("UDATA").join("save")).unwrap(), b"s");
        assert!(!user.join("game.partial").exists());
        // Both a stranded copy and one in the game folder: both kept.
        let stranded = user.join("game.partial").join("UDATA");
        fs::create_dir_all(&stranded).unwrap();
        fs::write(stranded.join("save"), b"old").unwrap();
        prepare_game(&src, &user).unwrap();
        assert_eq!(fs::read(user.join("game").join("UDATA").join("save")).unwrap(), b"s");
        assert_eq!(fs::read(user.join("game").join("UDATA.rescued").join("save")).unwrap(), b"old");
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn a_release_names_its_images_old_style_or_new() {
        let d = scratch("release");
        fs::write(d.join("release.json"), r#"{"title_id": "4553000a", "title_name": "T",
            "version": "1", "exe": "t",
            "images": [{"sha1": "AB", "name": "PAL", "config": "config/pal"}],
            "xbe_sha1": ["cd", "ab"]}"#).unwrap();
        let r = Release::load(&d).ok().expect("loads");
        assert_eq!(r.title_id, "4553000A");
        assert_eq!(r.images.len(), 2, "the old list adds only what is new");
        assert_eq!((r.images[0].sha1.as_str(), r.images[0].name.as_str()), ("ab", "PAL"));
        assert_eq!(r.images[0].config.as_deref(), Some("config/pal"));
        assert_eq!(r.images[1].sha1, "cd");
        assert_eq!(r.boot_xbe(), "default.xbe", "default.xbe unless the recipe says");
        fs::write(d.join("release.json"), r#"{"title_id": "1", "title_name": "T",
            "version": "1", "exe": "t", "xbe": "game_eng.xbe", "xbe_sha1": ["ab"]}"#).unwrap();
        assert_eq!(Release::load(&d).ok().expect("loads").boot_xbe(), "game_eng.xbe");
        fs::write(d.join("release.json"), r#"{"title_id": "1", "title_name": "T",
            "version": "1", "exe": "t", "xbe": "../x.xbe", "xbe_sha1": ["ab"]}"#).unwrap();
        assert!(Release::load(&d).is_err(), "only a file in the game folder");
        fs::write(d.join("release.json"), r#"{"title_id": "1", "title_name": "T",
            "version": "1", "exe": "t"}"#).unwrap();
        assert!(Release::load(&d).is_err(), "a release must name an image");
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn a_rebuild_keeps_the_saves() {
        let d = scratch("saves");
        let src = d.join("src");
        fs::create_dir_all(src.join("Data")).unwrap();
        fs::write(src.join("default.xbe"), b"xbe").unwrap();
        prepare_game(&src, &d).unwrap();
        // The game saves into its own UDATA (E: is the game folder).
        fs::create_dir_all(d.join("game/UDATA/4553000A")).unwrap();
        fs::write(d.join("game/UDATA/4553000A/save.dat"), b"progress").unwrap();
        // Build again: the mirror is made anew, and the save comes with it.
        prepare_game(&src, &d).unwrap();
        assert_eq!(fs::read(d.join("game/UDATA/4553000A/save.dat")).unwrap(), b"progress");
        assert!(!src.join("UDATA").exists(), "never written into the player's copy");
        // A new folder that brings its own UDATA keeps it beside the saves.
        fs::create_dir_all(src.join("UDATA")).unwrap();
        fs::write(src.join("UDATA/disc.txt"), b"disc").unwrap();
        let other = d.join("game.new");
        mirror_folder(&src, &other).unwrap();
        fs::create_dir_all(other.join("UDATA")).unwrap();      // as an image would
        carry_saves(&d.join("game"), &other).unwrap();
        assert!(other.join("UDATA/4553000A/save.dat").is_file());
        assert!(other.join("UDATA.disc").is_dir());
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn one_build_at_a_time_and_a_dead_one_is_taken_over() {
        let d = scratch("lock");
        let held = BuildLock::take(&d).ok().expect("first lock");
        assert!(BuildLock::take(&d).is_err(), "a second build must be refused");
        drop(held);
        assert!(!d.join("build.lock").exists());
        // A pid no process has: what a build killed by Cancel leaves.
        fs::write(d.join("build.lock"), "4000000000").unwrap();
        assert!(BuildLock::take(&d).is_ok(), "a dead build's lock is taken over");
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn the_recipe_sync_leaves_unchanged_files_alone() {
        let d = scratch("sync");
        let (from, to) = (d.join("from"), d.join("to"));
        fs::create_dir_all(from.join("src")).unwrap();
        fs::write(from.join("src/main.c"), b"int main;").unwrap();
        sync_tree(&from, &to).unwrap();
        let before = fs::metadata(to.join("src/main.c")).unwrap().modified().unwrap();
        std::thread::sleep(std::time::Duration::from_millis(20));
        sync_tree(&from, &to).unwrap();
        assert_eq!(fs::metadata(to.join("src/main.c")).unwrap().modified().unwrap(), before);
        fs::write(from.join("src/main.c"), b"int main2;").unwrap();
        sync_tree(&from, &to).unwrap();
        assert_eq!(fs::read(to.join("src/main.c")).unwrap(), b"int main2;");
        let _ = fs::remove_dir_all(&d);
    }
}
