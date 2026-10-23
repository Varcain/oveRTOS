#!/bin/sh
# Regenerates the test-only PKI used by tests/net_tls: an EC P-256 CA, a
# server certificate for localhost and a client certificate for mutual TLS.
# Valid for 100 years so the committed files do not expire under CI.
set -e
cd "$(dirname "$0")"
DAYS=36500
openssl ecparam -name prime256v1 -genkey -noout -out ca.key
openssl req -new -x509 -key ca.key -sha256 -days $DAYS -subj "/CN=ove-test-ca" -out ca.pem
for who in srv cli; do
	openssl ecparam -name prime256v1 -genkey -noout -out $who.key
done
openssl ecparam -name prime256v1 -genkey -noout -out other.key
openssl req -new -key srv.key -subj "/CN=localhost" -out srv.csr
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n' > srv.ext
openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial -sha256 -days $DAYS \
	-extfile srv.ext -out srv.pem
openssl req -new -key cli.key -subj "/CN=ove-client" -out cli.csr
printf 'extendedKeyUsage=clientAuth\n' > cli.ext
openssl x509 -req -in cli.csr -CA ca.pem -CAkey ca.key -CAcreateserial -sha256 -days $DAYS \
	-extfile cli.ext -out cli.pem
rm -f srv.csr cli.csr srv.ext cli.ext ca.srl ca.key
