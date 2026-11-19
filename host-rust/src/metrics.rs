// SPDX-License-Identifier: Apache-2.0
//! Host metric collection straight out of `/proc` and `/sys`.
//!
//! `/proc` files are reopened once and then re-read from offset 0 on every tick,
//! and every sampler writes into caller owned buffers, so a steady state tick
//! does essentially no allocation.

use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::time::Instant;

use crate::proto::{Iface, MAX_IFACES};

/// A /proc file that can be re-read cheaply.
pub struct ProcFile {
    file: File,
    buf: String,
}

impl ProcFile {
    pub fn open(path: &str) -> io::Result<Self> {
        Ok(Self {
            file: File::open(path)?,
            buf: String::with_capacity(16 * 1024),
        })
    }

    /// Refresh the contents (procfs regenerates on seek back to 0).
    pub fn fill(&mut self) -> io::Result<()> {
        self.buf.clear();
        self.file.seek(SeekFrom::Start(0))?;
        self.file.read_to_string(&mut self.buf)?;
        Ok(())
    }

    pub fn text(&self) -> &str {
        &self.buf
    }
}

// ------------------------------------------------------------------- cpu ---

pub struct CpuSampler {
    stat: ProcFile,
    prev: Vec<(u64, u64)>, // (busy, total)
    cur: Vec<(u64, u64)>,
    primed: bool,
}

impl CpuSampler {
    pub fn new() -> io::Result<Self> {
        Ok(Self {
            stat: ProcFile::open("/proc/stat")?,
            prev: Vec::with_capacity(64),
            cur: Vec::with_capacity(64),
            primed: false,
        })
    }

    /// Per-logical-CPU busy percentage since the previous call.
    pub fn sample(&mut self, out: &mut Vec<f64>) -> io::Result<()> {
        self.stat.fill()?;
        self.cur.clear();

        for line in self.stat.text().lines() {
            let Some(rest) = line.strip_prefix("cpu") else {
                break; // the cpu lines are always first in /proc/stat
            };
            let (name, fields) = rest.split_once(' ').unwrap_or((rest, ""));
            if name.is_empty() || !name.bytes().all(|b| b.is_ascii_digit()) {
                continue; // the aggregate "cpu " line
            }
            let mut total = 0u64;
            let mut idle = 0u64;
            for (i, tok) in fields.split_whitespace().enumerate() {
                let Ok(v) = tok.parse::<u64>() else { continue };
                total += v;
                if i == 3 || i == 4 {
                    // idle + iowait
                    idle += v;
                }
            }
            self.cur.push((total - idle, total));
        }

        out.clear();
        if !self.primed || self.prev.len() != self.cur.len() {
            self.primed = true;
            out.resize(self.cur.len(), 0.0);
        } else {
            for ((pbusy, ptotal), (busy, total)) in self.prev.iter().zip(self.cur.iter()) {
                let dt = total.saturating_sub(*ptotal);
                out.push(if dt > 0 {
                    100.0 * (busy.saturating_sub(*pbusy)) as f64 / dt as f64
                } else {
                    0.0
                });
            }
        }
        std::mem::swap(&mut self.prev, &mut self.cur);
        Ok(())
    }
}

// --------------------------------------------------------------- memory ---

#[derive(Clone, Copy, Default)]
pub struct MemInfo {
    pub total_mb: u32,
    pub used_mb: u32,
    pub swap_total_mb: u32,
    pub swap_used_mb: u32,
}

pub struct MemSampler {
    meminfo: ProcFile,
}

impl MemSampler {
    pub fn new() -> io::Result<Self> {
        Ok(Self {
            meminfo: ProcFile::open("/proc/meminfo")?,
        })
    }

    pub fn sample(&mut self) -> io::Result<MemInfo> {
        self.meminfo.fill()?;
        let mut total_kb = 0u64;
        let mut avail_kb = 0u64;
        let mut free_kb = 0u64;
        let mut swap_total_kb = 0u64;
        let mut swap_free_kb = 0u64;

        for line in self.meminfo.text().lines() {
            let Some((key, rest)) = line.split_once(':') else {
                continue;
            };
            let value = rest
                .split_whitespace()
                .next()
                .and_then(|v| v.parse::<u64>().ok())
                .unwrap_or(0);
            match key {
                "MemTotal" => total_kb = value,
                "MemAvailable" => avail_kb = value,
                "MemFree" => free_kb = value,
                "SwapTotal" => swap_total_kb = value,
                "SwapFree" => swap_free_kb = value,
                _ => {}
            }
        }
        if avail_kb == 0 {
            avail_kb = free_kb;
        }

        let total_mb = (total_kb / 1024) as u32;
        let avail_mb = (avail_kb / 1024) as u32;
        let swap_total_mb = (swap_total_kb / 1024) as u32;
        let swap_free_mb = (swap_free_kb / 1024) as u32;
        Ok(MemInfo {
            total_mb,
            used_mb: total_mb.saturating_sub(avail_mb),
            swap_total_mb,
            swap_used_mb: swap_total_mb.saturating_sub(swap_free_mb),
        })
    }
}

// -------------------------------------------------------------- network ---

fn operstate_up(name: &str) -> bool {
    std::fs::read_to_string(Path::new("/sys/class/net").join(name).join("operstate"))
        .map(|s| s.trim() == "up")
        .unwrap_or(false)
}

fn is_physical(name: &str) -> bool {
    Path::new("/sys/class/net")
        .join(name)
        .join("device")
        .exists()
}

pub struct NetSampler {
    dev: ProcFile,
    forced: Option<String>,
    prev: Vec<(String, u64, u64)>,
    ema: Vec<(String, f64, f64)>,
    last: Instant,
}

impl NetSampler {
    pub fn new(forced: Option<String>) -> io::Result<Self> {
        let mut s = Self {
            dev: ProcFile::open("/proc/net/dev")?,
            forced,
            prev: Vec::with_capacity(16),
            ema: Vec::with_capacity(16),
            last: Instant::now(),
        };
        s.dev.fill()?;
        s.prev = parse_netdev(s.dev.text());
        Ok(s)
    }

    /// Pick at most `MAX_IFACES` interfaces and compute smoothed byte rates.
    pub fn sample(&mut self, out: &mut Vec<Iface>) -> io::Result<()> {
        let now = Instant::now();
        let dt = (now - self.last).as_secs_f64().max(1e-6);
        self.last = now;

        self.dev.fill()?;
        let cur = parse_netdev(self.dev.text());

        let mut rates: Vec<(&str, f64, f64)> = Vec::with_capacity(cur.len());
        for (name, rx, tx) in &cur {
            let (prx, ptx) = self
                .prev
                .iter()
                .find(|(n, _, _)| n == name)
                .map(|(_, r, t)| (*r, *t))
                .unwrap_or((*rx, *tx));
            rates.push((
                name.as_str(),
                rx.saturating_sub(prx) as f64 / dt,
                tx.saturating_sub(ptx) as f64 / dt,
            ));
        }

        let chosen: Vec<&str> = match self.forced.as_deref() {
            Some(forced) => vec![forced],
            None => choose(&rates),
        };
        // rate independent smoothing (tau = 0.4 s)
        let alpha = 1.0 - (-dt / 0.4).exp();

        out.clear();
        for name in chosen {
            let (_, rx, tx) = rates
                .iter()
                .find(|(n, _, _)| *n == name)
                .copied()
                .unwrap_or((name, 0.0, 0.0));

            let slot = self.ema.iter_mut().find(|(n, _, _)| n.as_str() == name);
            let (erx, etx) = match slot {
                Some((_, erx, etx)) => {
                    *erx += alpha * (rx - *erx);
                    *etx += alpha * (tx - *etx);
                    (*erx, *etx)
                }
                None => {
                    self.ema.push((name.to_string(), rx, tx));
                    (rx, tx)
                }
            };
            out.push(Iface::new(
                name,
                erx.max(0.0) as u32,
                etx.max(0.0) as u32,
                operstate_up(name),
            ));
        }

        self.prev = cur;
        Ok(())
    }
}

/// Prefer physical, up interfaces, busiest first.
fn choose<'a>(rates: &[(&'a str, f64, f64)]) -> Vec<&'a str> {
    let mut names: Vec<&str> = rates
        .iter()
        .map(|(n, _, _)| *n)
        .filter(|n| *n != "lo")
        .collect();
    let physical: Vec<&str> = names.iter().copied().filter(|n| is_physical(n)).collect();
    if !physical.is_empty() {
        names = physical;
    }
    let up: Vec<&str> = names.iter().copied().filter(|n| operstate_up(n)).collect();
    if !up.is_empty() {
        names = up;
    }
    let rate_of = |name: &str| {
        rates
            .iter()
            .find(|(n, _, _)| *n == name)
            .map(|(_, r, t)| r + t)
            .unwrap_or(0.0)
    };
    names.sort_by(|a, b| {
        rate_of(b)
            .partial_cmp(&rate_of(a))
            .unwrap_or(std::cmp::Ordering::Equal)
    });
    names.truncate(MAX_IFACES);
    names
}

fn parse_netdev(text: &str) -> Vec<(String, u64, u64)> {
    let mut out = Vec::with_capacity(16);
    for line in text.lines() {
        let Some((name, rest)) = line.split_once(':') else {
            continue; // the two header lines have no colon
        };
        let fields: Vec<&str> = rest.split_whitespace().collect();
        if fields.len() < 9 {
            continue;
        }
        let rx = fields[0].parse::<u64>().unwrap_or(0);
        let tx = fields[8].parse::<u64>().unwrap_or(0);
        out.push((name.trim().to_string(), rx, tx));
    }
    out
}

// ----------------------------------------------------------------- misc ---

/// loadavg x100 (1, 5, 15 minutes).
pub fn read_loadavg() -> [u16; 3] {
    let mut out = [0u16; 3];
    if let Ok(text) = std::fs::read_to_string("/proc/loadavg") {
        for (i, tok) in text.split_whitespace().take(3).enumerate() {
            out[i] = tok
                .parse::<f64>()
                .map(|v| (v * 100.0).round().clamp(0.0, u16::MAX as f64) as u16)
                .unwrap_or(0);
        }
    }
    out
}

pub fn read_uptime_s() -> u32 {
    std::fs::read_to_string("/proc/uptime")
        .ok()
        .and_then(|s| {
            s.split_whitespace()
                .next()
                .and_then(|v| v.parse::<f64>().ok())
        })
        .map(|v| v.max(0.0) as u32)
        .unwrap_or(0)
}

pub fn hostname() -> String {
    std::fs::read_to_string("/proc/sys/kernel/hostname")
        .map(|s| s.trim().to_string())
        .unwrap_or_else(|_| "unknown".to_string())
}

/// Local wall clock hour/minute/second.
pub fn local_hms() -> [u8; 3] {
    let mut now: libc::time_t = 0;
    // SAFETY: `now` and `tm` are valid, properly aligned locals and `localtime_r`
    // does not retain either pointer.
    unsafe {
        libc::time(&mut now as *mut libc::time_t);
        let mut tm: libc::tm = std::mem::zeroed();
        if libc::localtime_r(&now as *const libc::time_t, &mut tm as *mut libc::tm).is_null() {
            return [0, 0, 0];
        }
        [tm.tm_hour as u8, tm.tm_min as u8, tm.tm_sec as u8]
    }
}

// ----------------------------------------------------------- temperature ---

/// Checked in this order -- never in hwmon enumeration order.  ACPI thermal
/// zones ("acpitz") report a made-up value (a flat 20 C on this machine) and
/// usually sit at hwmon0, so they must not win over a real die sensor.
const TEMP_PRIORITY: &[&str] = &[
    "k10temp",
    "coretemp",
    "zenpower",
    "cpu_thermal",
    "soc_thermal",
    "acpitz",
];
/// Within one chip, prefer the package/die reading over per-core ones.
const TEMP_LABEL_HINTS: &[&str] = &["tctl", "tdie", "package", "cpu", "composite"];
/// Definitely not the CPU.
const TEMP_NEVER: &[&str] = &["nvme", "amdgpu", "radeon"];

pub struct TempSensor {
    pub path: PathBuf,
    pub label: String,
}

impl TempSensor {
    pub fn detect(explicit: Option<&str>) -> Option<Self> {
        if let Some(p) = explicit {
            let path = PathBuf::from(p);
            return Some(Self {
                label: describe(&path),
                path,
            });
        }

        let sensors = hwmon_sensors();
        for want in TEMP_PRIORITY {
            if let Some((_, temps)) = sensors.iter().find(|(name, _)| name == want) {
                return Some(temps_to_sensor(temps));
            }
        }
        // Last resort: anything that is not obviously a disk/GPU.
        for (name, temps) in &sensors {
            if !TEMP_NEVER.contains(&name.as_str()) {
                return Some(temps_to_sensor(temps));
            }
        }
        None
    }

    /// Degrees Celsius x10, or `NO_TEMP` if unreadable.
    pub fn read(&self) -> i16 {
        match std::fs::read_to_string(&self.path) {
            Ok(text) => match text.trim().parse::<i64>() {
                // millidegrees -> decidegrees, floored like the Python version
                Ok(milli) => milli
                    .div_euclid(100)
                    .clamp(i16::MIN as i64, i16::MAX as i64) as i16,
                Err(_) => crate::proto::NO_TEMP,
            },
            Err(_) => crate::proto::NO_TEMP,
        }
    }
}

fn temps_to_sensor(temps: &[(String, PathBuf)]) -> TempSensor {
    let pick = TEMP_LABEL_HINTS
        .iter()
        .find_map(|hint| {
            temps
                .iter()
                .find(|(label, _)| label.to_ascii_lowercase().contains(hint))
        })
        .or_else(|| {
            temps
                .iter()
                .find(|(_, p)| p.to_string_lossy().ends_with("temp1_input"))
        })
        .or_else(|| temps.first())
        .expect("temps_to_sensor called with an empty list");
    TempSensor {
        path: pick.1.clone(),
        label: describe(&pick.1),
    }
}

fn describe(path: &Path) -> String {
    let file = path
        .file_name()
        .map(|f| f.to_string_lossy().to_string())
        .unwrap_or_default();
    // <hwmonN>/tempM_input -> the chip name lives next to the temp file
    let base = path.parent();
    let chip = base
        .and_then(|b| std::fs::read_to_string(b.join("name")).ok())
        .map(|s| s.trim().to_string())
        .unwrap_or_else(|| "?".to_string());
    let label = path
        .to_string_lossy()
        .strip_suffix("_input")
        .and_then(|prefix| std::fs::read_to_string(format!("{prefix}_label")).ok())
        .map(|s| s.trim().to_string())
        .unwrap_or_default();
    if label.is_empty() {
        format!("{chip}/{file}")
    } else {
        format!("{chip}/{file} {label}")
    }
}

fn hwmon_sensors() -> Vec<(String, Vec<(String, PathBuf)>)> {
    let mut out = Vec::new();
    let Ok(entries) = std::fs::read_dir("/sys/class/hwmon") else {
        return out;
    };
    let mut dirs: Vec<PathBuf> = entries.flatten().map(|e| e.path()).collect();
    dirs.sort();
    for base in dirs {
        let Ok(name) = std::fs::read_to_string(base.join("name")) else {
            continue;
        };
        let Ok(files) = std::fs::read_dir(&base) else {
            continue;
        };
        let mut names: Vec<String> = files
            .flatten()
            .filter_map(|e| e.file_name().into_string().ok())
            .collect();
        names.sort();

        let mut temps = Vec::new();
        for f in names {
            if !(f.starts_with("temp") && f.ends_with("_input")) {
                continue;
            }
            let prefix = &f[..f.len() - "_input".len()];
            let label = std::fs::read_to_string(base.join(format!("{prefix}_label")))
                .map(|s| s.trim().to_string())
                .unwrap_or_default();
            temps.push((label, base.join(&f)));
        }
        if !temps.is_empty() {
            out.push((name.trim().to_string(), temps));
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn netdev_parsing_skips_headers() {
        let text = "Inter-|   Receive                                                |  Transmit\n \
                    face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n \
                    lo: 100 1 0 0 0 0 0 0 200 2 0 0 0 0 0 0\n \
                    wlan0: 300 3 0 0 0 0 0 0 400 4 0 0 0 0 0 0\n";
        let parsed = parse_netdev(text);
        assert_eq!(parsed.len(), 2);
        assert_eq!(parsed[0], ("lo".to_string(), 100, 200));
        assert_eq!(parsed[1], ("wlan0".to_string(), 300, 400));
    }

    #[test]
    fn cpu_sampler_reads_cores() {
        let mut s = CpuSampler::new().expect("/proc/stat");
        let mut out = Vec::new();
        s.sample(&mut out).unwrap();
        assert!(!out.is_empty(), "expected at least one logical cpu");
        std::thread::sleep(std::time::Duration::from_millis(120));
        s.sample(&mut out).unwrap();
        assert!(out.iter().all(|v| (0.0..=100.0).contains(v)), "{out:?}");
    }

    #[test]
    fn temp_sensor_skips_acpitz_when_a_real_one_exists() {
        let sensors = hwmon_sensors();
        if sensors.iter().any(|(n, _)| n == "k10temp") {
            let t = TempSensor::detect(None).expect("a sensor");
            assert!(t.label.contains("k10temp"), "picked {}", t.label);
        }
    }
}
