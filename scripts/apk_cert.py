"""Print the APK v2/v3 signing certificate (subject, SHA-256) of an APK, read as plain bytes."""
import hashlib, struct, sys

data = open(sys.argv[1], 'rb').read()
eocd = data.rfind(b'PK\x05\x06')
cd_offset = struct.unpack_from('<I', data, eocd + 16)[0]
assert data[cd_offset - 16:cd_offset] == b'APK Sig Block 42', 'no APK signing block'
size = struct.unpack_from('<Q', data, cd_offset - 24)[0]
start = cd_offset - size - 8
pos, end = start + 8, cd_offset - 24
while pos < end:
    length, ident = struct.unpack_from('<QI', data, pos)
    value = data[pos + 12:pos + 8 + length]
    if ident in (0x7109871a, 0xf05368c0):  # v2, v3
        # signers(len) > signer(len) > signed data(len) > digests(len) ... > certificates(len) > cert(len)
        p = 4 + 4 + 4  # signers seq len, signer len, signed data len
        p += 4 + struct.unpack_from('<I', value, p)[0]  # skip digests
        p += 4  # certificates seq len
        n = struct.unpack_from('<I', value, p)[0]
        cert = value[p + 4:p + 4 + n]
        print('v2' if ident == 0x7109871a else 'v3', 'cert sha256', hashlib.sha256(cert).hexdigest(), 'sha1', hashlib.sha1(cert).hexdigest())
        try:
            from cryptography import x509
            c = x509.load_der_x509_certificate(cert)
            print('  subject', c.subject.rfc4514_string(), '| valid from', c.not_valid_before_utc.date())
        except Exception as e:
            print('  (no cryptography parse:', e, ')')
    pos += 8 + length
