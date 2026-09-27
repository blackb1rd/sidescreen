//! Announces this computer with a small UDP message every second on each network it is on, so a
//! tablet finds it where Bonjour doesn't reach, notably when the computer has joined the
//! tablet's own hotspot (no router). Payload: "SSB1" + the host name the tablet knows from
//! pairing. IPv4 broadcast, and IPv6 all-nodes multicast (ff02::1) for IPv6-only networks.
//! Same as the Mac app's Beacon.swift.

use std::collections::HashSet;
use std::net::{Ipv4Addr, Ipv6Addr, SocketAddr, SocketAddrV4, SocketAddrV6, UdpSocket};
use std::time::Duration;

const MAGIC: &[u8] = b"SSB1";

pub fn start(host_name: &str, port: u16) {
    let mut payload = MAGIC.to_vec();
    payload.extend(host_name.as_bytes().iter().take(200));
    std::thread::spawn(move || {
        let v4 = UdpSocket::bind("0.0.0.0:0").ok();
        if let Some(s) = &v4 {
            let _ = s.set_broadcast(true);
        }
        let v6 = UdpSocket::bind("[::]:0").ok();
        loop {
            announce(&payload, port, v4.as_ref(), v6.as_ref());
            std::thread::sleep(Duration::from_secs(1));
        }
    });
}

fn announce(payload: &[u8], port: u16, v4: Option<&UdpSocket>, v6: Option<&UdpSocket>) {
    let Ok(interfaces) = if_addrs::get_if_addrs() else {
        return;
    };
    let mut v6_done = HashSet::new();
    for i in interfaces.iter().filter(|i| !i.is_loopback()) {
        match &i.addr {
            if_addrs::IfAddr::V4(a) => {
                // Windows may not report the subnet's broadcast address: the general one then.
                if let Some(s) = v4 {
                    let b = a.broadcast.unwrap_or(Ipv4Addr::BROADCAST);
                    let _ = s.send_to(payload, SocketAddr::V4(SocketAddrV4::new(b, port)));
                }
            }
            if_addrs::IfAddr::V6(_) => {
                if let (Some(s), Some(index)) = (v6, i.index)
                    && v6_done.insert(index)
                {
                    let all_nodes = Ipv6Addr::new(0xff02, 0, 0, 0, 0, 0, 0, 1);
                    let _ = s.send_to(
                        payload,
                        SocketAddr::V6(SocketAddrV6::new(all_nodes, port, 0, index)),
                    );
                }
            }
        }
    }
}
