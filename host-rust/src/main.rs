// SPDX-License-Identifier: Apache-2.0
//! Host side of the ESP32-S3-RLCD-4.2 load dashboard.
//!
//! Samples CPU / memory / network from /proc and pushes a compact binary frame
//! to the board's native USB serial (USB-Serial-JTAG) at a steady rate.  This is
//! a drop-in replacement for `host/rlcd_host.py` and speaks the exact same wire
//! format (see `src/proto.rs` and `firmware/rlcd_dash/protocol.h`).

mod metrics;
mod proto;
mod serial;

use std::io::{self, Write};
use std::process::ExitCode;
use std::time::{Duration, Instant};

use metrics::{
    hostname, local_hms, read_loadavg, read_uptime_s, CpuSampler, MemSampler, NetSampler,
    TempSensor,
};
use proto::{Iface, Payload, FRAME_LEN, MAX_CPUS};
use serial::SerialLink;

// ------------------------------------------------------------ smoothing ---

/// Exponential smoothing with a time constant, so the display feels the same
/// whether we run at 5 Hz or 60 Hz.
struct Ema {
    tau: f64,
    value: Option<f64>,
    at: Option<Instant>,
}

impl Ema {
    fn new(tau: f64) -> Self {
        Self {
            tau,
            value: None,
            at: None,
        }
    }

    fn update(&mut self, v: f64, now: Instant) -> f64 {
        let next = match (self.value, self.at) {
            (Some(prev), Some(at)) => {
                let dt = (now - at).as_secs_f64().max(1e-6);
                prev + (1.0 - (-dt / self.tau).exp()) * (v - prev)
            }
            _ => v,
        };
        self.value = Some(next);
        self.at = Some(now);
        next
    }
}

/// One `Ema` per logical CPU, rebuilt when the core count changes.
struct EmaList {
    tau: f64,
    emas: Vec<Ema>,
}

impl EmaList {
    fn new(tau: f64) -> Self {
        Self {
            tau,
            emas: Vec::new(),
        }
    }

    fn update(&mut self, values: &[f64], now: Instant, out: &mut Vec<f64>) {
        if self.emas.len() != values.len() {
            self.emas = (0..values.len()).map(|_| Ema::new(self.tau)).collect();
        }
        out.clear();
        for (ema, v) in self.emas.iter_mut().zip(values.iter()) {
            out.push(ema.update(*v, now));
        }
    }
}

// ----------------------------------------------------------------- args ---

struct Args {
    port: Option<String>,
    hz: f64,
    iface: Option<String>,
    temp: Option<String>,
    wait_secs: Option<u64>,
    report_every: f64,
    quiet: bool,
    dry_run: bool,
    print_every: f64,
    help: bool,
}

impl Default for Args {
    fn default() -> Self {
        Self {
            port: Some("/dev/ttyACM0".to_string()),
            hz: 10.0,
            iface: None,
            temp: None,
            wait_secs: None,
            report_every: 60.0,
            quiet: false,
            dry_run: false,
            print_every: 0.0,
            help: false,
        }
    }
}

const USAGE: &str = "\
rlcd-host - stream host load to an ESP32-S3-RLCD-4.2 over USB serial

USAGE:
    rlcd-host [OPTIONS]

OPTIONS:
    -p, --port <DEV>       serial device                 [default: /dev/ttyACM0]
    -r, --hz <HZ>          frame rate                    [default: 10]
    -i, --iface <NAME>     force a network interface     [default: auto]
        --temp <PATH>      force a temperature sensor    [default: auto]
        --wait <SECS>      wait for the serial device, 0 = forever [default: fail]
        --print-every <S>  dry-run: seconds between metric prints [default: off]
        --report-every <S> seconds between send summaries, 0 = off [default: 60]
    -q, --quiet            do not print board status lines
        --dry-run          sample and print, do not open the serial port
    -h, --help             print this help
";

fn parse_args() -> Result<Args, String> {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let mut a = Args::default();
    let mut i = 0usize;

    while i < argv.len() {
        let arg = argv[i].clone();
        let (key, inline) = match arg.split_once('=') {
            Some((k, v)) => (k.to_string(), Some(v.to_string())),
            None => (arg.clone(), None),
        };

        macro_rules! value {
            () => {
                match inline.clone() {
                    Some(v) => v,
                    None => {
                        i += 1;
                        argv.get(i)
                            .cloned()
                            .ok_or_else(|| format!("{key} requires a value"))?
                    }
                }
            };
        }

        match key.as_str() {
            "-p" | "--port" => a.port = Some(value!()),
            "-r" | "--hz" => {
                a.hz = value!()
                    .parse::<f64>()
                    .map_err(|_| "--hz expects a number".to_string())?
            }
            "-i" | "--iface" => a.iface = Some(value!()),
            "--temp" => a.temp = Some(value!()),
            "--wait" => {
                a.wait_secs =
                    Some(value!().parse::<u64>().map_err(|_| {
                        "--wait expects a number of seconds (0 = forever)".to_string()
                    })?)
            }
            "--print-every" => {
                a.print_every = value!()
                    .parse::<f64>()
                    .map_err(|_| "--print-every expects a number".to_string())?
            }
            "--report-every" => {
                a.report_every = value!()
                    .parse::<f64>()
                    .map_err(|_| "--report-every expects a number of seconds".to_string())?
            }
            "-q" | "--quiet" => a.quiet = true,
            "--dry-run" => a.dry_run = true,
            "-h" | "--help" => a.help = true,
            other => return Err(format!("unknown option: {other}")),
        }
        i += 1;
    }

    if !(a.hz.is_finite() && a.hz > 0.0) {
        return Err("--hz must be a positive number".to_string());
    }
    if a.dry_run {
        a.port = None;
    }
    Ok(a)
}

// ----------------------------------------------------------------- misc ---

fn human_rate(bps: f64) -> String {
    if bps >= 1024.0 * 1024.0 {
        format!("{:.2} MB/s", bps / 1024.0 / 1024.0)
    } else if bps >= 1024.0 {
        format!("{:.1} KB/s", bps / 1024.0)
    } else {
        format!("{bps:.0} B/s")
    }
}

// ------------------------------------------------------------------ run ---

fn run(args: Args) -> io::Result<()> {
    let host = hostname();
    let mut cpu = CpuSampler::new()?;
    let mut mem = MemSampler::new()?;
    let mut net = NetSampler::new(args.iface.clone())?;
    let temp = TempSensor::detect(args.temp.as_deref());

    let mut link = match &args.port {
        Some(port) => Some(SerialLink::open_waiting(port, args.wait_secs)?),
        None => None,
    };

    let stdout = io::stdout();
    let mut out = stdout.lock();

    if let (Some(port), Some(_)) = (&args.port, &link) {
        writeln!(
            out,
            "[host] {}: streaming to {} at {} Hz ({} B/frame = {:.1} KB/s)",
            host,
            port,
            args.hz,
            FRAME_LEN,
            FRAME_LEN as f64 * args.hz / 1024.0
        )?;
    }
    match &temp {
        Some(t) => writeln!(out, "[host] cpu temp sensor: {}", t.label)?,
        None => writeln!(
            out,
            "[host] cpu temp sensor: none  (dashboard will show --)"
        )?,
    }
    if let Some(wanted) = &args.temp {
        if !std::path::Path::new(wanted).exists() {
            writeln!(out, "[host] warning: {wanted} does not exist")?;
        }
    }
    out.flush()?;

    // Buffers reused for the whole run.
    let mut cpu_raw: Vec<f64> = Vec::with_capacity(64);
    let mut cpu_smooth: Vec<f64> = Vec::with_capacity(64);
    let mut ifaces: Vec<Iface> = Vec::with_capacity(4);
    let mut frame: Vec<u8> = Vec::with_capacity(FRAME_LEN);
    let mut rx: Vec<u8> = Vec::with_capacity(4096);
    let mut rx_text = String::with_capacity(4096);
    let mut payload = Payload::default();
    payload.set_host(&host);

    let mut ema_total = Ema::new(0.5);
    let mut ema_cores = EmaList::new(0.6);

    // Prime the deltas.
    cpu.sample(&mut cpu_raw)?;
    net.sample(&mut ifaces)?;

    let period = Duration::from_secs_f64(1.0 / args.hz);
    let mut next = Instant::now();
    let mut seq: u32 = 0;
    let mut sent: u32 = 0;
    let mut dropped: u32 = 0;
    let mut late: u32 = 0;
    let mut last_report = Instant::now();
    let mut last_print = last_report;

    loop {
        next += period;
        let now = Instant::now();

        cpu.sample(&mut cpu_raw)?;
        ema_cores.update(&cpu_raw, now, &mut cpu_smooth);
        let mi = mem.sample()?;
        net.sample(&mut ifaces)?;

        let ncpu = cpu_smooth.len().min(MAX_CPUS);
        payload.seq = seq as u16;
        payload.ncpu = ncpu as u16;
        let avg = if cpu_smooth.is_empty() {
            0.0
        } else {
            cpu_smooth.iter().sum::<f64>() / cpu_smooth.len() as f64
        };
        payload.cpu_total_x10 = (ema_total.update(avg, now) * 10.0)
            .round()
            .clamp(0.0, 1000.0) as u16;
        payload.cpu_load = [0; MAX_CPUS];
        for (slot, v) in payload.cpu_load.iter_mut().zip(cpu_smooth.iter()) {
            *slot = v.round().clamp(0.0, 100.0) as u8;
        }
        payload.mem_used_mb = mi.used_mb;
        payload.mem_total_mb = mi.total_mb;
        payload.swap_used_mb = mi.swap_used_mb;
        payload.swap_total_mb = mi.swap_total_mb;
        payload.load = read_loadavg();
        payload.temp_x10 = temp
            .as_ref()
            .map(TempSensor::read)
            .unwrap_or(proto::NO_TEMP);
        payload.uptime_s = read_uptime_s();
        payload.niface = ifaces.len().min(proto::MAX_IFACES) as u8;
        payload.time_hms = local_hms();
        payload.ifaces = [Iface::default(); proto::MAX_IFACES];
        for (slot, iface) in payload.ifaces.iter_mut().zip(ifaces.iter()) {
            *slot = *iface;
        }

        payload.encode(&mut frame);
        seq = seq.wrapping_add(1);

        if args.dry_run {
            if args.print_every > 0.0
                && now.duration_since(last_print).as_secs_f64() >= args.print_every
            {
                last_print = now;
                let nets: Vec<String> = ifaces
                    .iter()
                    .map(|i| {
                        let name = String::from_utf8_lossy(&i.name)
                            .trim_end_matches('\0')
                            .to_string();
                        format!(
                            "{name} {}/{}",
                            human_rate(i.rx_bps as f64),
                            human_rate(i.tx_bps as f64)
                        )
                    })
                    .collect();
                writeln!(
                    out,
                    "cpu={:5.1}%  mem={:.1}G  temp={:.1}C  {}",
                    avg,
                    mi.used_mb as f64 / 1024.0,
                    payload.temp_x10 as f64 / 10.0,
                    nets.join("  ")
                )?;
                out.flush()?;
            }
        } else if let Some(link) = link.as_mut() {
            if link.write_frame(&frame) {
                sent += 1;
            } else {
                dropped += 1;
            }
        }

        // Pick up the board's STAT lines every so often.
        if let Some(link) = link.as_mut() {
            if seq % 15 == 0 {
                link.drain_into(&mut rx)?;
                if !rx.is_empty() {
                    rx_text.clear();
                    rx_text.push_str(&String::from_utf8_lossy(&rx));
                    for line in rx_text.lines() {
                        let line = line.trim_end_matches(['\r', '\n']).trim();
                        if !line.is_empty() && !args.quiet {
                            writeln!(out, "[board] {line}")?;
                        }
                    }
                    out.flush()?;
                }
            }
        }

        let after = Instant::now();
        if args.report_every > 0.0
            && after.duration_since(last_report).as_secs_f64() >= args.report_every
        {
            let dt = after.duration_since(last_report).as_secs_f64();
            writeln!(
                out,
                "[host] {:5.1} frames/s sent, {} dropped, {} late",
                sent as f64 / dt,
                dropped,
                late
            )?;
            out.flush()?;
            sent = 0;
            dropped = 0;
            late = 0;
            last_report = after;
        }

        let now = Instant::now();
        if next > now {
            std::thread::sleep(next - now);
        } else {
            late += 1;
            if now.duration_since(next) > Duration::from_millis(500) {
                next = now;
            }
        }
    }
}

fn main() -> ExitCode {
    let args = match parse_args() {
        Ok(a) => a,
        Err(e) => {
            eprintln!("error: {e}\n\n{USAGE}");
            return ExitCode::from(2);
        }
    };
    if args.help {
        print!("{USAGE}");
        return ExitCode::SUCCESS;
    }
    match run(args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("[host] fatal: {e}");
            ExitCode::FAILURE
        }
    }
}
