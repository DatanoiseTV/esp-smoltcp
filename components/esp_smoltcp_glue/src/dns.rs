//! DNS resolver using smoltcp's built-in `socket-dns` (which we now enable
//! as a feature). This replaces the previous hand-rolled UDP/53 parser:
//! smoltcp's resolver supports retry, multi-server, optional AAAA, and
//! handles the wire format / response validation properly.
//!
//! Public C API (unchanged signature, behaviour now stronger):
//!
//!   int smoltcp_resolve(IfaceCtx*, const char *name,
//!                       uint32_t server_be, uint32_t timeout_ms,
//!                       uint32_t *out_be);
//!
//! Returns 0 + writes IPv4 (network byte order) on success, -1 on error.
//! `server_be` is the DNS server (network-byte-order octets in a u32);
//! pass 0 to use the smoltcp default. Hostname or dotted-quad accepted.

use core::ffi::{c_char, c_int, CStr};
use alloc::vec;

use smoltcp::socket::dns;
use smoltcp::wire::{DnsQueryType, IpAddress, Ipv4Address};

use crate::IfaceCtx;

#[no_mangle]
pub unsafe extern "C" fn smoltcp_resolve(h: *mut IfaceCtx,
                                         name: *const c_char,
                                         server_be: u32,
                                         timeout_ms: u32,
                                         out_be: *mut u32) -> c_int {
    if h.is_null() || name.is_null() || out_be.is_null() { return -1; }
    let cs = match CStr::from_ptr(name).to_str() { Ok(v) => v, Err(_) => return -1 };

    // Dotted-quad fast-path — no DNS round-trip needed.
    if let Ok(addr) = cs.parse::<core::net::Ipv4Addr>() {
        *out_be = u32::from_le_bytes(addr.octets());
        return 0;
    }

    let ctx = &mut *h;

    // One DNS server. If the caller passed 0 we default to 1.1.1.1; the
    // app can override via the public netdb_wrap → smoltcp_resolve path.
    let server: IpAddress = if server_be == 0 {
        Ipv4Address::from_octets([1, 1, 1, 1]).into()
    } else {
        Ipv4Address::from_octets(server_be.to_le_bytes()).into()
    };
    let servers = [server];

    // 4 in-flight query slots; one is enough for sync use but headroom
    // is cheap.
    let queries = vec![None; 4];
    let mut sock = dns::Socket::new(&servers, queries);

    let cx = ctx.iface.context();
    let qh = match sock.start_query(cx, cs, DnsQueryType::A) {
        Ok(h) => h,
        Err(_) => return -1,
    };
    let handle = ctx.sockets.add(sock);
    let deadline = crate::now() + smoltcp::time::Duration::from_millis(timeout_ms as u64);

    let result: Option<Ipv4Address> = loop {
        ctx.iface.poll(crate::now(), &mut ctx.dev, &mut ctx.sockets);
        let s = ctx.sockets.get_mut::<dns::Socket>(handle);
        match s.get_query_result(qh) {
            Ok(addrs) => {
                let mut found = None;
                for a in addrs.iter() {
                    if let IpAddress::Ipv4(v4) = a {
                        found = Some(*v4);
                        break;
                    }
                }
                break found;
            }
            Err(dns::GetQueryResultError::Pending) => {}
            Err(_) => break None,
        }
        if crate::now() >= deadline { break None; }
    };

    // The Pending case left the query open; cancel before dropping the
    // socket so smoltcp doesn't keep retransmitting after we're gone.
    let s = ctx.sockets.get_mut::<dns::Socket>(handle);
    s.cancel_query(qh);
    ctx.sockets.remove(handle);

    match result {
        Some(addr) => { *out_be = u32::from_le_bytes(addr.octets()); 0 }
        None => -1,
    }
}
