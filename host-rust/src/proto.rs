// SPDX-License-Identifier: Apache-2.0
//! Wire protocol shared with the ESP32 firmware.
//!
//! Keep this in lockstep with `firmware/rlcd_dash/protocol.h` -- the layout is
//! asserted below so a change on one side cannot silently drift from the other.
//!
//! Frame: `A5 5A <len:u8> <payload:len> <crc16:u16 LE>` with `len = 167`.

pub const MAGIC: [u8; 2] = [0xA5, 0x5A];
pub const MAX_CPUS: usize = 32;
pub const MAX_IFACES: usize = 4;
pub const IFNAME_LEN: usize = 8;
pub const HOST_LEN: usize = 16;
/// Sentinel the firmware renders as "CPU --".
pub const NO_TEMP: i16 = i16::MIN;

pub const PAYLOAD_LEN: usize = 167;
pub const FRAME_LEN: usize = PAYLOAD_LEN + 5;

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection).
const CRC_TABLE: [u16; 256] = build_crc_table();

const fn build_crc_table() -> [u16; 256] {
    let mut table = [0u16; 256];
    let mut i = 0usize;
    while i < 256 {
        let mut crc = (i as u16) << 8;
        let mut bit = 0;
        while bit < 8 {
            crc = if crc & 0x8000 != 0 {
                (crc << 1) ^ 0x1021
            } else {
                crc << 1
            };
            bit += 1;
        }
        table[i] = crc;
        i += 1;
    }
    table
}

pub fn crc16(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xFFFF;
    for &byte in data {
        crc = (crc << 8) ^ CRC_TABLE[((crc >> 8) as u8 ^ byte) as usize];
    }
    crc
}

fn fixed_str<const N: usize>(s: &str) -> [u8; N] {
    let mut out = [0u8; N];
    let bytes = s.as_bytes();
    let len = bytes.len().min(N);
    out[..len].copy_from_slice(&bytes[..len]);
    out
}

#[derive(Clone, Copy, Default)]
pub struct Iface {
    pub name: [u8; IFNAME_LEN],
    pub rx_bps: u32,
    pub tx_bps: u32,
    pub up: u8,
}

impl Iface {
    pub fn new(name: &str, rx_bps: u32, tx_bps: u32, up: bool) -> Self {
        Self {
            name: fixed_str::<IFNAME_LEN>(name),
            rx_bps,
            tx_bps,
            up: up as u8,
        }
    }
}

#[derive(Clone, Copy)]
pub struct Payload {
    pub seq: u16,
    pub ncpu: u16,
    pub cpu_total_x10: u16,
    pub cpu_load: [u8; MAX_CPUS],
    pub mem_used_mb: u32,
    pub mem_total_mb: u32,
    pub swap_used_mb: u32,
    pub swap_total_mb: u32,
    pub load: [u16; 3],
    pub temp_x10: i16,
    pub uptime_s: u32,
    pub niface: u8,
    pub flags: u8,
    pub time_hms: [u8; 3],
    pub host: [u8; HOST_LEN],
    pub ifaces: [Iface; MAX_IFACES],
}

impl Default for Payload {
    fn default() -> Self {
        Self {
            seq: 0,
            ncpu: 0,
            cpu_total_x10: 0,
            cpu_load: [0; MAX_CPUS],
            mem_used_mb: 0,
            mem_total_mb: 0,
            swap_used_mb: 0,
            swap_total_mb: 0,
            load: [0; 3],
            temp_x10: NO_TEMP,
            uptime_s: 0,
            niface: 0,
            flags: 0,
            time_hms: [0; 3],
            host: [0; HOST_LEN],
            ifaces: [Iface::default(); MAX_IFACES],
        }
    }
}

impl Payload {
    pub fn set_host(&mut self, name: &str) {
        self.host = fixed_str::<HOST_LEN>(name);
    }

    /// Serialise a complete wire frame into `out` (cleared first).
    pub fn encode(&self, out: &mut Vec<u8>) {
        out.clear();
        out.reserve(FRAME_LEN);
        out.extend_from_slice(&MAGIC);
        out.push(PAYLOAD_LEN as u8);

        let start = out.len();
        out.extend_from_slice(&self.seq.to_le_bytes());
        out.extend_from_slice(&self.ncpu.to_le_bytes());
        out.extend_from_slice(&self.cpu_total_x10.to_le_bytes());
        out.extend_from_slice(&self.cpu_load);
        out.extend_from_slice(&self.mem_used_mb.to_le_bytes());
        out.extend_from_slice(&self.mem_total_mb.to_le_bytes());
        out.extend_from_slice(&self.swap_used_mb.to_le_bytes());
        out.extend_from_slice(&self.swap_total_mb.to_le_bytes());
        out.extend_from_slice(&self.load[0].to_le_bytes());
        out.extend_from_slice(&self.load[1].to_le_bytes());
        out.extend_from_slice(&self.load[2].to_le_bytes());
        out.extend_from_slice(&self.temp_x10.to_le_bytes());
        out.extend_from_slice(&self.uptime_s.to_le_bytes());
        out.push(self.niface);
        out.push(self.flags);
        out.extend_from_slice(&self.time_hms);
        out.extend_from_slice(&self.host);
        for iface in &self.ifaces {
            out.extend_from_slice(&iface.name);
            out.extend_from_slice(&iface.rx_bps.to_le_bytes());
            out.extend_from_slice(&iface.tx_bps.to_le_bytes());
            out.push(iface.up);
            out.extend_from_slice(&[0, 0, 0]); // padding, matches the C struct
        }

        assert_eq!(
            out.len() - start,
            PAYLOAD_LEN,
            "payload layout drifted from protocol.h"
        );
        let crc = crc16(&out[start..]);
        out.extend_from_slice(&crc.to_le_bytes());
        assert_eq!(out.len(), FRAME_LEN);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc_matches_ccitt_false_check_value() {
        // The standard "123456789" check value for CRC-16/CCITT-FALSE.
        assert_eq!(crc16(b"123456789"), 0x29B1);
        assert_eq!(crc16(b""), 0xFFFF);
    }

    #[test]
    fn payload_is_167_bytes() {
        let mut buf = Vec::new();
        Payload::default().encode(&mut buf);
        assert_eq!(buf.len(), FRAME_LEN);
        assert_eq!(buf[0], 0xA5);
        assert_eq!(buf[1], 0x5A);
        assert_eq!(buf[2] as usize, PAYLOAD_LEN);
        // every field is zero except the "no sensor" sentinel
        let mut expected = [0u8; PAYLOAD_LEN];
        expected[60..62].copy_from_slice(&NO_TEMP.to_le_bytes());
        assert_eq!(&buf[3..3 + PAYLOAD_LEN], &expected[..]);
        // pinned so an accidental layout change shows up as a byte diff
        assert_eq!(&buf[3 + PAYLOAD_LEN..], &0x3793u16.to_le_bytes());
    }

    #[test]
    fn field_offsets_match_protocol_h() {
        let mut buf = Vec::new();
        let mut p = Payload::default();
        p.seq = 0x1234;
        p.mem_total_mb = 0xDEADBEEF;
        p.ifaces[0] = Iface::new("wlan0", 111, 222, true);
        p.encode(&mut buf);
        let body = &buf[3..3 + PAYLOAD_LEN];
        assert_eq!(&body[0..2], &0x1234u16.to_le_bytes());
        // mem_used(38) mem_total(42): 2+2+2+32 = 38
        assert_eq!(&body[42..46], &0xDEADBEEFu32.to_le_bytes());
        // ifaces start at 87
        assert_eq!(&body[87..92], b"wlan0");
        assert_eq!(&body[95..99], &111u32.to_le_bytes());
        assert_eq!(body[103], 1);
    }
}
