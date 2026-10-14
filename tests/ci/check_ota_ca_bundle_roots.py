#!/usr/bin/env python3
"""Fail if the embedded certificate bundle does not match what the docs promise.

THE CLAIM THIS EXISTS TO POLICE

EZPI_OTA_CA_BUNDLE tells the OTA client to verify the firmware server against
the root certificates ESP-IDF embeds, instead of the single pinned chain in
ca_mios.h. The selling point is that firmware can then be self-hosted behind
an ordinary public certificate, with no certificate handling in the firmware.

The Kconfig help text for that option used to say "a GitHub release asset or
anything behind Let's Encrypt is enough". Half of that was wrong, and not in a
subtle way.

ESP-IDF has two root sets, and the four committed sdkconfig.defaults.* files do
not agree on which one to use -- CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN
on esp32, CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL on esp32c3, esp32s3
and esp32s2. Both lines date from the same upstream commit, so this is drift
rather than a decision. Reading the generated bundle out of a real build:

    esp32   (CMN)    36 roots, 14746 bytes, no ISRG Root X1 and no ISRG Root X2
    esp32c3 (FULL)  136 roots, 63739 bytes, both present

Let's Encrypt has issued from ISRG Root X1 since the DST Root CA X3 cross-sign
ended in 2021. DST Root CA X3 is still in the CMN set, which is exactly the
trap: the root is there, it is expired, and it signs nothing current. So a
Let's Encrypt firmware host verifies on esp32c3, esp32s3 and esp32s2 and fails
TLS on esp32 -- same opt-in configuration, same code, different answer per
target. GitHub release assets chain to DigiCert, which is in both sets, so
that half of the sentence was true and the other half was not.

WHY A CHECK AND NOT JUST A CORRECTED SENTENCE

The corrected sentence is a statement about the contents of a file ESP-IDF
generates at build time from its own CSV and PEM, neither of which this
repository controls. Bumping the IDF pin can change it. Nothing in a build
would notice: the bundle is generated, embedded and linked whatever is in it,
esp_crt_bundle_attach returns ESP_OK regardless, and the first sign of trouble
is a hub in the field failing an update with a TLS error.

So this reads the bundle the build actually embedded and checks it against the
root set the documentation promises for that variant. Both directions matter.
A root the docs tell users to rely on going missing is the obvious failure. A
root the docs warn is absent turning up is also a failure, because then the
warning is wrong and self-hosters are being told to avoid something that would
work.

HOW IT READS THE BUNDLE

gen_crt_bundle.py writes a big-endian count followed by, per certificate, a
2+2 byte (name length, key length) header, the DER of the subject name, then
the DER of the public key. The subject CN sits in the name DER as plain ASCII,
which is all this needs -- it never has to parse the key. Searching the whole
blob for a CN string would also have worked, but would match a CN appearing
inside key bytes by chance, so the structure is walked properly.

LIMITS

Presence of a root is not proof a given host verifies against it: the server
still has to send a correct chain, and the leaf still has to be in date. This
answers the narrower question the docs actually assert -- whether the root is
in the image at all -- which is the half that silently changes under us.

Run from the repository root, after a build, with the build directory as the
first argument (default "build").
"""
import os
import struct
import sys

# What the documentation promises per bundle variant. Each entry maps a
# subject CN to whether it must be present. False entries are as load-bearing
# as True ones: they are the roots the docs warn self-hosters away from.
EXPECTED = {
    'CMN': {
        # Named in the Kconfig help as what a self-hosted firmware server on
        # esp32 can present. GitHub release assets chain to DigiCert.
        'DigiCert Global Root CA': True,
        'DigiCert Global Root G2': True,
        'USERTrust RSA Certification Authority': True,
        'Amazon Root CA 1': True,
        'GlobalSign Root CA': True,
        'GTS Root R1': True,
        # The documented gap. If these ever appear, the Kconfig help and the
        # README both become wrong and have to be updated.
        'ISRG Root X1': False,
        'ISRG Root X2': False,
    },
    'FULL': {
        'DigiCert Global Root CA': True,
        'DigiCert Global Root G2': True,
        'USERTrust RSA Certification Authority': True,
        'Amazon Root CA 1': True,
        'GlobalSign Root CA': True,
        'GTS Root R1': True,
        # The whole point of FULL for a self-hoster.
        'ISRG Root X1': True,
        'ISRG Root X2': True,
    },
}

VARIANT_SYMBOL = {
    'CMN': 'CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN',
    'FULL': 'CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL',
}


def parse_bundle(blob):
    """Return the list of subject-name blobs, or raise ValueError."""
    if len(blob) < 2:
        raise ValueError('bundle is %d bytes, too short to hold a count'
                         % len(blob))
    count = struct.unpack('>H', blob[:2])[0]
    if count == 0:
        raise ValueError('bundle declares zero certificates')

    names = []
    off = 2
    for i in range(count):
        if off + 4 > len(blob):
            raise ValueError('ran off the end at certificate %d of %d'
                             % (i + 1, count))
        name_len, key_len = struct.unpack('>HH', blob[off:off + 4])
        off += 4
        if off + name_len + key_len > len(blob):
            raise ValueError('certificate %d of %d claims %d+%d bytes but only '
                             '%d remain' % (i + 1, count, name_len, key_len,
                                            len(blob) - off))
        names.append(blob[off:off + name_len])
        off += name_len + key_len

    if off != len(blob):
        raise ValueError('%d trailing bytes after %d certificates'
                         % (len(blob) - off, count))
    return names


def subject_text(name_der):
    """The printable characters of a subject-name DER, for CN matching."""
    return ''.join(chr(b) if 32 <= b < 127 else '\n' for b in name_der)


def roots_present(names):
    return [subject_text(n) for n in names]


def check(subjects, variant):
    """Return (ok, [message]) comparing the bundle against EXPECTED."""
    problems = []
    for cn, must_be_present in sorted(EXPECTED[variant].items()):
        found = any(cn in s for s in subjects)
        if must_be_present and not found:
            problems.append(
                '%r is absent, but the %s bundle is documented as containing '
                'it. Either ESP-IDF dropped it or the IDF pin moved; a '
                'self-hosted firmware server using a certificate from this '
                'root will now fail TLS on this target.' % (cn, variant))
        elif not must_be_present and found:
            problems.append(
                '%r is present, but the %s bundle is documented as NOT having '
                'it -- the Kconfig help for EZPI_OTA_CA_BUNDLE and the README '
                'both warn self-hosters away from it on this target. The '
                'warning is now wrong. Update both, then update EXPECTED '
                'here.' % (cn, variant))
    return (not problems), problems


def detect_variant(sdkconfig_path):
    try:
        with open(sdkconfig_path, encoding='utf-8', errors='replace') as handle:
            text = handle.read()
    except OSError as exc:
        return None, 'cannot read %s: %s' % (sdkconfig_path, exc)

    selected = [v for v, sym in VARIANT_SYMBOL.items()
                if '%s=y' % sym in text]
    if len(selected) == 1:
        return selected[0], None
    if not selected:
        return None, ('neither %s nor %s is set in %s, so there is no '
                      'documented root set to check against'
                      % (VARIANT_SYMBOL['CMN'], VARIANT_SYMBOL['FULL'],
                         sdkconfig_path))
    return None, ('both bundle variants appear set in %s: %s'
                  % (sdkconfig_path, ', '.join(selected)))


def _fake_bundle(cns):
    """Build a well-formed bundle whose subject names contain these CNs."""
    out = struct.pack('>H', len(cns))
    for cn in cns:
        # A plausible subject-name DER is unnecessary; the CN only has to sit
        # in the name field as ASCII, which is what the real DER does.
        name = (b'\x30\x13\x31\x11\x30\x0f\x06\x03\x55\x04\x03\x0c'
                + bytes([len(cn)]) + cn.encode())
        key = b'\x00' * 8
        out += struct.pack('>HH', len(name), len(key)) + name + key
    return out


COMMERCIAL = ['DigiCert Global Root CA', 'DigiCert Global Root G2',
              'USERTrust RSA Certification Authority', 'Amazon Root CA 1',
              'GlobalSign Root CA', 'GTS Root R1']


def self_test():
    """Prove the comparison can fail, in both directions, before trusting it."""
    cases = [
        # The two configurations as they actually are today.
        ('cmn_as_shipped', COMMERCIAL, 'CMN', True),
        ('full_as_shipped', COMMERCIAL + ['ISRG Root X1', 'ISRG Root X2'],
         'FULL', True),
        # A promised root going missing.
        ('cmn_lost_digicert',
         [c for c in COMMERCIAL if c != 'DigiCert Global Root CA'],
         'CMN', False),
        ('full_lost_isrg', COMMERCIAL + ['ISRG Root X2'], 'FULL', False),
        # A root the docs warn is absent turning up. This is the direction an
        # earlier draft got wrong by only checking for missing roots, which
        # would have let the esp32 Let's Encrypt warning rot into a lie.
        ('cmn_gained_isrg', COMMERCIAL + ['ISRG Root X1'], 'CMN', False),
        # An empty bundle must not read as "nothing required is missing".
        ('empty', [], 'CMN', False),
    ]

    ok = True
    for name, cns, variant, expected_ok in cases:
        if cns:
            subjects = roots_present(parse_bundle(_fake_bundle(cns)))
        else:
            subjects = []
        got_ok, _ = check(subjects, variant)
        if got_ok != expected_ok:
            print('  self-test %-20s FAILED: expected ok=%s, got ok=%s'
                  % (name, expected_ok, got_ok))
            ok = False
        else:
            print('  self-test %-20s ok' % name)

    # The parser must reject malformed input rather than silently returning a
    # short list, which would look like "roots are missing" for the wrong
    # reason -- or worse, like a pass.
    for name, blob in [('truncated_header', b'\x00'),
                       ('zero_count', struct.pack('>H', 0)),
                       ('length_overrun',
                        struct.pack('>H', 1) + struct.pack('>HH', 999, 0)),
                       ('trailing_bytes', _fake_bundle(['X']) + b'\x00\x00')]:
        try:
            parse_bundle(blob)
        except ValueError:
            print('  self-test %-20s ok' % name)
        else:
            print('  self-test %-20s FAILED: malformed bundle accepted' % name)
            ok = False

    return ok


def main():
    args = sys.argv[1:]

    # --self-test exercises the comparison against synthetic bundles only, so
    # the checker can be verified without first doing a firmware build. The
    # self-test also runs unconditionally below, because a checker that has
    # not been shown to fail is not evidence of anything.
    only_self_test = '--self-test' in args
    args = [arg for arg in args if arg != '--self-test']

    build_dir = args[0] if args else 'build'
    bundle_path = os.path.join(build_dir, 'esp-idf', 'mbedtls',
                               'x509_crt_bundle')
    sdkconfig_path = 'sdkconfig'

    print('self-test:')
    if not self_test():
        print('\nThe comparison is wrong, so its verdict on the real bundle '
              'means nothing. Fix the checker.')
        return 2

    if only_self_test:
        print('\nself-test only; the embedded bundle was not read.')
        return 0

    variant, err = detect_variant(sdkconfig_path)
    if err:
        print('\n::error::%s' % err)
        return 1

    try:
        with open(bundle_path, 'rb') as handle:
            blob = handle.read()
    except OSError as exc:
        print('\n::error::cannot read %s: %s' % (bundle_path, exc))
        print('::error::This runs after a build; the bundle is generated by '
              'the mbedtls component.')
        return 1

    try:
        names = parse_bundle(blob)
    except ValueError as exc:
        print('\n::error file=%s::%s' % (bundle_path, exc))
        return 1

    subjects = roots_present(names)
    print('\n%s' % bundle_path)
    print('  variant %s, %d roots, %d bytes' % (variant, len(names), len(blob)))

    ok, problems = check(subjects, variant)
    if ok:
        print('  matches the %d documented expectations for %s'
              % (len(EXPECTED[variant]), variant))
        return 0

    print('')
    for problem in problems:
        print('::error file=%s::%s' % (bundle_path, problem))
    print('\n%d mismatch(es) between the embedded bundle and the docs.'
          % len(problems))
    return 1


if __name__ == '__main__':
    sys.exit(main())
