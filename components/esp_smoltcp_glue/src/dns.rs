//! Minimal DNS A-record resolver over UDP/53.
//!
//! Single-shot UDP query, parses the first A record from the answer
//! section. No caching, no AAAA, no retry across multiple servers, no
//! response-id mismatch retry. Smallest viable resolver to unblock
//! getaddrinfo() in the lwIP compat shim.

use core::ffi::{c_char, c_int, CStr};
use alloc::vec;

use smoltcp::socket::udp;
use smoltcp::wire::{IpAddress, IpEndpoint, Ipv4Address};

use crate::IfaceCtx;

#[no_mangle]
pub unsafe extern "C" fn smoltcp_resolve(h: *mut IfaceCtx,
                                         name: *const c_char,
                                         server_be: u32,
                                         timeout_ms: u32,
                                         out_be: *mut u32) -> c_int {
    if h.is_null() || name.is_null() || out_be.is_null() { return -1; }
    let cs = match CStr::from_ptr(name).to_str() { Ok(v) => v, Err(_) => return -1 };

    // Dotted-quad shortcut. The C contract is "uint32 with octets in
    // network byte order in memory" — on little-endian that means
    // from_le_bytes of the octets.
    if let Ok(addr) = cs.parse::<core::net::Ipv4Addr>() {
        *out_be = u32::from_le_bytes(addr.octets());
        return 0;
    }

    let ctx = &mut *h;
    let id = (crate::smoltcp_glue_rand32() & 0xFFFF) as u16;

    let mut query = alloc::vec::Vec::with_capacity(64);
    query.extend_from_slice(&id.to_be_bytes());
    query.extend_from_slice(&[0x01, 0x00]);              // standard query, RD=1
    query.extend_from_slice(&[0, 1, 0, 0, 0, 0, 0, 0]);  // 1 question
    for label in cs.split('.') {
        if label.is_empty() || label.len() > 63 { return -1; }
        query.push(label.len() as u8);
        query.extend_from_slice(label.as_bytes());
    }
    query.push(0);
    query.extend_from_slice(&[0, 1]);                     // QTYPE A
    query.extend_from_slice(&[0, 1]);                     // QCLASS IN

    let rx_meta = vec![udp::PacketMetadata::EMPTY; 2];
    let tx_meta = vec![udp::PacketMetadata::EMPTY; 2];
    let rx = udp::PacketBuffer::new(rx_meta, vec![0u8; 512]);
    let tx = udp::PacketBuffer::new(tx_meta, vec![0u8; 512]);
    let mut sock = udp::Socket::new(rx, tx);
    let local: u16 = 49152u16.wrapping_add((crate::smoltcp_glue_rand32() & 0x3FFF) as u16);
    if sock.bind(local).is_err() { return -1; }

    let dst: IpAddress = Ipv4Address::from_octets(server_be.to_le_bytes()).into();
    if sock.send_slice(&query, IpEndpoint::new(dst, 53)).is_err() { return -1; }

    let handle = ctx.sockets.add(sock);
    let deadline = crate::now() + smoltcp::time::Duration::from_millis(timeout_ms as u64);

    let result = loop {
        ctx.iface.poll(crate::now(), &mut ctx.dev, &mut ctx.sockets);
        let s = ctx.sockets.get_mut::<udp::Socket>(handle);
        let mut buf = [0u8; 512];
        if let Ok((n, _meta)) = s.recv_slice(&mut buf) {
            if let Some(addr) = parse_a_response(&buf[..n], id) {
                break Some(addr);
            }
        }
        if crate::now() >= deadline { break None; }
    };

    ctx.sockets.remove(handle);

    match result {
        Some(addr) => { *out_be = u32::from_le_bytes(addr.octets()); 0 }
        None => -1,
    }
}

fn parse_a_response(pkt: &[u8], expect_id: u16) -> Option<Ipv4Address> {
    if pkt.len() < 12 { return None; }
    if u16::from_be_bytes([pkt[0], pkt[1]]) != expect_id { return None; }
    let flags = u16::from_be_bytes([pkt[2], pkt[3]]);
    if flags & 0x000F != 0 { return None; }
    let ancount = u16::from_be_bytes([pkt[6], pkt[7]]);
    if ancount == 0 { return None; }

    let mut i = 12usize;
    while i < pkt.len() && pkt[i] != 0 {
        let l = pkt[i] as usize;
        if l & 0xC0 != 0 { return None; }
        i += 1 + l;
    }
    i += 1 + 4;

    for _ in 0..ancount {
        if i + 12 > pkt.len() { return None; }
        if pkt[i] & 0xC0 == 0xC0 { i += 2; }
        else {
            while i < pkt.len() && pkt[i] != 0 { i += 1 + pkt[i] as usize; }
            i += 1;
        }
        if i + 10 > pkt.len() { return None; }
        let qtype  = u16::from_be_bytes([pkt[i],   pkt[i+1]]);
        let rdlen  = u16::from_be_bytes([pkt[i+8], pkt[i+9]]) as usize;
        i += 10;
        if i + rdlen > pkt.len() { return None; }
        if qtype == 1 && rdlen == 4 {
            return Some(Ipv4Address::from_octets([pkt[i], pkt[i+1], pkt[i+2], pkt[i+3]]));
        }
        i += rdlen;
    }
    None
}
