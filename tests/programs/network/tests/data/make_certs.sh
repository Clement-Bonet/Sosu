#!/bin/sh
# The certificates of the TLS tests of tests/programs/network, made once with OpenSSL and
# committed: a test authority (ca.pem), a server for localhost and 127.0.0.1 that it signs
# (server.pem, server.key), the same server out of its dates (expired.pem), and a server for
# another name (other.pem). All ECDSA P-256; the keys are for these tests only.
# Run from this folder: sh make_certs.sh
set -e
openssl ecparam -name prime256v1 -genkey -noout -out ca.key
openssl req -x509 -new -key ca.key -sha256 -days 36500 -subj "/CN=Sosu test authority" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" -out ca.pem
openssl ecparam -name prime256v1 -genkey -noout -out server.key
openssl req -new -key server.key -subj "/CN=localhost" -out server.csr
cat > server.ext <<EOF
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1
EOF
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial -sha256 -days 36500 -extfile server.ext \
    -out server.pem
# out of its dates: signed by the authority, valid in 2001 only
mkdir -p demo/newcerts
: > demo/index.txt
echo 01 > demo/serial
cat > demo/ca.cnf <<EOF
[ca]
default_ca = test
[test]
dir = demo
database = demo/index.txt
new_certs_dir = demo/newcerts
serial = demo/serial
default_md = sha256
policy = anything
copy_extensions = none
[anything]
commonName = supplied
EOF
openssl ca -batch -config demo/ca.cnf -cert ca.pem -keyfile ca.key -in server.csr -startdate 20010101000000Z \
    -enddate 20011231000000Z -extfile server.ext -notext -out expired.pem
# for another name
openssl req -new -key server.key -subj "/CN=other.example" -out other.csr
sed 's/DNS:localhost,IP:127.0.0.1/DNS:other.example/' server.ext > other.ext
openssl x509 -req -in other.csr -CA ca.pem -CAkey ca.key -CAcreateserial -sha256 -days 36500 -extfile other.ext \
    -out other.pem
rm -rf demo server.csr other.csr server.ext other.ext ca.srl ca.key
