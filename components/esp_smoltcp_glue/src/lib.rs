//! C FFI shim that exposes smoltcp to ESP-IDF.
//!
//! Threading: not thread-safe by design. The C side serializes all calls
//! through the net_stack task. See smoltcp_glue.h.

#![no_std]
#![allow(clippy::missing_safety_doc)]

extern crate alloc;

use core::ffi::{c_char, c_int, CStr};
use core::ptr;

use alloc::boxed::Box;
use alloc::vec;
use alloc::vec::Vec;

use heapless::FnvIndexMap;
use smoltcp::iface::{Config, Interface, SocketHandle, SocketSet};
use smoltcp::phy::{self, Device, DeviceCapabilities, Medium};
use smoltcp::socket::{dhcpv4, tcp, udp};
use smoltcp::time::Instant;
use smoltcp::wire::{
    EthernetAddress, IpAddress, IpCidr, IpEndpoint, Ipv4Address, Ipv4Cidr,
    Ipv6Address, Ipv6Cidr,
};

mod alloc_shim;
mod panic;
mod dns;

// ---------------------------------------------------------------------------
// extern "C" callbacks supplied by the C side
// ---------------------------------------------------------------------------
extern "C" {
    fn smoltcp_glue_tx_cb(iface_id: u32, frame: *const u8, len: usize) -> c_int;
    fn smoltcp_glue_now_us() -> i64;
    pub(crate) fn smoltcp_glue_rand32() -> u32;
}

pub(crate) fn now() -> Instant {
    Instant::from_micros(unsafe { smoltcp_glue_now_us() })
}

// ---------------------------------------------------------------------------
// phy::Device implementation backed by C-side TX + an RX inbox
// ---------------------------------------------------------------------------
const MTU: usize = 1500;
const TX_FRAME: usize = 1536;       /* MTU + L2 headers, oversized */
const TX_POOL_N: usize = 16;        /* simultaneous in-flight TX frames */

/*
 * Static TX scratch pool. smoltcp's TxToken needs a writable buffer of
 * `len` bytes per packet; without a pool we'd `vec![0u8; len]` every
 * call, which (a) zeroes the buffer (cache miss) and (b) hits the heap
 * 5–10 K times/sec under load. With this pool the alloc is one bitmap
 * pop and a slice borrow.
 *
 * Single-threaded by virtue of being touched only under SMOLTCP_LOCK
 * on the C side. UnsafeCell wrapper keeps Rust 2024's static_mut_refs
 * lint happy without paying for a real lock we don't need.
 */
struct TxPool {
    bufs: [[u8; TX_FRAME]; TX_POOL_N],
    free_mask: u32,        /* bit i = buffer i is free */
}

struct TxPoolStorage(core::cell::UnsafeCell<TxPool>);
unsafe impl Sync for TxPoolStorage {}

static TX_POOL: TxPoolStorage = TxPoolStorage(core::cell::UnsafeCell::new(TxPool {
    bufs: [[0; TX_FRAME]; TX_POOL_N],
    free_mask: (1u32 << TX_POOL_N) - 1,
}));

unsafe fn tx_pool_alloc() -> Option<(usize, &'static mut [u8; TX_FRAME])> {
    let pool = &mut *TX_POOL.0.get();
    if pool.free_mask == 0 { return None; }
    let idx = pool.free_mask.trailing_zeros() as usize;
    pool.free_mask &= !(1u32 << idx);
    Some((idx, &mut pool.bufs[idx]))
}

unsafe fn tx_pool_free(idx: usize) {
    let pool = &mut *TX_POOL.0.get();
    pool.free_mask |= 1u32 << idx;
}

pub(crate) struct EspDevice {
    iface_id: u32,
    rx_inbox: Option<Vec<u8>>,
    pub(crate) link_up: bool,
}

impl EspDevice {
    fn new(iface_id: u32) -> Self {
        Self { iface_id, rx_inbox: None, link_up: false }
    }
    fn push_rx(&mut self, frame: &[u8]) {
        // If a previous frame is still pending we drop the new one. smoltcp
        // is polled immediately after each rx, so this should be rare.
        if self.rx_inbox.is_none() {
            self.rx_inbox = Some(frame.to_vec());
        }
    }
}

pub struct EspRxToken(Vec<u8>);
pub struct EspTxToken {
    iface_id: u32,
    pool_idx: usize,
    buf: &'static mut [u8; TX_FRAME],
}

impl phy::RxToken for EspRxToken {
    fn consume<R, F>(self, f: F) -> R
    where F: FnOnce(&[u8]) -> R {
        f(&self.0)
    }
}

impl phy::TxToken for EspTxToken {
    fn consume<R, F>(mut self, len: usize, f: F) -> R
    where F: FnOnce(&mut [u8]) -> R {
        let r = f(&mut self.buf[..len]);
        unsafe {
            // C side failure -> frame silently dropped; TCP will retx.
            let _ = smoltcp_glue_tx_cb(self.iface_id, self.buf.as_ptr(), len);
        }
        // self drops here -> Drop impl returns the buffer to the pool.
        r
    }
}

impl Drop for EspTxToken {
    /* CRITICAL: smoltcp may obtain a TxToken via Device::transmit() and
     * then drop it without calling consume() (e.g. when poll() probes
     * "is there anything to send" and finds nothing). Without this Drop
     * the pool slot leaks; after TX_POOL_N speculative drops the pool
     * is empty, transmit()/receive() both return None, and the entire
     * stack stops responding (ping stops, no replies). */
    fn drop(&mut self) {
        unsafe { tx_pool_free(self.pool_idx); }
    }
}

impl Device for EspDevice {
    type RxToken<'a> = EspRxToken where Self: 'a;
    type TxToken<'a> = EspTxToken where Self: 'a;

    fn capabilities(&self) -> DeviceCapabilities {
        let mut c = DeviceCapabilities::default();
        c.medium = Medium::Ethernet;
        c.max_transmission_unit = MTU;
        // None = unbounded burst — let smoltcp drain its TX queue in one
        // poll cycle instead of one packet per cycle. With per-cycle = 1
        // and a 1-2 ms poll cadence, peak throughput stays under 6 Mbit/s.
        c.max_burst_size = None;
        c
    }
    fn receive(&mut self, _t: Instant) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        let frame = self.rx_inbox.take()?;
        // Try to grab a TX buffer for the symmetric reply path. If the
        // pool is empty we skip RX delivery this cycle so smoltcp can
        // ack-or-retx without another pending alloc; the frame is held
        // in inbox until next poll.
        unsafe {
            let (idx, buf) = tx_pool_alloc()?;
            Some((EspRxToken(frame), EspTxToken {
                iface_id: self.iface_id, pool_idx: idx, buf,
            }))
        }
    }
    fn transmit(&mut self, _t: Instant) -> Option<Self::TxToken<'_>> {
        if !self.link_up { return None; }
        unsafe {
            let (idx, buf) = tx_pool_alloc()?;
            Some(EspTxToken { iface_id: self.iface_id, pool_idx: idx, buf })
        }
    }
}

// ---------------------------------------------------------------------------
// Interface bundle
// ---------------------------------------------------------------------------
// TX is also the advertised TCP window; bigger window = more in-flight
// per RTT and more bytes drained per poll cycle. 48 KiB TX × 4 listener
// pool slots + 1 active = 5 × 64 KiB ≈ 320 KiB out of ~580 KiB internal
// SRAM. Tight but fits, and the bigger window gives smoltcp more room
// to absorb the 32 KiB chunked-send bursts esp_http_server emits.
const TCP_RX: usize = 16 * 1024;
const TCP_TX: usize = 48 * 1024;
const UDP_RX: usize = 4 * 1024;
const UDP_TX: usize = 4 * 1024;
const UDP_META: usize = 16;

pub struct IfaceCtx {
    iface_id: u32,
    pub(crate) dev: EspDevice,
    pub(crate) iface: Interface,
    pub(crate) sockets: SocketSet<'static>,
    dhcp: Option<SocketHandle>,
    has_ip: bool,
    netmask_be: u32,
    gateway_be: u32,
    /// app socket id -> smoltcp handle (id 0 reserved as invalid)
    handles: FnvIndexMap<u32, SocketHandle, 32>,
    next_id: u32,
}

unsafe impl Send for IfaceCtx {}

impl IfaceCtx {
    fn new(iface_id: u32, mac: [u8; 6]) -> Self {
        let mut dev = EspDevice::new(iface_id);
        let cfg = Config::new(EthernetAddress(mac).into());
        let mut iface = Interface::new(cfg, &mut dev, now());
        let sockets = SocketSet::new(vec![]);

        // IPv6 link-local from the MAC, modified-EUI-64 form.
        // FE80::/64 ; second-half is mac[0..3] + FF:FE + mac[3..6] with the
        // U/L bit flipped on the first byte (XOR 0x02).
        let ll = [
            0xfe, 0x80, 0, 0, 0, 0, 0, 0,
            mac[0] ^ 0x02, mac[1], mac[2], 0xff, 0xfe, mac[3], mac[4], mac[5],
        ];
        let ll_addr = Ipv6Address::from_octets(ll);
        iface.update_ip_addrs(|addrs| {
            let _ = addrs.push(IpCidr::Ipv6(Ipv6Cidr::new(ll_addr, 64)));
        });

        Self {
            iface_id,
            dev,
            iface,
            sockets,
            dhcp: None,
            has_ip: false,
            netmask_be: 0,
            gateway_be: 0,
            handles: FnvIndexMap::new(),
            next_id: 1,
        }
    }

    fn alloc_id(&mut self, h: SocketHandle) -> u32 {
        let id = self.next_id;
        self.next_id = self.next_id.wrapping_add(1).max(1);
        let _ = self.handles.insert(id, h);
        id
    }
    fn lookup(&self, id: u32) -> Option<SocketHandle> {
        self.handles.get(&id).copied()
    }

    fn poll(&mut self) {
        self.iface.poll(now(), &mut self.dev, &mut self.sockets);

        // DHCP: copy any newly-acquired lease into the interface.
        if let Some(dh) = self.dhcp {
            let s = self.sockets.get_mut::<dhcpv4::Socket>(dh);
            match s.poll() {
                None => {}
                Some(dhcpv4::Event::Configured(cfg)) => {
                    let cidr = cfg.address;
                    self.netmask_be = cidr_to_mask(cidr.prefix_len()).to_be();
                    self.iface.update_ip_addrs(|addrs| {
                        // Drop only existing IPv4 entries; keep the IPv6
                        // link-local we added at iface creation.
                        addrs.retain(|a| !matches!(a, IpCidr::Ipv4(_)));
                        let _ = addrs.push(IpCidr::Ipv4(cidr));
                    });
                    if let Some(gw) = cfg.router {
                        let _ = self.iface.routes_mut().add_default_ipv4_route(gw);
                        self.gateway_be = ipv4_to_be(gw);
                    } else {
                        self.gateway_be = 0;
                    }
                    self.has_ip = true;
                }
                Some(dhcpv4::Event::Deconfigured) => {
                    self.iface.update_ip_addrs(|addrs| {
                        addrs.retain(|a| !matches!(a, IpCidr::Ipv4(_)));
                    });
                    self.iface.routes_mut().remove_default_ipv4_route();
                    self.netmask_be = 0;
                    self.gateway_be = 0;
                    self.has_ip = false;
                }
            }
        } else {
            self.has_ip = self.iface.ipv4_addr().is_some();
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
fn cidr_to_mask(prefix: u8) -> u32 {
    if prefix == 0 { 0 } else { (!0u32) << (32 - prefix as u32) }
}

/*
 * The C FFI represents an IPv4 address as a uint32_t whose **bytes in
 * memory** are the dotted-quad in order (network-byte-order convention).
 * On little-endian targets that means the integer value's low byte is
 * the first octet — exactly what to_le_bytes / from_le_bytes give us.
 *
 * This is the same convention as `struct in_addr::s_addr` in <netinet/in.h>.
 */
fn ipv4_from_be(be: u32) -> Ipv4Address {
    Ipv4Address::from_octets(be.to_le_bytes())
}

fn ipv4_to_be(addr: Ipv4Address) -> u32 {
    u32::from_le_bytes(addr.octets())
}

// ---------------------------------------------------------------------------
// Public C-callable surface
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn smoltcp_core_init() {
    // Reserved for future global init.
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_new(iface_id: u32, mac: *const u8) -> *mut IfaceCtx {
    if mac.is_null() { return ptr::null_mut(); }
    let mut m = [0u8; 6];
    core::ptr::copy_nonoverlapping(mac, m.as_mut_ptr(), 6);
    let ctx = Box::new(IfaceCtx::new(iface_id, m));
    Box::into_raw(ctx)
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_destroy(h: *mut IfaceCtx) {
    if !h.is_null() { drop(Box::from_raw(h)); }
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_rx(h: *mut IfaceCtx, frame: *const u8, len: usize) {
    if h.is_null() || frame.is_null() { return; }
    let ctx = &mut *h;
    let slice = core::slice::from_raw_parts(frame, len);
    ctx.dev.push_rx(slice);
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_poll(h: *mut IfaceCtx) {
    if h.is_null() { return; }
    (&mut *h).poll();
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_poll_delay_us(h: *mut IfaceCtx) -> i64 {
    if h.is_null() { return -1; }
    let ctx = &mut *h;
    match ctx.iface.poll_delay(now(), &ctx.sockets) {
        Some(d) => d.total_micros() as i64,
        None => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_set_link_up(h: *mut IfaceCtx, up: bool) {
    if h.is_null() { return; }
    (&mut *h).dev.link_up = up;
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_has_ip(h: *mut IfaceCtx) -> bool {
    if h.is_null() { return false; }
    (&*h).has_ip
}

/* Number of currently-allocated app sockets (TCP + UDP) on this iface.
 * Used by /api/stats to spot socket leaks under sustained traffic. */
#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_socket_count(h: *mut IfaceCtx) -> u32 {
    if h.is_null() { return 0; }
    (&*h).handles.len() as u32
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_get_ipv4(h: *mut IfaceCtx) -> u32 {
    if h.is_null() { return 0; }
    match (&*h).iface.ipv4_addr() {
        Some(a) => ipv4_to_be(a),
        None => 0,
    }
}

/* Copy out the first non-loopback IPv6 address (typically the link-local
 * we created at iface init). `out` must point to at least 16 bytes.
 * Returns 1 on success, 0 if no v6 address is configured. */
#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_get_ipv6(h: *mut IfaceCtx, out: *mut u8) -> c_int {
    if h.is_null() || out.is_null() { return 0; }
    for cidr in (&*h).iface.ip_addrs() {
        if let IpCidr::Ipv6(c) = cidr {
            let octets = c.address().octets();
            core::ptr::copy_nonoverlapping(octets.as_ptr(), out, 16);
            return 1;
        }
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_get_gw(h: *mut IfaceCtx) -> u32 {
    if h.is_null() { return 0; }
    (&*h).gateway_be
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_get_netmask(h: *mut IfaceCtx) -> u32 {
    if h.is_null() { return 0; }
    (&*h).netmask_be
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_set_static(h: *mut IfaceCtx,
                                                  ipv4_be: u32,
                                                  prefix_len: u32,
                                                  gw_be: u32) {
    if h.is_null() { return; }
    let ctx = &mut *h;
    ctx.dhcp.take();
    let addr = ipv4_from_be(ipv4_be);
    let cidr = Ipv4Cidr::new(addr, prefix_len as u8);
    ctx.iface.update_ip_addrs(|a| {
        a.retain(|cur| !matches!(cur, IpCidr::Ipv4(_)));   // keep v6 LL
        let _ = a.push(IpCidr::Ipv4(cidr));
    });
    ctx.netmask_be = cidr_to_mask(prefix_len as u8).to_be();
    if gw_be != 0 {
        let gw = ipv4_from_be(gw_be);
        let _ = ctx.iface.routes_mut().add_default_ipv4_route(gw);
        ctx.gateway_be = gw_be;
    }
    ctx.has_ip = true;
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_enable_dhcp(h: *mut IfaceCtx) {
    if h.is_null() { return; }
    let ctx = &mut *h;
    if ctx.dhcp.is_some() { return; }
    let s = dhcpv4::Socket::new();
    let dh = ctx.sockets.add(s);
    ctx.dhcp = Some(dh);
}

// ---------------------------------------------------------------------------
// Multicast (IGMP)
// ---------------------------------------------------------------------------
#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_mcast_join(h: *mut IfaceCtx, group_be: u32) -> c_int {
    if h.is_null() { return -1; }
    let ctx = &mut *h;
    let g = ipv4_from_be(group_be);
    match ctx.iface.join_multicast_group(g) { Ok(_) => 0, Err(_) => -1 }
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_iface_mcast_leave(h: *mut IfaceCtx, group_be: u32) -> c_int {
    if h.is_null() { return -1; }
    let ctx = &mut *h;
    let g = ipv4_from_be(group_be);
    match ctx.iface.leave_multicast_group(g) { Ok(_) => 0, Err(_) => -1 }
}

// ---------------------------------------------------------------------------
// TCP
// ---------------------------------------------------------------------------
#[no_mangle]
pub unsafe extern "C" fn smoltcp_tcp_open(h: *mut IfaceCtx) -> u32 {
    if h.is_null() { return 0; }
    let ctx = &mut *h;
    let rx = tcp::SocketBuffer::new(vec![0u8; TCP_RX]);
    let tx = tcp::SocketBuffer::new(vec![0u8; TCP_TX]);
    let s = tcp::Socket::new(rx, tx);
    let sh = ctx.sockets.add(s);
    ctx.alloc_id(sh)
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_tcp_connect(h: *mut IfaceCtx, id: u32,
                                             dst_be: u32, dst_port: u16,
                                             local_port: u16) -> c_int {
    if h.is_null() { return -1; }
    let ctx = &mut *h;
    let Some(sh) = ctx.lookup(id) else { return -1; };
    let dst: IpAddress = ipv4_from_be(dst_be).into();
    let local = if local_port == 0 {
        49152u16.wrapping_add((smoltcp_glue_rand32() & 0x3FFF) as u16)
    } else { local_port };
    let cx = ctx.iface.context();
    let s = ctx.sockets.get_mut::<tcp::Socket>(sh);
    match s.connect(cx, (dst, dst_port), local) { Ok(_) => 0, Err(_) => -1 }
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_tcp_listen(h: *mut IfaceCtx, id: u32, port: u16) -> c_int {
    if h.is_null() { return -1; }
    let ctx = &mut *h;
    let Some(sh) = ctx.lookup(id) else { return -1; };
    let s = ctx.sockets.get_mut::<tcp::Socket>(sh);
    match s.listen(port) { Ok(_) => 0, Err(_) => -1 }
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_tcp_send(h: *mut IfaceCtx, id: u32,
                                          buf: *const u8, len: usize) -> c_int {
    if h.is_null() || buf.is_null() { return -1; }
    let ctx = &mut *h;
    let Some(sh) = ctx.lookup(id) else { return -1; };
    let slice = core::slice::from_raw_parts(buf, len);
    let s = ctx.sockets.get_mut::<tcp::Socket>(sh);
    match s.send_slice(slice) { Ok(n) => n as c_int, Err(_) => -1 }
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_tcp_recv(h: *mut IfaceCtx, id: u32,
                                          buf: *mut u8, cap: usize) -> c_int {
    if h.is_null() || buf.is_null() { return -1; }
    let ctx = &mut *h;
    let Some(sh) = ctx.lookup(id) else { return -1; };
    let slice = core::slice::from_raw_parts_mut(buf, cap);
    let s = ctx.sockets.get_mut::<tcp::Socket>(sh);
    match s.recv_slice(slice) { Ok(n) => n as c_int, Err(_) => -1 }
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_tcp_is_active(h: *mut IfaceCtx, id: u32) -> bool {
    if h.is_null() { return false; }
    let ctx = &mut *h;
    let Some(sh) = ctx.lookup(id) else { return false; };
    let s = ctx.sockets.get_mut::<tcp::Socket>(sh);
    s.is_active()
}

/* True only when the three-way handshake is complete and data can flow.
 *
 * may_recv() returns true once the socket is past SYN-RECEIVED — i.e.
 * the connection is genuinely ESTABLISHED. Using is_active() here would
 * fire accept() during SYN-RECEIVED, before the peer's final ACK has
 * been seen; httpd then recv()s on an incomplete connection, gets zero
 * bytes, treats the socket as closed, and the request is dropped. */
#[no_mangle]
pub unsafe extern "C" fn smoltcp_tcp_is_connected(h: *mut IfaceCtx, id: u32) -> bool {
    if h.is_null() { return false; }
    let ctx = &mut *h;
    let Some(sh) = ctx.lookup(id) else { return false; };
    let s = ctx.sockets.get_mut::<tcp::Socket>(sh);
    s.may_recv() || s.may_send()
}

/* Bytes currently buffered in the TCP recv queue (data ready for recv). */
#[no_mangle]
pub unsafe extern "C" fn smoltcp_tcp_recv_queue(h: *mut IfaceCtx, id: u32) -> usize {
    if h.is_null() { return 0; }
    let ctx = &mut *h;
    let Some(sh) = ctx.lookup(id) else { return 0; };
    ctx.sockets.get_mut::<tcp::Socket>(sh).recv_queue()
}

/* Bytes of free space in the TCP send buffer (room to send). */
#[no_mangle]
pub unsafe extern "C" fn smoltcp_tcp_send_capacity(h: *mut IfaceCtx, id: u32) -> usize {
    if h.is_null() { return 0; }
    let ctx = &mut *h;
    let Some(sh) = ctx.lookup(id) else { return 0; };
    let s = ctx.sockets.get_mut::<tcp::Socket>(sh);
    if s.can_send() { s.send_capacity().saturating_sub(s.send_queue()) } else { 0 }
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_tcp_close(h: *mut IfaceCtx, id: u32) {
    if h.is_null() { return; }
    let ctx = &mut *h;
    if let Some(sh) = ctx.lookup(id) {
        ctx.sockets.get_mut::<tcp::Socket>(sh).close();
        ctx.sockets.remove(sh);
        ctx.handles.remove(&id);
    }
}

// ---------------------------------------------------------------------------
// UDP
// ---------------------------------------------------------------------------
#[no_mangle]
pub unsafe extern "C" fn smoltcp_udp_open(h: *mut IfaceCtx, local_port: u16) -> u32 {
    if h.is_null() { return 0; }
    let ctx = &mut *h;
    let rx_meta = vec![udp::PacketMetadata::EMPTY; UDP_META];
    let tx_meta = vec![udp::PacketMetadata::EMPTY; UDP_META];
    let rx = udp::PacketBuffer::new(rx_meta, vec![0u8; UDP_RX]);
    let tx = udp::PacketBuffer::new(tx_meta, vec![0u8; UDP_TX]);
    let mut s = udp::Socket::new(rx, tx);
    if local_port != 0 && s.bind(local_port).is_err() { return 0; }
    let sh = ctx.sockets.add(s);
    ctx.alloc_id(sh)
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_udp_sendto(h: *mut IfaceCtx, id: u32,
                                            buf: *const u8, len: usize,
                                            dst_be: u32, dst_port: u16) -> c_int {
    if h.is_null() || buf.is_null() { return -1; }
    let ctx = &mut *h;
    let Some(sh) = ctx.lookup(id) else { return -1; };
    let slice = core::slice::from_raw_parts(buf, len);
    let dst: IpAddress = ipv4_from_be(dst_be).into();
    let endpoint = IpEndpoint::new(dst, dst_port);
    let s = ctx.sockets.get_mut::<udp::Socket>(sh);
    match s.send_slice(slice, endpoint) {
        Ok(_) => len as c_int,
        Err(_) => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_udp_recvfrom(h: *mut IfaceCtx, id: u32,
                                              buf: *mut u8, cap: usize,
                                              src_be: *mut u32, src_port: *mut u16) -> c_int {
    if h.is_null() || buf.is_null() { return -1; }
    let ctx = &mut *h;
    let Some(sh) = ctx.lookup(id) else { return -1; };
    let slice = core::slice::from_raw_parts_mut(buf, cap);
    let s = ctx.sockets.get_mut::<udp::Socket>(sh);
    match s.recv_slice(slice) {
        Ok((n, meta)) => {
            if let IpAddress::Ipv4(a) = meta.endpoint.addr {
                if !src_be.is_null() {
                    *src_be = ipv4_to_be(a);
                }
            }
            if !src_port.is_null() { *src_port = meta.endpoint.port; }
            n as c_int
        }
        Err(_) => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn smoltcp_udp_close(h: *mut IfaceCtx, id: u32) {
    if h.is_null() { return; }
    let ctx = &mut *h;
    if let Some(sh) = ctx.lookup(id) {
        ctx.sockets.get_mut::<udp::Socket>(sh).close();
        ctx.sockets.remove(sh);
        ctx.handles.remove(&id);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
#[no_mangle]
pub unsafe extern "C" fn smoltcp_parse_ipv4(s: *const c_char, out_be: *mut u32) -> c_int {
    if s.is_null() || out_be.is_null() { return -1; }
    let cs = match CStr::from_ptr(s).to_str() { Ok(v) => v, Err(_) => return -1 };
    let mut octets = [0u8; 4];
    let mut i = 0;
    for part in cs.split('.') {
        if i >= 4 { return -1; }
        match part.parse::<u8>() {
            Ok(v) => { octets[i] = v; i += 1; }
            Err(_) => return -1,
        }
    }
    if i != 4 { return -1; }
    *out_be = u32::from_be_bytes(octets);
    0
}
