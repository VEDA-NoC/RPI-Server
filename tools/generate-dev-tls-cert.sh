#!/usr/bin/env bash
set -euo pipefail

cert_dir="${RPI_VMS_TLS_DIR:-certs}"
tls_cert="${RPI_VMS_TLS_CERT:-$cert_dir/server.crt}"
tls_key="${RPI_VMS_TLS_KEY:-$cert_dir/server.key}"
days="${RPI_VMS_TLS_DAYS:-365}"

command -v openssl >/dev/null || {
    echo 'FAIL: required command not found: openssl' >&2
    exit 1
}
[[ "$days" =~ ^[1-9][0-9]*$ ]] || {
    echo 'FAIL: RPI_VMS_TLS_DAYS must be a positive integer' >&2
    exit 1
}
if [[ -e "$tls_cert" || -e "$tls_key" ]]; then
    if [[ -r "$tls_cert" && -r "$tls_key" ]]; then
        echo "INFO: TLS certificate already exists; refusing to overwrite"
        echo "Certificate: $tls_cert"
        echo "Private key: $tls_key"
        openssl x509 -in "$tls_cert" -noout -subject -dates -fingerprint -sha256
        exit 0
    fi
    echo "FAIL: only one TLS file exists; repair explicitly without overwriting the other" >&2
    echo "Certificate: $tls_cert" >&2
    echo "Private key: $tls_key" >&2
    exit 1
fi

mkdir -p -- "$(dirname -- "$tls_cert")" "$(dirname -- "$tls_key")"
hostname_short="$(hostname -s)"
subject_alt_name="DNS:${hostname_short},DNS:localhost,IP:127.0.0.1"
while read -r address; do
    [[ -n "$address" ]] && subject_alt_name+=",IP:${address}"
done < <(hostname -I | tr ' ' '\n' | sed '/^$/d')
if command -v tailscale >/dev/null; then
    while read -r address; do
        [[ -n "$address" ]] && subject_alt_name+=",IP:${address}"
    done < <(tailscale ip -4 2>/dev/null || true)
fi

openssl req -x509 -newkey rsa:3072 -sha256 -nodes -days "$days" \
    -subj "/CN=${hostname_short}" \
    -addext "subjectAltName=${subject_alt_name}" \
    -addext 'keyUsage=critical,digitalSignature,keyEncipherment' \
    -addext 'extendedKeyUsage=serverAuth' \
    -keyout "$tls_key" \
    -out "$tls_cert" \
    >/dev/null 2>&1
chmod 600 "$tls_key"
chmod 644 "$tls_cert"

echo 'PASS: persistent development TLS certificate created'
echo "Certificate: $tls_cert"
echo "Private key: $tls_key"
openssl x509 -in "$tls_cert" -noout -subject -dates -fingerprint -sha256
echo 'NOTE: this is a self-signed development certificate; replace it with the deployment certificate when available.'
