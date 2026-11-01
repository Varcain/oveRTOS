// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#[cfg(test)]
mod tests {
    use ove::{Error, ffi, net_httpd as http, net_tls, nvs};
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::time::Duration;

    unsafe extern "C" {
        fn ove_test_tls_start() -> i32;
        fn ove_test_tls_finish() -> i32;
    }

    fn port() -> u16 {
        TcpListener::bind("127.0.0.1:0")
            .unwrap()
            .local_addr()
            .unwrap()
            .port()
    }
    fn client(port: u16) -> TcpStream {
        let stream = TcpStream::connect(("127.0.0.1", port)).unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(5)))
            .unwrap();
        stream
    }
    unsafe extern "C" fn handler(
        _: *mut ffi::ove_httpd_req_t,
        raw: *mut ffi::ove_httpd_resp_t,
    ) -> i32 {
        // SAFETY: the server supplies a live response for this callback only.
        let response = unsafe { http::Response::from_raw(raw) };
        let result = (|| {
            if response.send(200, "text/plain\0bad", b"bad") != Err(Error::InvalidParam) {
                return Err(Error::InvalidParam);
            }
            if response.error_fmt(400, format_args!("{}", "\"".repeat(96))) != Err(Error::NoSpace) {
                return Err(Error::InvalidParam);
            }
            response.json(200, b"A\0B")
        })();
        result.map_or_else(Error::to_code, |()| 0)
    }

    #[test]
    fn rust_response_metadata_and_binary_json() {
        assert_eq!(
            http::route("GET\0bad", "/bad", Some(handler)),
            Err(Error::InvalidParam)
        );
        assert_eq!(
            http::route("TOOLONG!", "/bad", Some(handler)),
            Err(Error::NameTooLong)
        );
        assert_eq!(
            http::route("GET", &"p".repeat(64), Some(handler)),
            Err(Error::NameTooLong)
        );
        let method = String::from("GETsuffix");
        let path = String::from("/rust-binarysuffix");
        http::route(&method[..3], &path[..12], Some(handler)).unwrap();
        drop((method, path));
        let port = port();
        http::start(port, 1024).unwrap();
        let mut socket = client(port);
        socket
            .write_all(b"GET /rust-binary HTTP/1.1\r\nHost: localhost\r\n\r\n")
            .unwrap();
        let mut response = Vec::new();
        socket.read_to_end(&mut response).unwrap();
        http::stop();
        assert!(response.starts_with(b"HTTP/1.1 200"));
        let header_end = response.windows(4).position(|s| s == b"\r\n\r\n").unwrap() + 4;
        let header = std::str::from_utf8(&response[..header_end]).unwrap();
        assert!(header.contains("Content-Type: application/json\r\nContent-Length: 3\r\n"));
        assert_eq!(&response[header_end..], b"A\0B");
    }

    #[test]
    fn rust_websocket_paths_logs_and_binary_broadcast() {
        let path = String::from("/rust-wssuffix");
        http::ws::route(&path[..8], None, None).unwrap();
        drop(path);
        assert_eq!(
            http::ws::route("/nul\0tail", None, None),
            Err(Error::InvalidParam)
        );
        let port = port();
        http::start(port, 1024).unwrap();
        let mut socket = client(port);
        socket.write_all(b"GET /rust-ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n").unwrap();
        let mut header = Vec::new();
        while !header.ends_with(b"\r\n\r\n") {
            let mut b = [0];
            socket.read_exact(&mut b).unwrap();
            header.push(b[0]);
        }
        assert!(header.starts_with(b"HTTP/1.1 101"));
        assert_eq!(
            http::ws::broadcast("/rust-ws\0tail", b"bad"),
            Err(Error::InvalidParam)
        );
        assert_eq!(http::ws::broadcast("", b"bad"), Ok(0));
        assert_eq!(
            http::ws::broadcast(&String::from("/rust-wssuffix")[..8], b"A\0B"),
            Ok(1)
        );
        let mut frame = [0; 5];
        socket.read_exact(&mut frame).unwrap();
        assert_eq!(&frame[1..], b"\x03A\0B");
        assert_eq!(http::ws::broadcast_all(b"C\0D"), Ok(1));
        socket.read_exact(&mut frame).unwrap();
        assert_eq!(&frame[1..], b"\x03C\0D");
        let mut log = "L".repeat(2048);
        log.push('\0');
        assert_eq!(http::log_append(&log), Err(Error::InvalidParam));
        assert_eq!(http::log_append(&log[..2048]), Ok(()));
        drop(socket);
        http::stop();
    }

    #[test]
    fn file_nvs_uses_configured_prefix_limit() {
        nvs::init().unwrap();
        let max = 96 - "/tmp/ove-nvs-rust-tests".len() - 2;
        assert_eq!(nvs::key_max_len(), max);
        let prefix = format!("p{}", std::process::id());
        let key = format!("{prefix}{}", "k".repeat(max - prefix.len()));
        nvs::write_fmt(format_args!("{key}"), b"A\0B").unwrap();
        let mut bytes = [0; 2];
        assert_eq!(nvs::read(&key, &mut bytes), Ok(3));
        assert_eq!(&bytes, b"A\0");
        assert_eq!(nvs::read(&key, &mut []), Ok(3));
        assert_eq!(
            nvs::write_fmt(format_args!("{key}x"), b"bad"),
            Err(Error::InvalidParam)
        );
        assert_eq!(
            nvs::write(&(key.clone() + "\0tail"), b"bad"),
            Err(Error::InvalidParam)
        );
        let mut complete = [0; 3];
        assert_eq!(nvs::read(&key, &mut complete), Ok(3));
        assert_eq!(&complete, b"A\0B");
        nvs::erase(&key).unwrap();
        nvs::erase(&key).unwrap();
    }

    #[test]
    fn tls_hostname_is_private_terminated_and_copied() {
        #[cfg(not(zero_heap))]
        let session = net_tls::Session::new().unwrap();
        #[cfg(zero_heap)]
        let mut storage = std::mem::MaybeUninit::<ffi::ove_tls_storage_t>::uninit();
        #[cfg(zero_heap)]
        // SAFETY: storage lives until after the session drops and has one owner.
        let session = unsafe { net_tls::Session::from_static(storage.as_mut_ptr()).unwrap() };
        let port = unsafe { ove_test_tls_start() };
        assert!(port > 0);
        let socket = ove::net::TcpStream::new().unwrap();
        socket
            .connect(
                &ove::net::Address::ipv4(127, 0, 0, 1, port as u16),
                Duration::from_secs(5),
            )
            .unwrap();
        let mut ca = include_bytes!("../../net_tls/pki/ca.pem").to_vec();
        ca.push(0);
        for hostname in ["localhost\0tail".to_owned(), "h".repeat(256)] {
            let expected = if hostname.contains('\0') {
                Error::InvalidParam
            } else {
                Error::NameTooLong
            };
            assert_eq!(
                session.handshake(
                    &socket,
                    &net_tls::TlsConfig {
                        ca_cert: Some(&ca),
                        hostname: Some(&hostname)
                    }
                ),
                Err(expected)
            );
        }
        // Native fail-closed policy distinguishes valid staging from overflow,
        // without beginning a handshake or consuming the server connection.
        assert_eq!(
            session.handshake(
                &socket,
                &net_tls::TlsConfig {
                    ca_cert: None,
                    hostname: Some(&"h".repeat(255)),
                }
            ),
            Err(Error::InvalidParam)
        );
        assert_eq!(
            session.handshake(
                &socket,
                &net_tls::TlsConfig {
                    ca_cert: Some(&ca),
                    hostname: None,
                }
            ),
            Err(Error::InvalidParam)
        );
        let hostname = String::from("localhostsuffix");
        session
            .handshake(
                &socket,
                &net_tls::TlsConfig {
                    ca_cert: Some(&ca),
                    hostname: Some(&hostname[..9]),
                },
            )
            .unwrap();
        drop(hostname);
        drop(ca);
        let overwrite = [b'x'; 1024];
        std::hint::black_box(overwrite);
        assert_eq!(session.send(b"A\0B").unwrap(), 3);
        let mut bytes = [0; 3];
        let mut n = 0;
        while n < 3 {
            let got = session.recv(&mut bytes[n..]).unwrap();
            assert!(got > 0);
            n += got;
        }
        assert_eq!(&bytes, b"B\0A");
        session.close();
        drop(session);
        drop(socket);
        assert_eq!(unsafe { ove_test_tls_finish() }, 0);
    }
}
