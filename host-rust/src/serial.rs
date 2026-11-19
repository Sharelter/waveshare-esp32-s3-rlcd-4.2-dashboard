// SPDX-License-Identifier: Apache-2.0
//! Raw termios access to the board's USB-Serial-JTAG tty.
//!
//! Deliberately no serial crate: we only need `open` + `read` + `write` on a
//! raw, non-blocking fd, which `libc` covers directly.

use std::ffi::CString;
use std::io;
use std::os::fd::RawFd;
use std::thread::sleep;
use std::time::{Duration, Instant};

pub struct SerialLink {
    path: String,
    fd: RawFd,
    /// At most one frame is ever queued, so a backed up link costs one frame of
    /// latency instead of growing an unbounded backlog.
    pending: Vec<u8>,
}

impl SerialLink {
    pub fn open(path: &str) -> io::Result<Self> {
        let fd = open_raw(path)?;
        Ok(Self {
            path: path.to_string(),
            fd,
            pending: Vec::with_capacity(crate::proto::FRAME_LEN),
        })
    }

    /// Like [`open`], but tolerant of the board not being enumerated yet:
    /// `Some(0)` waits forever, `Some(n)` waits up to `n` seconds, `None` fails
    /// immediately.  This is what makes the unit usable as a boot service.
    pub fn open_waiting(path: &str, wait_secs: Option<u64>) -> io::Result<Self> {
        let Some(secs) = wait_secs else {
            return Self::open(path);
        };
        let deadline = if secs == 0 {
            None
        } else {
            Some(Instant::now() + Duration::from_secs(secs))
        };
        let mut announced = false;
        loop {
            match Self::open(path) {
                Ok(link) => {
                    if announced {
                        eprintln!("[host] {path}: device is back");
                    }
                    return Ok(link);
                }
                Err(err) => {
                    if !announced {
                        eprintln!("[host] {path}: {err} -- waiting for the device");
                        announced = true;
                    }
                    if let Some(dl) = deadline {
                        if Instant::now() >= dl {
                            return Err(err);
                        }
                    }
                    sleep(Duration::from_millis(500));
                }
            }
        }
    }

    /// Reopen after the device re-enumerated.  Retries indefinitely: a service
    /// should ride out an unplug rather than die and rely on Restart=.
    pub fn reopen(&mut self) {
        unsafe { libc::close(self.fd) };
        self.fd = -1;
        self.pending.clear();

        let mut attempts = 0u32;
        loop {
            match open_raw(&self.path) {
                Ok(fd) => {
                    self.fd = fd;
                    if attempts > 0 {
                        eprintln!(
                            "[host] {}: reconnected after {} attempts",
                            self.path, attempts
                        );
                    }
                    return;
                }
                Err(_) => {
                    attempts += 1;
                    if attempts % 20 == 1 {
                        eprintln!(
                            "[host] {}: disconnected, waiting for it to come back",
                            self.path
                        );
                    }
                    sleep(Duration::from_millis(500));
                }
            }
        }
    }

    /// Non blocking write.  Returns false if a frame had to be dropped.
    pub fn write_frame(&mut self, frame: &[u8]) -> bool {
        let ok = self.pending.is_empty();
        self.pending.clear();
        self.pending.extend_from_slice(frame);

        while !self.pending.is_empty() {
            let n = unsafe {
                libc::write(
                    self.fd,
                    self.pending.as_ptr() as *const libc::c_void,
                    self.pending.len(),
                )
            };
            if n < 0 {
                match io::Error::last_os_error().raw_os_error() {
                    Some(libc::EAGAIN) => return false,
                    Some(libc::EIO) | Some(libc::ENODEV) | Some(libc::EBADF) => {
                        self.pending.clear();
                        self.reopen();
                        return false;
                    }
                    _ => return false,
                }
            }
            if n == 0 {
                return false;
            }
            self.pending.drain(..n as usize);
        }
        ok
    }

    /// Read whatever the board has sent; appends to `out` (cleared first).
    pub fn drain_into(&mut self, out: &mut Vec<u8>) -> io::Result<()> {
        out.clear();
        let mut buf = [0u8; 4096];
        loop {
            let n =
                unsafe { libc::read(self.fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len()) };
            if n < 0 {
                match io::Error::last_os_error().raw_os_error() {
                    Some(libc::EAGAIN) | Some(libc::EIO) | Some(libc::ENODEV) => break,
                    other => {
                        if let Some(code) = other {
                            return Err(io::Error::from_raw_os_error(code));
                        }
                        break;
                    }
                }
            }
            if n == 0 {
                break;
            }
            out.extend_from_slice(&buf[..n as usize]);
        }
        Ok(())
    }
}

impl Drop for SerialLink {
    fn drop(&mut self) {
        if self.fd >= 0 {
            unsafe { libc::close(self.fd) };
        }
    }
}

fn open_raw(path: &str) -> io::Result<RawFd> {
    let c_path = CString::new(path)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "port path contains NUL"))?;

    let fd = unsafe {
        libc::open(
            c_path.as_ptr(),
            libc::O_RDWR | libc::O_NOCTTY | libc::O_NONBLOCK,
        )
    };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }

    // SAFETY: `fd` is a fresh valid descriptor and `termios` is a plain C struct.
    unsafe {
        let mut t: libc::termios = std::mem::zeroed();
        if libc::tcgetattr(fd, &mut t) != 0 {
            let err = io::Error::last_os_error();
            libc::close(fd);
            return Err(err);
        }
        // raw, 8N1, no flow control, no echo, no signal generation.
        t.c_iflag = 0;
        t.c_oflag = 0;
        t.c_cflag = libc::CS8 | libc::CREAD | libc::CLOCAL;
        t.c_lflag = 0;
        t.c_cc[libc::VMIN] = 0;
        t.c_cc[libc::VTIME] = 0;
        // USB CDC ignores the rate, but set a conventional 115200 anyway.
        libc::cfsetispeed(&mut t, libc::B115200);
        libc::cfsetospeed(&mut t, libc::B115200);

        if libc::tcsetattr(fd, libc::TCSANOW, &t) != 0 {
            let err = io::Error::last_os_error();
            libc::close(fd);
            return Err(err);
        }
        libc::tcflush(fd, libc::TCIOFLUSH);
    }
    Ok(fd)
}
