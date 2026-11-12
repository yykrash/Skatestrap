"""APK Signature Scheme v2 check: the signed content digest matches the file, and the signer's key
signed it with the certificate's own key. Usage: python -I apk_verify.py <apk>"""
import hashlib, struct, sys
from cryptography import x509
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec, padding

data = open(sys.argv[1], 'rb').read()
eocd = data.rfind(b'PK\x05\x06')
cd = struct.unpack_from('<I', data, eocd + 16)[0]
size = struct.unpack_from('<Q', data, cd - 24)[0]
block_start = cd - size - 8


def lp(buf, p):
    n = struct.unpack_from('<I', buf, p)[0]
    return buf[p + 4:p + 4 + n], p + 4 + n


pos, v2 = block_start + 8, None
while pos < cd - 24:
    length, ident = struct.unpack_from('<QI', data, pos)
    if ident == 0x7109871a:
        v2 = data[pos + 12:pos + 8 + length]
    pos += 8 + length
assert v2 is not None, 'no v2 block'
signers, _ = lp(v2, 0)
signer, _ = lp(signers, 0)
signed, p = lp(signer, 0)
sigs, p = lp(signer, p)
pub, _ = lp(signer, p)

digests, q = lp(signed, 0)
certs, _ = lp(signed, q)
cert = x509.load_der_x509_certificate(lp(certs, 0)[0])
d, _ = lp(digests, 0)
algo = struct.unpack_from('<I', d, 0)[0]
want, _ = lp(d, 4)

sig, _ = lp(sigs, 0)
sig_algo = struct.unpack_from('<I', sig, 0)[0]
signature, _ = lp(sig, 4)
key = cert.public_key()
h = hashes.SHA512() if sig_algo in (0x0102, 0x0104, 0x0202) else hashes.SHA256()
if sig_algo in (0x0103, 0x0104):
    key.verify(signature, signed, padding.PKCS1v15(), h)
elif sig_algo in (0x0101, 0x0102):
    key.verify(signature, signed, padding.PSS(padding.MGF1(h), h.digest_size), h)
else:
    key.verify(signature, signed, ec.ECDSA(h))
print('signature over signed data: OK (algo %#x)' % sig_algo)

H = hashlib.sha512 if algo in (0x0102, 0x0104, 0x0202) else hashlib.sha256
eocd_mod = bytearray(data[eocd:])
struct.pack_into('<I', eocd_mod, 16, block_start)
chunks = []
for section in (memoryview(data)[:block_start], memoryview(data)[cd:eocd], memoryview(bytes(eocd_mod))):
    for i in range(0, len(section), 1 << 20):
        c = section[i:i + (1 << 20)]
        chunks.append(H(b'\xa5' + struct.pack('<I', len(c)) + c).digest())
got = H(b'\x5a' + struct.pack('<I', len(chunks)) + b''.join(chunks)).digest()
assert got == want, 'CONTENT DIGEST MISMATCH: file was changed after signing'
print('content digest over whole APK: OK (%d chunks)' % len(chunks))
print('cert sha256', hashlib.sha256(cert.public_bytes(__import__('cryptography.hazmat.primitives.serialization', fromlist=['Encoding']).Encoding.DER)).hexdigest())
