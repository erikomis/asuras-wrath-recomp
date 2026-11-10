#!/usr/bin/env python3
"""Fetch the keys that decrypt this game's executables, from RPCS3's public source.

    tools/fetch_keys.py                 # write the key file if it is not there yet
    tools/fetch_keys.py --force         # download and write it again
    tools/fetch_keys.py --source FILE   # use a key_vault.cpp you already have

No key is in this repository. The executables are SELFs encrypted with the
PS3's application loader keys, and those keys are published in RPCS3's source
code, rpcs3/Crypto/key_vault.cpp. This downloads that one file from GitHub at
a pinned commit, checks its SHA-256 (so the file is exactly the one this was
tested with, whatever the network or TLS in between), and writes the entries
in scetool's key-file format for tools/decrypt_self.py:

  program type APP, every revision    (KeyVault::LoadSelfAPPKeys)
  program type NPDRM, every revision  (KeyVault::LoadSelfNPDRMKeys)

Asura's Wrath (BLUS30721) needs APP revision 0x0016 for the disc's EBOOT.BIN
(disc version 01.02) and NPDRM revision 0x0010 for the 1.02 update package's
EBOOT.BIN (licence type 3, free). A disc from another region or another
update would need different revisions; every one is written, so those work
too.

The key file goes to a per-user folder outside the repository, readable only
by you (mode 600):

  macOS  ~/Library/Application Support/asuras-wrath-recomp/keys/self_keys.txt
  other  $XDG_DATA_HOME (~/.local/share)/asuras-wrath-recomp/keys/self_keys.txt

--out or ASURA_SELF_KEYS put it elsewhere. The path is printed on stdout (and
nothing else); progress and errors go to stderr. No key is ever printed.

Adapted from the Demons' Souls port's fetch_keys.py (same author, MIT), which
took it from the author's earlier ports. Only the standard library is used,
so it runs before the toolkit's Python packages are installed.
"""
import argparse
import hashlib
import os
import re
import ssl
import sys
import urllib.error
import urllib.request

# The last RPCS3 commit that changed key_vault.cpp when the Demons' Souls port
# wrote its fetch_keys.py ("Use std::string_view in crypto code", 2026-06-16),
# and the SHA-256 of the file at that commit. A hash is not a key: it only
# says which file is right.
RPCS3_COMMIT = 'b8ca6d953f734900c23f2cfbe79a36e598171553'
KEY_VAULT_SHA256 = 'a2947ff6b681258c198552ec6344e16685e2df649018206daa3f41a0a1aadbd7'
KEY_VAULT_URL = ('https://raw.githubusercontent.com/RPCS3/rpcs3/%s/rpcs3/Crypto/key_vault.cpp'
                 % RPCS3_COMMIT)
KEY_HEADER_URL = KEY_VAULT_URL.removesuffix('.cpp') + '.h'
KEY_HEADER_SHA256 = 'd5d0cede74c7900bb23ca8d60066194648ed4b976eaee60315c0bcbed7dc2aaf'
RAP_KEY_SHA256 = 'ae5fcb75a631aa3e1e7a514c1eb42cb90e7f0ff83743f8769e6de7be64ff94db'

# What this game's executables use: the disc's EBOOT.BIN (APP, rev 0x16 for
# the 01.02 disc) and the 1.02 update package's EBOOT.BIN (NPDRM, rev 0x10,
# free licence). Both whole tables are written, so these must be among them.
NEEDED_APP_REVISIONS = (0x0016,)
NEEDED_NPDRM_REVISIONS = (0x0010,)
# Named keys the NPDRM path needs: the free klicensee (licence type 3) and the
# RAP unwrapping key (licence types 1-2, for a bought .rap, if there is ever
# one), plus the debug one for completeness.
NAMED_KEYS = {'RAP_KEY': 'RAP_KEY', 'NP_klic_key': 'NP_KLIC_KEY',
              'NP_klic_free': 'NP_KLIC_FREE'}


def default_path():
    if os.environ.get('ASURA_SELF_KEYS'):
        return os.environ['ASURA_SELF_KEYS']
    home = os.path.expanduser('~')
    if os.name == 'nt':   # Windows: %LOCALAPPDATA%\asuras-wrath-recomp\keys
        base = os.environ.get('LOCALAPPDATA') or os.path.join(home, 'AppData', 'Local')
    elif sys.platform == 'darwin':
        base = os.path.join(home, 'Library', 'Application Support')
    else:
        base = os.environ.get('XDG_DATA_HOME') or os.path.join(home, '.local', 'share')
    return os.path.join(base, 'asuras-wrath-recomp', 'keys', 'self_keys.txt')


def log(msg):
    print(msg, file=sys.stderr)


class FetchError(Exception):
    pass


def download(url, timeout):
    req = urllib.request.Request(url, headers={'User-Agent': 'asuras-wrath-recomp fetch_keys.py'})

    def get(ctx):
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
            return r.read()
    try:
        try:
            return get(ssl.create_default_context())
        except urllib.error.URLError as e:
            if not isinstance(e.reason, ssl.SSLCertVerificationError):
                raise
            # The file is checked by its pinned SHA-256 below, so a connection
            # that cannot be verified still cannot hand us a different file.
            log('  (this Python cannot verify GitHub\'s certificate; the file is checked by its SHA-256 instead)')
            return get(ssl._create_unverified_context())
    except (urllib.error.URLError, OSError) as e:
        reason = getattr(e, 'reason', e)
        raise FetchError(
            'could not download the decryption keys from GitHub (%s).\n'
            'Check that this computer is connected to the internet and try again.\n'
            'They come from: %s' % (reason, url))


def _parse_self_keys(text, loader, needed, self_type):
    """[(revision, erk hex, riv hex)] from one KeyVault::LoadSelf*Keys body, in
    file order (RPCS3 takes the first entry of a revision; so does decrypt_self.py)."""
    m = re.search(r'void\s+%s\s*\(\s*\)\s*\{(.*?)\n\}' % loader, text, re.S)
    if not m:
        raise FetchError('key_vault.cpp has no %s' % loader)
    entry = re.compile(
        r'\.emplace_back\(\s*0x[0-9A-Fa-f]+\s*,\s*0x[0-9A-Fa-f]+\s*,\s*0x([0-9A-Fa-f]+)\s*,'
        r'\s*KEY_%s\s*,\s*"([0-9A-Fa-f]*)"\s*,\s*"([0-9A-Fa-f]*)"' % self_type)
    keys, seen = [], set()
    for rev, erk, riv in entry.findall(m.group(1)):
        rev = int(rev, 16)
        if len(erk) != 64 or len(riv) != 32 or rev in seen:
            continue
        seen.add(rev)
        keys.append((rev, erk.upper(), riv.upper()))
    missing = [r for r in needed if r not in seen]
    if missing:
        raise FetchError('key_vault.cpp has no %s key for revision %s'
                         % (self_type, ', '.join('0x%04X' % r for r in missing)))
    return keys


def parse_app_keys(text):
    return _parse_self_keys(text, 'KeyVault::LoadSelfAPPKeys',
                            NEEDED_APP_REVISIONS, 'APP')


def parse_npdrm_keys(text):
    return _parse_self_keys(text, 'KeyVault::LoadSelfNPDRMKeys',
                            NEEDED_NPDRM_REVISIONS, 'NPDRM')


def parse_named_keys(text):
    keys = {}
    for name, symbol in NAMED_KEYS.items():
        m = re.search(r'\b' + symbol + r'\s*\[[^]]+\]\s*=\s*\{([^}]+)\}', text)
        if not m:
            raise FetchError('key_vault.h has no %s' % symbol)
        values = re.findall(r'0x([0-9A-Fa-f]{2})\b', m.group(1))
        if len(values) != 16:
            raise FetchError('key_vault.h has an invalid %s size' % symbol)
        keys[name] = ''.join(values).upper()
    if hashlib.sha256(bytes.fromhex(keys['RAP_KEY'])).hexdigest() != RAP_KEY_SHA256:
        raise FetchError('RAP_KEY hash does not match')
    return keys


def key_file_text(app_keys, npdrm_keys, named, source):
    out = ['# SELF appldr keys (program types APP and NPDRM), in scetool\'s key-file',
           '# format, for tools/decrypt_self.py. Written by tools/fetch_keys.py from',
           '# %s' % source,
           '# Do not share this file or put it in a repository.', '']
    out += ['%s=%s' % (name, value) for name, value in named.items()] + ['']
    out += ['# Program type APP: the disc\'s EBOOT.BIN (Asura\'s Wrath BLUS30721, rev 0x0016).']
    for rev, erk, riv in app_keys:
        out += ['[appldr_%04X]' % rev, 'type=SELF', 'revision=%04X' % rev, 'self_type=APP',
                'erk=%s' % erk, 'riv=%s' % riv, '']
    out += ['# Program type NPDRM: the update packages\' EBOOT.BIN (rev 0x0010 for the',
            '# 1.02 patch, free licence).']
    for rev, erk, riv in npdrm_keys:
        out += ['[appldr_npdrm_%04X]' % rev, 'type=SELF', 'revision=%04X' % rev, 'self_type=NPDRM',
                'erk=%s' % erk, 'riv=%s' % riv, '']
    return '\n'.join(out)


def write_private(path, text):
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, mode=0o700, exist_ok=True)
    tmp = path + '.tmp'
    try:
        os.unlink(tmp)
    except FileNotFoundError:
        pass
    # Created 600 from the start, so the keys are never readable by others,
    # not even for a moment.
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w', encoding='ascii') as f:
        f.write(text)
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)


def usable(path):
    """True when the key file is there and has everything the game needs:
    both appldr key revisions and the three named keys."""
    try:
        with open(path, encoding='ascii', errors='replace') as f:
            text = f.read()
    except OSError:
        return False
    have = {}
    for block in re.split(r'\n\[', text):
        m_type = re.search(r'self_type\s*=\s*(\w+)', block)
        m_rev = re.search(r'revision\s*=\s*([0-9A-Fa-f]+)', block)
        m_erk = re.search(r'erk\s*=\s*([0-9A-Fa-f]{64})', block)
        m_riv = re.search(r'riv\s*=\s*([0-9A-Fa-f]{32})', block)
        if m_type and m_rev and m_erk and m_riv:
            have[(m_type.group(1), int(m_rev.group(1), 16))] = True
    if not all((('APP', r) in have) for r in NEEDED_APP_REVISIONS):
        return False
    if not all((('NPDRM', r) in have) for r in NEEDED_NPDRM_REVISIONS):
        return False
    for name in NAMED_KEYS:
        if not re.search(r'^%s=([0-9A-Fa-f]{32})\s*$' % re.escape(name), text, re.M):
            return False
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--force', action='store_true',
                    help='download and write the key file again')
    ap.add_argument('--source', metavar='FILE',
                    help='use a key_vault.cpp you already have')
    ap.add_argument('--out', metavar='FILE',
                    help='where to write the keys (default: %s)' % default_path())
    args = ap.parse_args(argv)

    out = args.out or default_path()
    if usable(out) and not args.force:
        print(out)
        log('the key file is already there and has the keys this game needs:')
        log('  %s' % out)
        log('(tools/fetch_keys.py --force writes it again)')
        return 0

    if args.source:
        try:
            with open(args.source, encoding='utf-8', errors='replace') as f:
                text = f.read()
        except OSError as e:
            log('could not read --source: %s' % e)
            return 1
        header = None
    else:
        log('downloading the key list from RPCS3\'s source (two files: key_vault.cpp, key_vault.h)...')
        try:
            data = download(KEY_VAULT_URL, timeout=30)
        except FetchError as e:
            log(str(e))
            return 1
        digest = hashlib.sha256(data).hexdigest()
        if digest != KEY_VAULT_SHA256:
            log('the file from GitHub has the wrong SHA-256 (%s).\n'
                'RPCS3\'s key_vault.cpp changed; pin a new commit in tools/fetch_keys.py.'
                % digest)
            return 1
        text = data.decode('utf-8', 'replace')
        try:
            hdata = download(KEY_HEADER_URL, timeout=30)
        except FetchError as e:
            log(str(e))
            return 1
        if hashlib.sha256(hdata).hexdigest() != KEY_HEADER_SHA256:
            log('key_vault.h from GitHub has the wrong SHA-256.\n'
                'RPCS3\'s key_vault.h changed; pin a new commit in tools/fetch_keys.py.')
            return 1
        header = hdata.decode('utf-8', 'replace')

    try:
        app_keys = parse_app_keys(text)
        npdrm_keys = parse_npdrm_keys(text)
        if header is None:
            # a --source key_vault.cpp: named keys may already be in the text
            header = text
        named = parse_named_keys(header)
    except FetchError as e:
        log(str(e))
        return 1

    write_private(out, key_file_text(app_keys, npdrm_keys, named,
                                     args.source or KEY_VAULT_URL))
    log('wrote %d APP keys, %d NPDRM keys and %d named keys to %s'
        % (len(app_keys), len(npdrm_keys), len(named), out))
    print(out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
