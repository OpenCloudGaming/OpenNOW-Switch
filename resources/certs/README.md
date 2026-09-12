# Supplemental TLS root

`DigiCertGlobalRootG3.pem` is the unmodified public root downloaded from
https://cacerts.digicert.com/DigiCertGlobalRootG3.crt.pem. Its DER SHA-256 fingerprint is
`31AD6648F8104138C738F39EA4320133393E3A18CC02296EF97C2AC9EF6731D0`.

On 2026-09-12, the `static.nvidiagrid.net` catalog certificate chained through
`DigiCert Global G3 TLS ECC SHA384 2020 CA1` to this root. The PEM was also checked
against Ubuntu's Mozilla-derived trust store.

Switch curl's libnx backend imports this root through `CURLOPT_CAINFO` in addition
to the system trust store. Peer, hostname and certificate-date verification stay
enabled. Horizon includes this root starting with version 11.0.0, so the resource
provides compatibility for older trust stores; it does not establish the cause of
certificate failures on current firmware. A wrong system clock or a different
server chain still requires diagnosis on the console.

Package this directory at `romfs:/certs`. Do not replace this root with a server
certificate or an unverified download. When updating it, verify its fingerprint
against the issuing CA and a maintained public trust store.
