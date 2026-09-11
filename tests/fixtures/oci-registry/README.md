# OCI registry TLS fixture

These public test credentials exist only for the loopback OCI registry gate.
They are intentionally static so the gate tests Warden and Mbed TLS against
identical DER bytes on every supported host instead of inheriting the local
`openssl` command's certificate-generation defaults. The server certificate
is valid for `localhost` from 2026-09-01 through 2046-08-27.
